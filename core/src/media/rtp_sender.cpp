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
    if (tcp_data_sock != kInvalidSocket) {
        net::set_tcp_nodelay(tcp_data_sock, true);
    }
    writer_th = std::thread([this]() {
        TsPacketizer packetizer(stream_type);
        uint16_t rtp_seq = 0;
        int64_t last_pts_90k = -1;
        const int64_t min_frame_step_90k = (stream_type == TsAudioStreamType::WfdLpcm) ? 900 : 1920;
        int consecutive_timeouts = 0;
        std::vector<uint8_t> wire_batch;
        wire_batch.reserve(3072);

        while (!stop_requested.load() && healthy.load()) {
            QueuedFrame frame;
            {
                std::unique_lock<std::mutex> lk(q_mu);
                q_cv.wait(lk, [this]() {
                    return stop_requested.load() || !healthy.load() || !queue.empty();
                });
                if (stop_requested.load() || !healthy.load()) break;
                frame = std::move(queue.front());
                queue.pop_front();
            }
            if (!frame.payload || frame.payload->empty()) continue;

            const int32_t bias_ms = delay_bias_ms.load();
            const int64_t delta_90k = sync::MasterMediaClock::us_to_90khz(
                static_cast<int64_t>(bias_ms) * 1000LL);
            int64_t adj_pts = std::max<int64_t>(0, static_cast<int64_t>(frame.base_pts_90khz) + delta_90k);

            // 动态下调主延迟或单设备偏置时，跳过与已发送音频时间重叠的帧，杜绝 PTS 倒流且不消耗 CC/RTP Seq
            if (last_pts_90k >= 0 && adj_pts < last_pts_90k + min_frame_step_90k) {
                dropped_frames.fetch_add(1);
                force_pat_pmt.store(true);
                continue;
            }

            // 保存打包前状态：若首字节即发生写超时，无损回滚 CC 与 RTP Seq，确保恢复帧在线上 100% 连续无断号
            const TsPacketizer saved_pack = packetizer;
            const uint16_t saved_seq = rtp_seq;
            const bool need_pat = force_pat_pmt.exchange(false);

            auto ts_bytes = packetizer.packetize_audio_frame(
                frame.payload->data(), frame.payload->size(), static_cast<uint64_t>(adj_pts), need_pat);
            const uint32_t rtp_ts = static_cast<uint32_t>(static_cast<uint64_t>(adj_pts) & 0xFFFFFFFFULL);

            wire_batch.clear();
            const size_t pkt_cnt = append_rtp_batch(wire_batch, ts_bytes, rtp_seq, rtp_ts);

            if (stop_requested.load()) break;
            const auto send_res = connection->write(wire_batch.data(), wire_batch.size(), 120, &stop_requested);
            if (send_res == net::SendAllStatus::Ok) {
                consecutive_timeouts = 0;
                last_pts_90k = adj_pts;
                rtp_seq_snap.store(rtp_seq);
                packets_sent.fetch_add(pkt_cnt);
                bytes_sent.fetch_add(wire_batch.size());
            } else if (send_res == net::SendAllStatus::TimeoutClean && !stop_requested.load()) {
                // 尚未向 TCP 流写入任何字节，流帧边界 100% 完好：回滚序列号与 CC，整帧安全丢弃
                packetizer = saved_pack;
                rtp_seq = saved_seq;
                force_pat_pmt.store(true);
                dropped_frames.fetch_add(1);
                if (++consecutive_timeouts >= 5) {
                    healthy.store(false);
                    break;
                }
            } else {
                // 连接已断，或发生半包写出后持续超时（TCP 帧边界已损坏），立即隔离该目标绝不继续发脏包
                healthy.store(false);
                break;
            }
        }
        if (!healthy.load() && !stop_requested.load()) {
            connection->abort();
            if (on_failure) on_failure();
        }
    });
}

void MultiAudioSender::ActiveTarget::stop_and_join() {
    {
        std::lock_guard<std::mutex> lock(q_mu);
        stop_requested.store(true);
        healthy.store(false);
        queue.clear();
    }
    if (owns_socket && tcp_data_sock != kInvalidSocket) {
        net::shutdown_socket(tcp_data_sock);
    }
    q_cv.notify_all();
    if (writer_th.joinable()) writer_th.join();
    if (owns_socket && tcp_data_sock != kInvalidSocket) {
        connection.reset();
        tcp_data_sock = kInvalidSocket;
    }
}

