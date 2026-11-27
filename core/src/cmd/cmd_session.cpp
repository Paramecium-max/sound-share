#include "cmd/cmd_session.h"
#include "crypto/mini_crypto.h"
#include "sync/master_clock.h"
#include "util/json.h"

#include <chrono>
#include <cstring>
#ifndef _WIN32
#include <cerrno>
#endif

namespace miplay::cmd {

namespace {
std::string extract_json_field(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}
bool connect_pending() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS || errno == EINTR;
#endif
}
} // namespace

CmdSession::CmdSession(std::string ip, uint16_t port, std::string bind_ip)
    : ip_(std::move(ip)), port_(port), bind_ip_(std::move(bind_ip)) {}

CmdSession::~CmdSession() {
    disconnect();
}

CmdSession::HandshakeStage CmdSession::handshake_stage() const {
    std::lock_guard<std::mutex> lk(auth_mu_);
    return stage_;
}

void CmdSession::emit_status(const std::string& s) {
    if (cbs_.on_status) cbs_.on_status(s);
}

void CmdSession::mark_disconnected(const std::string& reason) {
    bool was_conn = connected_.exchange(false);
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        peer_reset_ = true;
        if (failure_reason_.empty() && !reason.empty()) {
            failure_reason_ = reason;
        }
        if (stage_ != HandshakeStage::ReadySafety && stage_ != HandshakeStage::ReadyLegacy) {
            stage_ = HandshakeStage::Failed;
        }
    }
    auth_cv_.notify_all();
    wake_update_worker();
    if (was_conn) {
        if (!reason.empty()) emit_status(reason);
        if (cbs_.on_state_changed) cbs_.on_state_changed(false);
    }
}

