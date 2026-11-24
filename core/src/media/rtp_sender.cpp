#include "media/rtp_sender.h"
#include "sync/master_clock.h"

#include <algorithm>
#include <cstring>

namespace miplay::media {

namespace {
constexpr size_t kTsPktSize = 188;
constexpr size_t kMaxTsPerRtp = 7; // 7 * 188 = 1316 bytes < MTU 1500
} // namespace

void MultiAudioSender::ActiveTarget::start_writer() {
    // The RTSP server may still need this TCP connection for teardown.
    if (tcp_data_sock != kInvalidSocket) {
        net::set_socket_timeout(tcp_data_sock, SO_SNDTIMEO, 100);
    }
    writer_th = std::thread([this]() {
        int consecutive_timeouts = 0;
        while (!stop_requested.load() && healthy.load()) {
            QueuedUnit unit;
            {
                std::unique_lock<std::mutex> lk(q_mu);
                q_cv.wait(lk, [this]() {
                    return stop_requested.load() || !healthy.load() || !queue.empty();
                });
                if (stop_requested.load() || !healthy.load()) break;
                unit = std::move(queue.front());
                queue.pop_front();
            }

            for (const auto& pkt : unit.rtp_packets) {
                if (stop_requested.load()) break;
                if (!net::send_all(tcp_data_sock, pkt.data(), pkt.size())) {
                    if (net::last_socket_error_is_timeout() && !stop_requested.load()) {
                        ++consecutive_timeouts;
                        dropped_frames.fetch_add(1);
                        needs_resync.store(true);
                        if (consecutive_timeouts >= 4) {
                            healthy.store(false);
                        }
                        break;
                    }
                    healthy.store(false);
                    break;
                }
                consecutive_timeouts = 0;
                packets_sent.fetch_add(1);
                bytes_sent.fetch_add(pkt.size());
            }
        }
    });
}

void MultiAudioSender::ActiveTarget::stop_and_join() {
    {
        std::lock_guard<std::mutex> lock(q_mu);
        stop_requested.store(true);
        healthy.store(false);
    }
    if (owns_socket && tcp_data_sock != kInvalidSocket) {
        net::shutdown_socket(tcp_data_sock);
    }
    q_cv.notify_all();
    if (writer_th.joinable()) writer_th.join();
    if (owns_socket && tcp_data_sock != kInvalidSocket) {
        net::close_socket(tcp_data_sock);
        tcp_data_sock = kInvalidSocket;
    }
}

MultiAudioSender::~MultiAudioSender() {
    clear_all();
}

void MultiAudioSender::set_stream_type(TsAudioStreamType type) {
    std::lock_guard<std::mutex> lock(mu_);
    stream_type_ = type;
    shared_packetizer_.set_stream_type(type);
    shared_packetizer_.reset();
    for (auto& [_, p] : biased_packetizers_) {
        p.set_stream_type(type);
        p.reset();
    }
}

void MultiAudioSender::add_or_update_target(
    const std::string& device_id,
    const std::string& peer_ip,
    SocketHandle data_sock,
    int32_t delay_bias_ms,
    bool owns_socket,
    uint64_t session_id) {
    std::shared_ptr<ActiveTarget> replaced;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = targets_.find(device_id);
        if (it != targets_.end()) {
            // 防止过期的旧会话回写覆盖新会话目标
            if (session_id != 0 && it->second->session_id != 0 && session_id < it->second->session_id) {
                if (owns_socket && data_sock != kInvalidSocket) net::close_socket(data_sock);
                return;
            }
            replaced = std::move(it->second);
            targets_.erase(it);
        }
        auto node = std::make_shared<ActiveTarget>();
        node->device_id = device_id;
        node->session_id = session_id;
        node->peer_ip = peer_ip;
        node->tcp_data_sock = data_sock;
        node->owns_socket = owns_socket;
        node->delay_bias_ms.store(std::clamp(delay_bias_ms, -300, 300));
        node->healthy.store(data_sock != kInvalidSocket);
        if (data_sock != kInvalidSocket) {
            node->start_writer();
        }
        targets_[device_id] = std::move(node);
        if (delay_bias_ms != 0) {
            biased_packetizers_[device_id] = TsPacketizer(stream_type_);
        }
    }
    if (replaced) replaced->stop_and_join();
}

void MultiAudioSender::remove_target(const std::string& device_id, uint64_t session_id) {
    std::shared_ptr<ActiveTarget> removed;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = targets_.find(device_id);
        if (it != targets_.end()) {
            if (session_id != 0 && it->second->session_id != session_id) {
                return; // 旧会话清理不误删新会话目标
            }
            removed = std::move(it->second);
            targets_.erase(it);
            biased_packetizers_.erase(device_id);
        }
    }
    if (removed) removed->stop_and_join();
}

void MultiAudioSender::set_target_delay_bias(const std::string& device_id, int32_t delay_bias_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    delay_bias_ms = std::clamp(delay_bias_ms, -300, 300);
    auto it = targets_.find(device_id);
    if (it != targets_.end()) {
        it->second->delay_bias_ms.store(delay_bias_ms);
        if (delay_bias_ms != 0 && biased_packetizers_.find(device_id) == biased_packetizers_.end()) {
            biased_packetizers_[device_id] = TsPacketizer(stream_type_);
        }
    }
}

void MultiAudioSender::clear_all() {
    std::vector<std::shared_ptr<ActiveTarget>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mu_);
        to_stop.reserve(targets_.size());
        for (auto& [_, node] : targets_) {
            to_stop.push_back(std::move(node));
        }
        targets_.clear();
        biased_packetizers_.clear();
    }
    for (auto& node : to_stop) {
        if (node) node->stop_and_join();
    }
}