MultiAudioSender::~MultiAudioSender() {
    clear_all();
}

void MultiAudioSender::set_stream_type(TsAudioStreamType type) {
    std::lock_guard<std::mutex> lock(mu_);
    stream_type_ = type;
}

void MultiAudioSender::add_or_update_target(
    const std::string& device_id,
    const std::string& peer_ip,
    SocketHandle data_sock,
    int32_t delay_bias_ms,
    bool owns_socket,
    uint64_t session_id,
    std::shared_ptr<net::ConnectionWriter> connection,
    std::function<void()> on_failure) {
    std::shared_ptr<ActiveTarget> replaced;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = targets_.find(device_id);
        if (it != targets_.end()) {
            // 防止过期的旧会话回写覆盖新会话目标
            if (session_id != 0 && it->second->session_id != 0 && session_id < it->second->session_id) {
                if (!connection && owns_socket && data_sock != kInvalidSocket) net::close_socket(data_sock);
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
        node->connection = connection ? std::move(connection)
                                      : std::make_shared<net::ConnectionWriter>(data_sock, owns_socket);
        node->on_failure = std::move(on_failure);
        node->stream_type = stream_type_;
        node->delay_bias_ms.store(std::clamp(delay_bias_ms, -300, 300));
        node->healthy.store(data_sock != kInvalidSocket);
        if (data_sock != kInvalidSocket) {
            node->start_writer();
        }
        targets_[device_id] = std::move(node);
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
        return target.healthy.load() && target.tcp_data_sock != kInvalidSocket;
    });
}

size_t MultiAudioSender::append_rtp_batch(std::vector<uint8_t>& wire, const std::vector<uint8_t>& ts,
                                          uint16_t& sequence, uint32_t timestamp) {
    constexpr size_t chunk_max = kMaxTsPerRtp * kTsPktSize;
    size_t count = 0;
    size_t offset = 0;
    auto append = [&](size_t len) {
        auto packet = build_rtp_over_tcp_packet(sequence++, timestamp, ts.data() + offset,
                                                len, 0xDEADBEEFU, offset + len == ts.size());
        wire.insert(wire.end(), packet.begin(), packet.end());
        offset += len;
        ++count;
    };

    // Keep PSI/PCR in its own RTP frame when it would otherwise split the PES earlier.
    if (ts.size() > chunk_max && ts[0] == 0x47 && (ts[1] & 0x1F) == 0 && ts[2] == 0) {
        append(3 * kTsPktSize);
    }
    while (offset < ts.size()) append(std::min(chunk_max, ts.size() - offset));
    return count;
}

std::vector<uint8_t> MultiAudioSender::build_rtp_over_tcp_packet(
    uint16_t rtp_seq,
    uint32_t rtp_ts_90khz,
    const uint8_t* ts_bytes,
    size_t ts_len,
    uint32_t ssrc,
    bool marker) {
    const uint16_t rtp_len = static_cast<uint16_t>(12 + ts_len);
    std::vector<uint8_t> pkt(4 + rtp_len);
    pkt[0] = 0x24;
    pkt[1] = 0x00;
    pkt[2] = static_cast<uint8_t>((rtp_len >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(rtp_len & 0xFF);

    pkt[4] = 0x80;
    pkt[5] = marker ? 0xA1 : 0x21;
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
    uint32_t /*base_rtp_ts_90khz*/) {
    if (!audio_payload || payload_len == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (targets_.empty()) return;

    // 共享不可变载荷，零拷贝入队各设备独立发送通道
    auto shared_payload = std::make_shared<const std::vector<uint8_t>>(audio_payload, audio_payload + payload_len);

    for (auto& [_, node] : targets_) {
        if (!node->healthy.load() || node->stop_requested.load() || node->tcp_data_sock == kInvalidSocket) {
            continue;
        }
        {
            std::lock_guard<std::mutex> qlk(node->q_mu);
            while (node->queue.size() >= kMaxQueuedFramesPerTarget) {
                node->queue.pop_front(); // 打包前淘汰旧帧，线上 CC 与 RTP Seq 保持连续不断号
                node->dropped_frames.fetch_add(1);
                node->force_pat_pmt.store(true);
            }
            node->queue.push_back(QueuedFrame{shared_payload, base_pts_90khz});
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
        snap.rtp_seq = n->rtp_seq_snap.load();
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