bool CmdSession::connect(CmdSessionCallbacks cbs, const std::atomic<bool>* cancelled) {
    disconnect();
    aborted_.store(false);
    const auto is_cancelled = [this, cancelled] {
        return aborted_.load() || (cancelled && cancelled->load());
    };
    if (is_cancelled()) return false;
    net::init_network();
    cbs_ = std::move(cbs);

    emit_status("8899 TCP 建连中...");
    SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        emit_status("8899 Socket 创建失败");
        return false;
    }

    if (!bind_ip_.empty()) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (inet_pton(AF_INET, bind_ip_.c_str(), &local.sin_addr) != 1 ||
            bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            net::close_socket(s);
            emit_status("8899 本地绑定地址不可用: " + bind_ip_);
            return false;
        }
    }

    net::set_tcp_nodelay(s, true);

    if (!net::set_nonblocking(s, true)) {
        net::close_socket(s);
        emit_status("8899 Socket 配置失败");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(sock_mu_);
        if (is_cancelled()) {
            net::close_socket(s);
            return false;
        }
        sock_ = s;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        {
            std::lock_guard<std::mutex> lk(sock_mu_);
            if (sock_ != kInvalidSocket) { net::close_socket(sock_); sock_ = kInvalidSocket; }
        }
        emit_status("8899 无效目标 IP");
        return false;
    }

    bool tcp_connected = ::connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!tcp_connected && connect_pending()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        while (!is_cancelled() && std::chrono::steady_clock::now() < deadline) {
            if (!net::wait_connect_result(s, 25)) continue;
            int error = 0;
            socklen_t len = sizeof(error);
            tcp_connected = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) == 0 &&
                            error == 0;
            break;
        }
    }
    if (!tcp_connected || is_cancelled()) {
        {
            std::lock_guard<std::mutex> lk(sock_mu_);
            if (sock_ != kInvalidSocket) { net::close_socket(sock_); sock_ = kInvalidSocket; }
        }
        if (!is_cancelled()) emit_status("8899 TCP 连接失败/超时");
        return false;
    }

    struct sockaddr_in local_addr{};
    socklen_t llen = sizeof(local_addr);
    if (getsockname(sock_, reinterpret_cast<struct sockaddr*>(&local_addr), &llen) == 0) {
        char lip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &local_addr.sin_addr, lip, sizeof(lip));
        local_ip_ = lip;
        local_port_ = ntohs(local_addr.sin_port);
    }

    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        stage_ = HandshakeStage::WaitingChallenge;
        legacy_auth_done_ = false;
        version_ack_received_ = false;
        safety_info_ack_received_ = false;
        safety_info_ok_ = false;
        peer_challenge_acked_ = false;
        local_challenge_verified_ = false;
        peer_reset_ = false;
        failure_reason_.clear();
        dec_cipher_locked_ = false;
        safety_active_.store(false);
    }

    {
        std::scoped_lock lock(send_mu_, sock_mu_);
        if (is_cancelled()) {
            net::close_socket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }
        writer_ = std::make_unique<net::ConnectionWriter>(sock_);
        running_.store(true);
        connected_.store(true);
    }
    seq_.store(1);
    if (cbs_.on_state_changed) cbs_.on_state_changed(true);

    recv_th_ = std::thread(&CmdSession::recv_loop, this);

    // 阶段 1：等待接收端主动下发 0x0028 挑战并完成 0x0029 认证
    emit_status("8899 等待 0x0028 挑战认证...");
    {
        std::unique_lock<std::mutex> lk(auth_mu_);
        auth_cv_.wait_for(lk, std::chrono::milliseconds(2000), [this]() {
            return legacy_auth_done_ || !connected_.load();
        });
    }

    if (!connected_.load()) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lk(auth_mu_);
            reason = failure_reason_.empty() ? "8899 挑战认证阶段被接收端断开" : failure_reason_;
        }
        emit_status(reason);
        disconnect();
        return false;
    }

    // 发送 GetVersion（无论 0x0028 是否到达都探活版本；若对端在后台被冻结则不会有任何回复）
    send_get_version("3.1.6030516");

    bool got_any_app_reply = false;
    {
        std::unique_lock<std::mutex> lk(auth_mu_);
        if (!legacy_auth_done_) {
            auth_cv_.wait_for(lk, std::chrono::milliseconds(1000), [this]() {
                return legacy_auth_done_ || version_ack_received_ || !connected_.load();
            });
        }
        got_any_app_reply = (legacy_auth_done_ || version_ack_received_);
    }

    if (!connected_.load() || !got_any_app_reply) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lk(auth_mu_);
            if (!failure_reason_.empty()) reason = failure_reason_;
            else reason = "8899 接收端无应用层响应（请确认接收端在前台运行未休眠）";
            stage_ = HandshakeStage::Failed;
        }
        emit_status(reason);
        disconnect();
        return false;
    }

    // 阶段 2：协商 Modern Safety 安全通道
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        stage_ = HandshakeStage::NegotiatingSafety;
    }
    emit_status("8899 协商 Safety 安全通道...");
    {
        std::string offer =
            "{\"aesIvTypes\":\"3\",\"aesKeyTypes\":\"1\",\"authAlgorithmTypes\":\"4\","
            "\"authKeyTypes\":\"1\",\"integrityTypes\":\"1\"}";
        auto env = encode_tlv_string("cmd", offer);
        send_cmd(CmdType::SafetyInfo, env, false);
    }

    {
        std::unique_lock<std::mutex> lk(auth_mu_);
        auth_cv_.wait_for(lk, std::chrono::milliseconds(1500), [this]() {
            return safety_info_ack_received_ || !connected_.load();
        });
    }

    if (!connected_.load()) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lk(auth_mu_);
            reason = failure_reason_.empty() ? "8899 SafetyInfo 协商阶段连接断开" : failure_reason_;
            stage_ = HandshakeStage::Failed;
        }
        emit_status(reason);
        disconnect();
        return false;
    }

    bool need_safety = false;
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        need_safety = safety_info_ok_;
        if (need_safety) stage_ = HandshakeStage::AuthenticatingSafety;
    }

    if (need_safety) {
        emit_status("8899 正在完成 Safety 双向认证...");
        bool mutual_ok = false;
        {
            std::unique_lock<std::mutex> lk(auth_mu_);
            auth_cv_.wait_for(lk, std::chrono::milliseconds(2500), [this]() {
                return (peer_challenge_acked_ && local_challenge_verified_) || !connected_.load();
            });
            mutual_ok = (peer_challenge_acked_ && local_challenge_verified_ && connected_.load());
            if (mutual_ok) {
                stage_ = HandshakeStage::ReadySafety;
                safety_active_.store(true);
            } else {
                stage_ = HandshakeStage::Failed;
                if (failure_reason_.empty()) {
                    if (!connected_.load()) failure_reason_ = "8899 SafetyAuth 阶段被远端重置/断开 (RST)";
                    else if (!peer_challenge_acked_) failure_reason_ = "8899 SafetyAuth 未能解密对端 0x1402 挑战";
                    else failure_reason_ = "8899 SafetyAuth 等待对端 0x1403 确认超时";
                }
            }
        }
        if (!mutual_ok) {
            std::string reason;
            {
                std::lock_guard<std::mutex> lk(auth_mu_);
                reason = failure_reason_;
            }
            emit_status(reason);
            disconnect();
            return false;
        }
        emit_status("8899 Safety 双向加密认证通过");
    } else {
        {
            std::lock_guard<std::mutex> lk(auth_mu_);
            stage_ = HandshakeStage::ReadyLegacy;
        }
        emit_status("8899 Legacy 认证通过（明文通道）");
    }

    if (aborted_.load() || !connected_.load()) {
        disconnect();
        return false;
    }
    hb_th_ = std::thread(&CmdSession::heartbeat_loop, this);
    return true;
}

