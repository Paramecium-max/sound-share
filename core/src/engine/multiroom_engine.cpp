#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace miplay {

MultiRoomEngine::MultiRoomEngine(std::string bind_ip)
    : bind_ip_(bind_ip.empty() ? net::pick_default_lan_ipv4() : std::move(bind_ip)) {
    net::init_network();
    audio_pipeline_.set_source_mode(media::AudioSourceMode::TestTone, "");
}

MultiRoomEngine::~MultiRoomEngine() {
    stop_multiroom();
    stop_discovery();
}

void MultiRoomEngine::set_event_callback(JsonEventCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    event_cb_ = std::move(cb);
}

void MultiRoomEngine::set_bind_ip(const std::string& ip) {
    if (ip.empty()) return;
    bool was_disc = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        bind_ip_ = ip;
        was_disc = discovery_running_;
    }
    if (was_disc) {
        stop_discovery();
        start_discovery();
    }
    emit_state_changed();
}

std::string MultiRoomEngine::bind_ip() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bind_ip_;
}

bool MultiRoomEngine::start_discovery() {
    std::string ip;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (discovery_running_) return true;
        ip = bind_ip_;
    }
    bool ok = discovery_.start(ip, [this](const discovery::DiscoveredDevice& dev) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = devices_.find(dev.id);
            if (it == devices_.end()) {
                ManagedDevice md;
                md.info = dev;
                devices_[dev.id] = std::move(md);
                changed = true;
            } else {
                if (it->second.info.ip != dev.ip || it->second.info.cmd_port != dev.cmd_port) {
                    changed = true;
                }
                it->second.info.ip = dev.ip;
                it->second.info.cmd_port = dev.cmd_port;
                if (!dev.name.empty() && it->second.info.name != dev.name) {
                    it->second.info.name = dev.name;
                    changed = true;
                }
                it->second.info.last_seen_us = dev.last_seen_us;
            }
        }
        if (changed) emit_state_changed();
    });
    if (ok) {
        std::lock_guard<std::mutex> lock(mu_);
        discovery_running_ = true;
    }
    emit_state_changed();
    return ok;
}

void MultiRoomEngine::stop_discovery() {
    discovery_.stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        discovery_running_ = false;
    }
    emit_state_changed();
}

void MultiRoomEngine::refresh_discovery() {
    discovery_.trigger_query();
}

std::string MultiRoomEngine::add_manual_device(const std::string& name, const std::string& ip, uint16_t cmd_port) {
    std::string id = "manual_" + ip + "_" + std::to_string(cmd_port);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& md = devices_[id];
        md.info.id = id;
        md.info.name = name.empty() ? ("MiPlay-" + ip) : name;
        md.info.ip = ip;
        md.info.cmd_port = cmd_port;
        md.info.is_manual = true;
        md.info.device_type = 4;
        md.info.support_audio = true;
        md.info.last_seen_us = sync::monotonic_now_us();
    }
    emit_state_changed();
    return id;
}

bool MultiRoomEngine::remove_device(const std::string& device_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        detach_device_stream_locked(it->second);
        devices_.erase(it);
    }
    emit_state_changed();
    return true;
}