size_t MultiAudioSender::active_target_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return targets_.size();
}

bool MultiAudioSender::has_streaming_target() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::any_of(targets_.begin(), targets_.end(), [](const auto& entry) {
        const auto& target = *entry.second;
        return target.healthy.load() && target.packets_sent.load() > 0;
    });
}

std::vector<uint8_t> MultiAudioSender::build_rtp_over_tcp_packet(
    uint16_t rtp_seq,
    uint32_t rtp_ts_90khz,
    const uint8_t* ts_bytes,
    size_t ts_len,
    uint32_t ssrc) {
    const uint16_t rtp_len = static_cast<uint16_t>(12 + ts_len);
    std::vector<uint8_t> pkt(4 + rtp_len);
    pkt[0] = 0x24;
    pkt[1] = 0x00;
    pkt[2] = static_cast<uint8_t>((rtp_len >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(rtp_len & 0xFF);

    pkt[4] = 0x80;
    pkt[5] = 0xA1;
    pkt[6] = static_cast<uint8_t>((rtp_seq >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(rtp_seq & 0xFF);
    pkt[8] = static_cast<uint8_t>((rtp_ts_90khz >> 24) & 0xFF);
    pkt[9] = static_cast<uint8_t>((rtp_ts_90khz >> 16) & 0xFF);
    pkt[10] = static_cast<uint8_t>((rtp_ts_90khz >> 8) & 0xFF);
    pkt[11] = static_cast<uint8_t>(rtp_ts_90khz & 0xFF);
    pkt[12] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    pkt[13] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    pkt[14] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    pkt[15] = static_cast<uint8_t>(ssrc & 0xFF);

    std::memcpy(pkt.data() + 16, ts_bytes, ts_len);
    return pkt;
}

void MultiAudioSender::broadcast_audio_frame(
    const uint8_t* audio_payload,
    size_t payload_len,
    uint64_t base_pts_90khz,
    uint32_t base_rtp_ts_90khz) {
    std::lock_guard<std::mutex> lock(mu_);
    if (targets_.empty()) return;

    std::vector<uint8_t> shared_ts;
    bool built_shared = false;

    for (auto& [id, node] : targets_) {
        if (!node->healthy.load() || node->stop_requested.load() || node->tcp_data_sock == kInvalidSocket) {
            continue;
        }

        const int32_t bias_ms = node->delay_bias_ms.load();
        const bool need_resync = node->needs_resync.exchange(false);
        const std::vector<uint8_t>* ts_ptr = nullptr;
        std::vector<uint8_t> local_ts;
        uint32_t node_rtp_ts = base_rtp_ts_90khz;

        if (bias_ms == 0 && !need_resync) {
            if (!built_shared) {
                shared_ts = shared_packetizer_.packetize_audio_frame(
                    audio_payload, payload_len, base_pts_90khz);
                built_shared = true;
            }
            ts_ptr = &shared_ts;
        } else {
            int64_t delta_90k = sync::MasterMediaClock::us_to_90khz(
                static_cast<int64_t>(bias_ms) * 1000LL);
            int64_t adj_pts = static_cast<int64_t>(base_pts_90khz) + delta_90k;
            if (adj_pts < 0) adj_pts = 0;
            node_rtp_ts = static_cast<uint32_t>(static_cast<uint64_t>(adj_pts) & 0xFFFFFFFFULL);
            auto& pack = biased_packetizers_[id];
            if (need_resync) {
                pack.set_stream_type(stream_type_);
                pack.reset(); // 强制下一帧带 PAT/PMT/PCR，实现干净 TS 边界恢复
            }
            local_ts = pack.packetize_audio_frame(
                audio_payload, payload_len, static_cast<uint64_t>(adj_pts));
            ts_ptr = &local_ts;
        }

        QueuedUnit unit;
        const size_t chunk_max = kMaxTsPerRtp * kTsPktSize;
        for (size_t off = 0; off < ts_ptr->size(); off += chunk_max) {
            size_t len = std::min(chunk_max, ts_ptr->size() - off);
            unit.rtp_packets.push_back(build_rtp_over_tcp_packet(
                node->rtp_seq++, node_rtp_ts, ts_ptr->data() + off, len));
        }

        {
            std::lock_guard<std::mutex> qlk(node->q_mu);
            while (node->queue.size() >= kMaxQueuedFramesPerTarget) {
                node->queue.pop_front(); // 淘汰最旧整帧（保持 TS/PES 完整边界）
                node->dropped_frames.fetch_add(1);
                node->needs_resync.store(true);
            }
            node->queue.push_back(std::move(unit));
        }
        node->q_cv.notify_one();
    }
}

std::vector<MirrorTargetNode> MultiAudioSender::snapshot_nodes() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MirrorTargetNode> out;
    out.reserve(targets_.size());
    for (const auto& [_, n] : targets_) {
        MirrorTargetNode snap;
        snap.device_id = n->device_id;
        snap.session_id = n->session_id;
        snap.peer_ip = n->peer_ip;
        snap.tcp_data_sock = n->tcp_data_sock;
        snap.delay_bias_ms = n->delay_bias_ms.load();
        snap.packets_sent = n->packets_sent.load();
        snap.bytes_sent = n->bytes_sent.load();
        snap.dropped_frames = n->dropped_frames.load();
        snap.healthy = n->healthy.load();
        snap.owns_socket = n->owns_socket;
        out.push_back(std::move(snap));
    }
    return out;
}

} // namespace miplay::media