void CmdSession::send_close_and_abort() {
    if (connected_.load()) {
        send_close_device();
        {
            std::lock_guard<std::mutex> lk(sock_mu_);
            if (sock_ != kInvalidSocket) {
#ifdef _WIN32
                ::shutdown(sock_, SD_SEND);
#else
                ::shutdown(sock_, SHUT_WR);
#endif
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    abort();
}

void CmdSession::abort() {
    aborted_.store(true);
    running_.store(false);
    connected_.store(false);
    auth_cv_.notify_all();
    wake_update_worker();
    std::lock_guard<std::mutex> lk(sock_mu_);
    if (sock_ != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(sock_, SD_BOTH);
#else
        ::shutdown(sock_, SHUT_RDWR);
#endif
    }
}

void CmdSession::close_and_disconnect() {
    send_close_and_abort();
    disconnect();
}

void CmdSession::disconnect() {
    aborted_.store(true);
    running_.store(false);
    bool was_conn = connected_.exchange(false);
    auth_cv_.notify_all();
    wake_update_worker();
    {
        std::lock_guard<std::mutex> lk(sock_mu_);
        if (sock_ != kInvalidSocket) {
#ifdef _WIN32
            ::shutdown(sock_, SD_BOTH);
#else
            ::shutdown(sock_, SHUT_RDWR);
#endif
        }
    }
    if (recv_th_.joinable() && recv_th_.get_id() != std::this_thread::get_id()) recv_th_.join();
    if (hb_th_.joinable() && hb_th_.get_id() != std::this_thread::get_id()) hb_th_.join();
    {
        std::scoped_lock lock(send_mu_, sock_mu_);
        writer_.reset();
        if (sock_ != kInvalidSocket) {
            net::close_socket(sock_);
            sock_ = kInvalidSocket;
        }
    }
    {
        std::lock_guard lock(updates_mu_);
        pending_media_.reset();
        pending_position_.reset();
    }
    if (was_conn && cbs_.on_state_changed) cbs_.on_state_changed(false);
}

bool CmdSession::send_raw_locked(uint16_t cmd_type, uint16_t seq, const std::vector<uint8_t>& payload) {
    if (!connected_.load() || !writer_) return false;
    CmdFrame f;
    f.cmd_type = cmd_type;
    f.seq = seq;
    f.payload = payload;
    auto wire = encode_frame(f);
    if (writer_->write(wire.data(), wire.size(), 100, &aborted_) == net::SendAllStatus::Ok) {
        return true;
    }
    writer_->abort();
    return false;
}

bool CmdSession::send_cmd(CmdType type, const std::vector<uint8_t>& raw_payload, bool allow_encrypt) {
    if (!connected_.load()) return false;
    std::lock_guard<std::mutex> lock(send_mu_);
    uint16_t s = seq_.fetch_add(1);
    std::vector<uint8_t> payload = raw_payload;
    if (allow_encrypt && safety_active_.load() && enc_cipher_.is_ready()) {
        payload = enc_cipher_.encrypt_blob(raw_payload);
    }
    return send_raw_locked(static_cast<uint16_t>(type), s, payload);
}

bool CmdSession::send_get_version(const std::string& ver) {
    std::vector<uint8_t> p(ver.begin(), ver.end());
    p.push_back(0x00);
    return send_cmd(CmdType::GetVersion, p, false);
}

bool CmdSession::send_get_device_info() {
    return send_cmd(CmdType::GetDeviceInfo, {}, true);
}

bool CmdSession::send_set_local_device_info(const std::string& source_name) {
    std::string json = "{\"sourceName\":\"" + source_name + "\"}";
    std::vector<uint8_t> p(json.begin(), json.end());
    return send_cmd(CmdType::SetLocalDeviceInfo, p, true);
}

bool CmdSession::send_set_play_source(int play_source) {
    std::string json = "{\"playSource\":" + std::to_string(play_source) + "}";
    std::vector<uint8_t> p(json.begin(), json.end());
    return send_cmd(CmdType::SetPlaySource, p, true);
}

bool CmdSession::send_open_device(const std::string& sender_ip, uint16_t rtsp_port, int mirror_mode) {
    auto st = handshake_stage();
    if (st != HandshakeStage::ReadySafety && st != HandshakeStage::ReadyLegacy) {
        return false;
    }

    send_set_local_device_info("MiPlay MultiRoom");
    send_set_play_source(1);

    std::string url = "wfd://" + sender_ip + ":" + std::to_string(rtsp_port) +
                      "?mirrorMode=" + std::to_string(mirror_mode);
    std::vector<uint8_t> p(url.begin(), url.end());
    p.push_back(0x00);
    bool ok = send_cmd(CmdType::Open, p, true);
    if (ok) emit_status("已发 Open(" + sender_ip + ":" + std::to_string(rtsp_port) + ")，等待设备反连 RTSP...");
    return ok;
}

bool CmdSession::send_close_device() {
    return send_cmd(CmdType::Close, {}, true);
}

bool CmdSession::send_resume() {
    return send_cmd(CmdType::Resume, {}, true);
}

bool CmdSession::send_pause() {
    return send_cmd(CmdType::Pause, {}, true);
}

bool CmdSession::send_heartbeat() {
    return send_cmd(CmdType::HeartBeat, {}, true);
}

bool CmdSession::send_set_volume(int volume_0_100) {
    if (volume_0_100 < 0) volume_0_100 = 0;
    if (volume_0_100 > 100) volume_0_100 = 100;
    // 4 字节大端整数 (如 41% -> 00 00 00 29)，实测同时兼容 SoundFlow 1.1.2 与小米 MiPCAudio / 开源接收端
    return send_cmd(CmdType::SetVolume, encode_u32_be(static_cast<uint32_t>(volume_0_100)), true);
}

bool CmdSession::send_set_position_ms(uint64_t position_ms) {
    std::vector<uint8_t> p(8, 0);
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((position_ms >> (8 * i)) & 0xFF);
    return send_cmd(CmdType::SetPosition, p, true);
}

bool CmdSession::send_set_media_info(const std::string& title, const std::string& artist,
                                     const std::string& album, uint64_t duration_ms) {
    MediaInfoPayload info;
    info.title = title;
    info.artist = artist;
    info.album = album;
    info.duration_ms = duration_ms;
    return send_set_media_info(info);
}

bool CmdSession::send_set_media_info(const MediaInfoPayload& info) {
    // 若仅有实时单行歌词而无完整 LRC 文本，则同时填充 mLrc/lyrics 以便所有接收端展示
    const std::string lrc_payload = !info.lyrics.empty() ? info.lyrics : info.lyric_line;
    json::Value::Object obj{
        {"title", info.title},
        {"mTitle", info.title},
        {"artist", info.artist},
        {"mArtist", info.artist},
        {"album", info.album},
        {"mAlbum", info.album},
        {"duration_ms", info.duration_ms},
        {"durationMs", info.duration_ms},
        {"mDuration", info.duration_ms},
        {"position_ms", info.position_ms},
        {"positionMs", info.position_ms},
        {"mPosition", info.position_ms},
        {"sourceName", info.source_name},
        {"source_name", info.source_name},
        {"mMetaChangeType", std::to_string(info.meta_change_type)},
        {"metaChangeType", std::to_string(info.meta_change_type)},
        {"metadata_change_type", info.meta_change_type},
    };
    if (!lrc_payload.empty()) {
        obj["mLrc"] = lrc_payload;
        obj["lyrics"] = lrc_payload;
        obj["lrc"] = lrc_payload;
    }
    if (!info.lyric_line.empty()) {
        obj["lyric_line"] = info.lyric_line;
        obj["lyricLine"] = info.lyric_line;
        obj["currentLyric"] = info.lyric_line;
    }
    const std::string json_str = json::dump(obj);
    std::vector<uint8_t> p(json_str.begin(), json_str.end());
    return send_cmd(CmdType::SetMediaInfo, p, true);
}

void CmdSession::handle_incoming_frame(const CmdFrame& f) {
    if (f.cmd_type == static_cast<uint16_t>(CmdType::Challenge)) {
        // 已完成握手进入业务态时忽略迟到的挑战帧，避免打乱会话
        {
            std::lock_guard<std::mutex> alk(auth_mu_);
            if (stage_ == HandshakeStage::ReadySafety || stage_ == HandshakeStage::ReadyLegacy) return;
        }
        auto resp = compute_legacy_challenge_response(f.payload);
        {
            std::lock_guard<std::mutex> slk(send_mu_);
            send_raw_locked(static_cast<uint16_t>(CmdType::ChallengeAck), f.seq, resp);
        }
        {
            std::lock_guard<std::mutex> alk(auth_mu_);
            legacy_auth_done_ = true;
        }
        auth_cv_.notify_all();
        return;
    }

    if (f.cmd_type == static_cast<uint16_t>(CmdType::GetVersionAck)) {
        {
            std::lock_guard<std::mutex> alk(auth_mu_);
            version_ack_received_ = true;
        }
        auth_cv_.notify_all();
        if (cbs_.on_version) {
            std::string v(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
            while (!v.empty() && v.back() == '\0') v.pop_back();
            cbs_.on_version(v);
        }
        return;
    }

    if (f.cmd_type == static_cast<uint16_t>(CmdType::SafetyInfoAck)) {
        {
            std::lock_guard<std::mutex> alk(auth_mu_);
            if (stage_ != HandshakeStage::NegotiatingSafety) return;
        }
        auto json_opt = decode_tlv_string(f.payload);
        if (!json_opt) {
            std::lock_guard<std::mutex> alk(auth_mu_);
            safety_info_ack_received_ = true;
            safety_info_ok_ = false;
            auth_cv_.notify_all();
            return;
        }
        std::string res = extract_json_field(*json_opt, "result");
        if (res != "0") {
            std::lock_guard<std::mutex> alk(auth_mu_);
            safety_info_ack_received_ = true;
            safety_info_ok_ = false;
            auth_cv_.notify_all();
            return;
        }

        // 派生 Type-1 密钥材料（receiverIP + receiverPort + senderIP + senderPort）
        auth_key_ = derive_type1_auth_key(ip_, port_, local_ip_, local_port_);
        const uint8_t* key16 = reinterpret_cast<const uint8_t*>(auth_key_.data());
        const uint8_t* iv_t1 = reinterpret_cast<const uint8_t*>(auth_key_.data());
        const uint8_t* iv_t2 = reinterpret_cast<const uint8_t*>(auth_key_.data() + 16);

        // 真机实证：即使 0x1401 协商 aesIvType="2"，小米原生/SoundFlow 接收端双向均以 iv_t1 (authKey[0:16]) 为初始 CBC IV
        // 开源 miplay-bridge 入向同样支持 iv_t1 (type-1-compat) 自动探测，因此发送端固定使用 iv_t1 实现全设备 100% 互通
        {
            std::lock_guard<std::mutex> slk(send_mu_);
            enc_cipher_.init(key16, iv_t1);
            dec_cand_type1_.init(key16, iv_t1);
            dec_cand_type2_.init(key16, iv_t2);
            dec_cipher_locked_ = false;

            std::string ts_str = std::to_string(sync::monotonic_now_us());
            auto md = crypto::md5(reinterpret_cast<const uint8_t*>(ts_str.data()), ts_str.size());
            local_auth_msg_ = crypto::hex_lower(md.data(), md.size());
            std::string ch_json = "{\"authMsg\":\"" + local_auth_msg_ + "\"}";
            auto env = encode_tlv_string("cmd", ch_json);
            auto blob = enc_cipher_.encrypt_blob(env);
            send_raw_locked(static_cast<uint16_t>(CmdType::SafetyAuth), seq_.fetch_add(1), blob);
        }

        {
            std::lock_guard<std::mutex> alk(auth_mu_);
            safety_info_ack_received_ = true;
            safety_info_ok_ = true;
            stage_ = HandshakeStage::AuthenticatingSafety;
        }
        auth_cv_.notify_all();
        return;
    }

    if (f.cmd_type == static_cast<uint16_t>(CmdType::SafetyAuth) ||
        f.cmd_type == static_cast<uint16_t>(CmdType::SafetyAuthAck)) {
        std::optional<std::vector<uint8_t>> plain;
        std::optional<std::string> json_opt;

        if (dec_cipher_locked_) {
            plain = dec_cipher_.decrypt_blob(f.payload);
            if (plain) json_opt = decode_tlv_string(*plain);
        } else {
            SafetyCipher c1 = dec_cand_type1_;
            if (auto p1 = c1.decrypt_blob(f.payload)) {
                if (auto j1 = decode_tlv_string(*p1)) {
                    dec_cipher_ = c1;
                    dec_cipher_locked_ = true;
                    json_opt = j1;
                }
            }
            if (!json_opt) {
                SafetyCipher c2 = dec_cand_type2_;
                if (auto p2 = c2.decrypt_blob(f.payload)) {
                    if (auto j2 = decode_tlv_string(*p2)) {
                        dec_cipher_ = c2;
                        dec_cipher_locked_ = true;
                        json_opt = j2;
                    }
                }
            }
        }

        if (!json_opt) {
            std::lock_guard<std::mutex> alk(auth_mu_);
            if (failure_reason_.empty()) {
                failure_reason_ = "8899 SafetyAuth 解密或 TLV 解析失败";
            }
            return;
        }

        if (f.cmd_type == static_cast<uint16_t>(CmdType::SafetyAuth)) {
            std::string peer_msg = extract_json_field(*json_opt, "authMsg");
            if (!peer_msg.empty()) {
                auto mac = crypto::hmac_sha256(
                    reinterpret_cast<const uint8_t*>(auth_key_.data()), auth_key_.size(),
                    reinterpret_cast<const uint8_t*>(peer_msg.data()), peer_msg.size());
                std::string ack_digest = crypto::hex_lower(mac.data(), mac.size());
                std::string ack_json = "{\"result\":\"0\",\"authMsgAck\":\"" + ack_digest + "\"}";
                auto env = encode_tlv_string("ack", ack_json);
                {
                    std::lock_guard<std::mutex> slk(send_mu_);
                    auto blob = enc_cipher_.encrypt_blob(env);
                    send_raw_locked(static_cast<uint16_t>(CmdType::SafetyAuthAck), f.seq, blob);
                }
                {
                    std::lock_guard<std::mutex> alk(auth_mu_);
                    peer_challenge_acked_ = true;
                }
                auth_cv_.notify_all();
            }
        } else {
            std::string res = extract_json_field(*json_opt, "result");
            if (res == "0" || res == "1") {
                std::lock_guard<std::mutex> alk(auth_mu_);
                local_challenge_verified_ = true;
                auth_cv_.notify_all();
            }
        }
        return;
    }

    CmdFrame out = f;
    if (safety_active_.load() && dec_cipher_locked_ && f.payload.size() >= 25) {
        if (auto p = dec_cipher_.decrypt_blob(f.payload)) {
            out.payload = std::move(*p);
        }
    }
    // 兼容直接发送 Pause/Resume/SetPosition 请求帧的接收端，及时回复对应 Ack
    if (out.cmd_type == static_cast<uint16_t>(CmdType::Pause) ||
        out.cmd_type == static_cast<uint16_t>(CmdType::Resume) ||
        out.cmd_type == static_cast<uint16_t>(CmdType::SetPosition)) {
        const uint16_t ack_cmd = static_cast<uint16_t>(out.cmd_type + 1);
        std::lock_guard<std::mutex> slk(send_mu_);
        std::vector<uint8_t> ack_payload;
        if (safety_active_.load() && enc_cipher_.is_ready()) {
            ack_payload = enc_cipher_.encrypt_blob(ack_payload);
        }
        send_raw_locked(ack_cmd, out.seq, ack_payload);
    }
    if (cbs_.on_remote_control) {
        for (const auto& ev : decode_remote_control_events(out.cmd_type, out.payload)) {
            cbs_.on_remote_control(ev);
        }
    }
    if (cbs_.on_frame) cbs_.on_frame(out);
}

void CmdSession::recv_loop() {
    std::vector<uint8_t> acc;
    acc.reserve(8192);
    uint8_t tmp[4096];

    while (running_.load()) {
        if (!net::wait_readable(sock_, 50) || !running_.load()) continue;
        auto n = recv(sock_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
        if (n > 0) {
            acc.insert(acc.end(), tmp, tmp + n);
            std::vector<CmdFrame> frames;
            size_t consumed = decode_frames(acc.data(), acc.size(), frames);
            if (consumed > 0) {
                acc.erase(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(consumed));
            }
            for (const auto& f : frames) {
                handle_incoming_frame(f);
            }
        } else if (n == 0) {
            mark_disconnected("8899 连接被远端关闭 (FIN)");
            break;
        } else {
            if (net::last_socket_error_is_timeout()) {
                continue;
            }
            int err = net::last_socket_error_code();
            mark_disconnected("8899 连接被远端重置/断开 (err=" + std::to_string(err) + ")");
            break;
        }
    }
}

void CmdSession::queue_media_info(const MediaInfoPayload& info) {
    {
        std::lock_guard lock(updates_mu_);
        if (!connected_.load()) return;
        pending_media_ = info;
    }
    updates_cv_.notify_one();
}

void CmdSession::queue_position_ms(uint64_t position_ms) {
    {
        std::lock_guard lock(updates_mu_);
        if (!connected_.load()) return;
        pending_position_ = position_ms;
    }
    updates_cv_.notify_one();
}

void CmdSession::wake_update_worker() {
    // Serialize a state notification with the worker entering its condition wait.
    std::lock_guard lock(updates_mu_);
    updates_cv_.notify_one();
}

void CmdSession::heartbeat_loop() {
    using Clock = std::chrono::steady_clock;
    constexpr auto heartbeat_interval = std::chrono::seconds(5);
    auto next_heartbeat = Clock::now() + heartbeat_interval;
    while (running_.load() && connected_.load()) {
        std::optional<MediaInfoPayload> media;
        std::optional<uint64_t> position;
        {
            std::unique_lock lock(updates_mu_);
            updates_cv_.wait_until(lock, next_heartbeat, [this] {
                return pending_media_ || pending_position_ || !running_.load() || !connected_.load();
            });
            if (!running_.load() || !connected_.load()) break;
            media.swap(pending_media_);
            position.swap(pending_position_);
        }
        if (Clock::now() >= next_heartbeat) {
            if (!send_heartbeat()) {
                mark_disconnected("8899 心跳发送失败/超时");
                abort();
                break;
            }
            next_heartbeat = Clock::now() + heartbeat_interval;
        }
        if (media && !send_set_media_info(*media)) {
            mark_disconnected("8899 媒体信息发送失败/超时");
            abort();
            break;
        }
        if (position && !send_set_position_ms(*position)) {
            mark_disconnected("8899 播放进度发送失败/超时");
            abort();
            break;
        }
    }
}

} // namespace miplay::cmd