bool MultiRoomEngine::start_multiroom(bool prefer_lpcm, uint32_t sample_rate, uint32_t base_play_delay_ms) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (multiroom_running_) return true;

        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        prefer_lpcm_ = prefer_lpcm;

        // 优先启用原生 ADTS AAC-LC 编码器；若当前平台无原生 AAC 编码器则自动回退 WFD LPCM 保证编码与 RTSP 协商一致
        aac_encoder_.shutdown();
        if (!prefer_lpcm_) {
            if (!aac_encoder_.init(sample_rate_, 2, 192000)) {
                prefer_lpcm_ = true;
            }
        }

        master_clock_.reset(sample_rate_, base_play_delay_ms);
        multi_sender_.set_stream_type(
            prefer_lpcm_ ? media::TsAudioStreamType::WfdLpcm : media::TsAudioStreamType::AdtsAac);

        if (!timer_service_.start("0.0.0.0", 0)) {
            return false;
        }

        // 若用户未勾选任何设备但列表有设备，不阻断启动（前端也支持热加入）
        for (auto& [_, dev] : devices_) {
            if (dev.in_group) {
                attach_device_stream_locked(dev);
            }
        }

        // AAC-LC 固定 1024 samples/帧 (21.333ms, 90kHz 步进 1920)；LPCM 用 480 samples/帧 (10ms)
        const uint32_t frame_samples = prefer_lpcm_ ? ((sample_rate_ == 44100) ? 441 : 480) : 1024;
        const bool use_aac = !prefer_lpcm_ && aac_encoder_.is_available();

        audio_pipeline_.start(
            sample_rate_,
            frame_samples,
            [this, use_aac](const int16_t* stereo_pcm, size_t samples) {
                auto ts = master_clock_.next_frame_timestamps(static_cast<uint32_t>(samples));
                if (use_aac) {
                    aac_encoder_.encode_pcm(stereo_pcm, samples,
                        [this, ts](const uint8_t* adts, size_t len) {
                            multi_sender_.broadcast_audio_frame(adts, len, ts.pts_90khz, ts.rtp_ts_90khz);
                        });
                } else {
                    auto lpcm_frame = media::TsPacketizer::wrap_wfd_lpcm(stereo_pcm, samples);
                    multi_sender_.broadcast_audio_frame(
                        lpcm_frame.data(), lpcm_frame.size(), ts.pts_90khz, ts.rtp_ts_90khz);
                }
            },
            [this](const std::array<float, 16>& levels) {
                std::ostringstream oss;
                oss << "{\"type\":\"spectrum\",\"levels\":[";
                for (size_t i = 0; i < levels.size(); ++i) {
                    if (i) oss << ",";
                    oss << std::fixed << std::setprecision(3) << levels[i];
                }
                oss << "]}";
                emit_event_json(oss.str());
            });

        multiroom_running_ = true;
    }
    emit_state_changed();
    return true;
}

void MultiRoomEngine::stop_multiroom() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!multiroom_running_) return;
        multiroom_running_ = false;
        audio_pipeline_.stop();
        for (auto& [_, dev] : devices_) {
            detach_device_stream_locked(dev);
        }
        multi_sender_.clear_all();
        aac_encoder_.shutdown();
        timer_service_.stop();
    }
    emit_state_changed();
}

void MultiRoomEngine::update_device_status(const std::string& dev_id, const std::string& status) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = devices_.find(dev_id);
        if (it != devices_.end()) it->second.status_text = status;
    }
    emit_state_changed();
}

void MultiRoomEngine::attach_device_stream_locked(ManagedDevice& dev) {
    detach_device_stream_locked(dev);

    const std::string dev_id = dev.info.id;
    const std::string dev_ip = dev.info.ip;
    const uint16_t cmd_port = dev.info.cmd_port;
    const int32_t init_vol = dev.volume;
    const std::string sender_ip = bind_ip_;
    const uint64_t gen = ++dev.attach_gen;
    dev.status_text = "启动 RTSP 监听与 8899 握手...";

    // 1. 启动该设备专属反向 WFD RTSP Server (全屋共享同一个 TimerService 端口与会话基准 TimeOffset)
    dev.rtsp_server = std::make_unique<rtsp::WfdRtspServer>();
    rtsp::RtspServerCallbacks rcbs;
    rcbs.on_stream_ready = [this](const std::string& id, const std::string& peer_ip, SocketHandle data_sock) {
        int32_t bias = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = devices_.find(id);
            if (it != devices_.end()) {
                it->second.rtsp_streaming = true;
                bias = it->second.delay_bias_ms;
            }
        }
        multi_sender_.add_or_update_target(id, peer_ip, data_sock, bias);
        emit_state_changed();
    };
    rcbs.on_latency_report = [this](const rtsp::RtspLatencyReport& rep) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = devices_.find(rep.device_id);
        if (it != devices_.end()) it->second.reported_latency_ms = rep.latency_ms;
    };
    rcbs.on_session_closed = [this](const std::string& id) {
        multi_sender_.remove_target(id);
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = devices_.find(id);
            if (it != devices_.end()) it->second.rtsp_streaming = false;
        }
        emit_state_changed();
    };
    rcbs.on_status = [this](const std::string& id, const std::string& st) {
        update_device_status(id, st);
    };

    dev.rtsp_server->start(
        dev_id,
        sender_ip,
        timer_service_.port(),
        master_clock_.session_origin_us(),
        prefer_lpcm_,
        std::move(rcbs));
    uint16_t rtsp_port = dev.rtsp_server->listen_port();

    // 2. 异步建立 TCP 8899 控制连接并完成 0x0028/0x0029 + Safety 握手（绝不阻塞主锁与 UI）
    auto session = std::make_shared<cmd::CmdSession>(dev_ip, cmd_port);
    dev.cmd_session = session;
    auto meta = audio_pipeline_.current_meta();

    std::thread([this, session, dev_id, sender_ip, rtsp_port, init_vol, meta, gen]() {
        cmd::CmdSessionCallbacks ccbs;
        ccbs.on_version = [this, dev_id](const std::string& v) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end()) it->second.remote_version = v;
            }
            emit_state_changed();
        };
        ccbs.on_state_changed = [this, dev_id, gen](bool conn) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end() && it->second.attach_gen == gen) {
                    it->second.cmd_connected = conn;
                }
            }
            emit_state_changed();
        };
        ccbs.on_status = [this, dev_id, gen](const std::string& st) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end() && it->second.attach_gen == gen) {
                    it->second.status_text = st;
                }
            }
            emit_state_changed();
        };

        if (session->connect(std::move(ccbs))) {
            bool still_active = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                still_active = (multiroom_running_ && it != devices_.end() &&
                                it->second.in_group && it->second.attach_gen == gen);
            }
            if (still_active) {
                session->send_open_device(sender_ip, rtsp_port, 1);
                session->send_set_media_info(meta.title, meta.artist, meta.album, meta.duration_ms);
                session->send_set_volume(init_vol);
            } else {
                session->disconnect();
            }
        }
    }).detach();
}

void MultiRoomEngine::detach_device_stream_locked(ManagedDevice& dev) {
    ++dev.attach_gen;
    multi_sender_.remove_target(dev.info.id);
    if (dev.cmd_session) {
        auto s = dev.cmd_session;
        dev.cmd_session.reset();
        if (s->is_connected()) s->send_close_device();
        std::thread([s]() { s->disconnect(); }).detach();
    }
    if (dev.rtsp_server) {
        dev.rtsp_server->stop();
        dev.rtsp_server.reset();
    }
    dev.cmd_connected = false;
    dev.rtsp_streaming = false;
    dev.status_text = "待机";
}

bool MultiRoomEngine::add_device_to_group(const std::string& device_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.in_group = true;
        if (multiroom_running_) {
            attach_device_stream_locked(it->second);
        }
    }
    emit_state_changed();
    return true;
}

bool MultiRoomEngine::remove_device_from_group(const std::string& device_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.in_group = false;
        if (multiroom_running_) {
            detach_device_stream_locked(it->second);
        }
    }
    emit_state_changed();
    return true;
}

void MultiRoomEngine::set_base_play_delay_ms(uint32_t delay_ms) {
    delay_ms = std::clamp<uint32_t>(delay_ms, 100, 1000);
    master_clock_.set_base_play_delay_ms(delay_ms);
    emit_state_changed();
}

bool MultiRoomEngine::set_device_delay_bias(const std::string& device_id, int32_t delay_bias_ms) {
    delay_bias_ms = std::clamp(delay_bias_ms, -300, 300);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.delay_bias_ms = delay_bias_ms;
    }
    multi_sender_.set_target_delay_bias(device_id, delay_bias_ms);
    emit_state_changed();
    return true;
}

bool MultiRoomEngine::set_device_volume(const std::string& device_id, int32_t volume_0_100) {
    volume_0_100 = std::clamp(volume_0_100, 0, 100);
    std::shared_ptr<cmd::CmdSession> s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.volume = volume_0_100;
        s = it->second.cmd_session;
    }
    if (s && s->is_connected()) s->send_set_volume(volume_0_100);
    emit_state_changed();
    return true;
}

void MultiRoomEngine::set_master_volume(int32_t volume_0_100) {
    volume_0_100 = std::clamp(volume_0_100, 0, 100);
    std::vector<std::pair<std::shared_ptr<cmd::CmdSession>, int32_t>> to_send;
    {
        std::lock_guard<std::mutex> lock(mu_);
        master_volume_ = volume_0_100;
        for (auto& [_, dev] : devices_) {
            if (!dev.in_group) continue;
            dev.volume = volume_0_100;
            if (dev.cmd_session && dev.cmd_session->is_connected()) {
                to_send.emplace_back(dev.cmd_session, dev.volume);
            }
        }
    }
    for (auto& [s, v] : to_send) s->send_set_volume(v);
    emit_state_changed();
}

void MultiRoomEngine::set_source_mode(media::AudioSourceMode mode, const std::string& param) {
    audio_pipeline_.set_source_mode(mode, param);
    auto meta = audio_pipeline_.current_meta();
    std::vector<std::shared_ptr<cmd::CmdSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [_, dev] : devices_) {
            if (dev.cmd_session && dev.cmd_session->is_connected()) {
                sessions.push_back(dev.cmd_session);
            }
        }
    }
    for (auto& s : sessions) {
        s->send_set_media_info(meta.title, meta.artist, meta.album, meta.duration_ms);
    }
    emit_state_changed();
}

void MultiRoomEngine::push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples) {
    audio_pipeline_.push_external_pcm(stereo_pcm, frame_samples);
}

EngineStateSnapshot MultiRoomEngine::snapshot_state() const {
    EngineStateSnapshot snap;
    auto timer_map = timer_service_.snapshot_telemetry();
    auto rtp_nodes = multi_sender_.snapshot_nodes();
    std::map<std::string, uint64_t> sent_pkts;
    for (const auto& n : rtp_nodes) sent_pkts[n.device_id] = n.packets_sent;

    std::lock_guard<std::mutex> lock(mu_);
    snap.bind_ip = bind_ip_;
    snap.discovery_running = discovery_running_;
    snap.multiroom_running = multiroom_running_;
    snap.timer_service_port = timer_service_.port();
    snap.session_time_offset_us = master_clock_.session_origin_us();
    snap.sample_rate = sample_rate_;
    snap.base_play_delay_ms = master_clock_.base_play_delay_ms();
    snap.prefer_lpcm = prefer_lpcm_;
    snap.master_volume = master_volume_;
    snap.source_mode = audio_pipeline_.source_mode();
    snap.track_meta = audio_pipeline_.current_meta();

    for (const auto& [_, dev] : devices_) {
        DeviceSnapshot ds;
        ds.info = dev.info;
        ds.in_group = dev.in_group;
        ds.cmd_connected = dev.cmd_connected;
        ds.rtsp_streaming = dev.rtsp_streaming;
        ds.volume = dev.volume;
        ds.delay_bias_ms = dev.delay_bias_ms;
        ds.reported_latency_ms = dev.reported_latency_ms;
        ds.remote_version = dev.remote_version;
        ds.status_text = dev.status_text;
        auto tit = timer_map.find(dev.info.ip);
        if (tit != timer_map.end()) {
            ds.sync_residual_us = tit->second.residual_error_us;
            ds.sync_packets = tit->second.packet_count;
        }
        auto pit = sent_pkts.find(dev.info.id);
        if (pit != sent_pkts.end()) {
            ds.audio_packets_sent = pit->second;
        }
        snap.devices.push_back(std::move(ds));
    }
    return snap;
}

std::string MultiRoomEngine::escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

std::string MultiRoomEngine::snapshot_state_json() const {
    auto snap = snapshot_state();
    std::ostringstream oss;
    oss << "{"
        << "\"bindIp\":\"" << escape_json(snap.bind_ip) << "\","
        << "\"discoveryRunning\":" << (snap.discovery_running ? "true" : "false") << ","
        << "\"multiroomRunning\":" << (snap.multiroom_running ? "true" : "false") << ","
        << "\"timerServicePort\":" << snap.timer_service_port << ","
        << "\"sessionTimeOffsetUs\":" << snap.session_time_offset_us << ","
        << "\"sampleRate\":" << snap.sample_rate << ","
        << "\"basePlayDelayMs\":" << snap.base_play_delay_ms << ","
        << "\"codec\":\"" << (snap.prefer_lpcm ? "LPCM" : "AAC_LC") << "\","
        << "\"masterVolume\":" << snap.master_volume << ","
        << "\"sourceMode\":" << static_cast<int>(snap.source_mode) << ","
        << "\"track\":{"
            << "\"title\":\"" << escape_json(snap.track_meta.title) << "\","
            << "\"artist\":\"" << escape_json(snap.track_meta.artist) << "\","
            << "\"album\":\"" << escape_json(snap.track_meta.album) << "\","
            << "\"durationMs\":" << snap.track_meta.duration_ms << ","
            << "\"positionMs\":" << snap.track_meta.position_ms
        << "},"
        << "\"devices\":[";
    for (size_t i = 0; i < snap.devices.size(); ++i) {
        const auto& d = snap.devices[i];
        if (i) oss << ",";
        oss << "{"
            << "\"id\":\"" << escape_json(d.info.id) << "\","
            << "\"name\":\"" << escape_json(d.info.name) << "\","
            << "\"ip\":\"" << escape_json(d.info.ip) << "\","
            << "\"cmdPort\":" << d.info.cmd_port << ","
            << "\"mac\":\"" << escape_json(d.info.mac) << "\","
            << "\"deviceType\":" << d.info.device_type << ","
            << "\"roomName\":\"" << escape_json(d.info.room_name) << "\","
            << "\"isManual\":" << (d.info.is_manual ? "true" : "false") << ","
            << "\"inGroup\":" << (d.in_group ? "true" : "false") << ","
            << "\"cmdConnected\":" << (d.cmd_connected ? "true" : "false") << ","
            << "\"rtspStreaming\":" << (d.rtsp_streaming ? "true" : "false") << ","
            << "\"volume\":" << d.volume << ","
            << "\"delayBiasMs\":" << d.delay_bias_ms << ","
            << "\"reportedLatencyMs\":" << d.reported_latency_ms << ","
            << "\"syncResidualUs\":" << d.sync_residual_us << ","
            << "\"syncPackets\":" << d.sync_packets << ","
            << "\"audioPacketsSent\":" << d.audio_packets_sent << ","
            << "\"remoteVersion\":\"" << escape_json(d.remote_version) << "\","
            << "\"statusText\":\"" << escape_json(d.status_text) << "\""
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void MultiRoomEngine::emit_event_json(const std::string& json) {
    JsonEventCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = event_cb_;
    }
    if (cb) cb(json);
}

void MultiRoomEngine::emit_state_changed() {
    emit_event_json("{\"type\":\"state\",\"data\":" + snapshot_state_json() + "}");
}

} // namespace miplay
