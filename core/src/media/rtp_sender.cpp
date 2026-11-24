#include "media/rtp_sender.h"
#include "sync/master_clock.h"

#include <algorithm>
#include <cstring>

namespace miplay::media {

namespace {
constexpr size_t kTsPktSize = 188;
constexpr size_t kMaxTsPerRtp = 7; // 7 * 188 = 1316 bytes < MTU 1500
} // namespace

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
    int32_t delay_bias_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = targets_.find(device_id);
    if (it != targets_.end() && it->second.tcp_data_sock != kInvalidSocket &&
        it->second.tcp_data_sock != data_sock) {
        net::close_socket(it->second.tcp_data_sock);
    }
    MirrorTargetNode node;
    node.device_id = device_id;
    node.peer_ip = peer_ip;
    node.tcp_data_sock = data_sock;
    node.delay_bias_ms = delay_bias_ms;
    node.healthy = (data_sock != kInvalidSocket);
    targets_[device_id] = std::move(node);
    if (delay_bias_ms != 0) {
        biased_packetizers_[device_id] = TsPacketizer(stream_type_);
    }
}

void MultiAudioSender::remove_target(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = targets_.find(device_id);
    if (it != targets_.end()) {
        if (it->second.tcp_data_sock != kInvalidSocket) {
            net::close_socket(it->second.tcp_data_sock);
        }
        targets_.erase(it);
    }
    biased_packetizers_.erase(device_id);
}

void MultiAudioSender::set_target_delay_bias(const std::string& device_id, int32_t delay_bias_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    delay_bias_ms = std::clamp(delay_bias_ms, -300, 300);
    auto it = targets_.find(device_id);
    if (it != targets_.end()) {
        it->second.delay_bias_ms = delay_bias_ms;
        if (delay_bias_ms != 0 && biased_packetizers_.find(device_id) == biased_packetizers_.end()) {
            biased_packetizers_[device_id] = TsPacketizer(stream_type_);
        }
    }
}

void MultiAudioSender::clear_all() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [_, node] : targets_) {
        if (node.tcp_data_sock != kInvalidSocket) {
            net::close_socket(node.tcp_data_sock);
        }
    }
    targets_.clear();
    biased_packetizers_.clear();
}

size_t MultiAudioSender::active_target_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return targets_.size();
}

std::vector<uint8_t> MultiAudioSender::build_rtp_over_tcp_packet(
    uint16_t rtp_seq,
    uint32_t rtp_ts_90khz,
    const uint8_t* ts_bytes,
    size_t ts_len,
    uint32_t ssrc) {
    const uint16_t rtp_len = static_cast<uint16_t>(12 + ts_len);
    std::vector<uint8_t> pkt(4 + rtp_len);
    // RFC 2326 Interleaved frame header: '$' + channel(0) + u16be len
    pkt[0] = 0x24;
    pkt[1] = 0x00;
    pkt[2] = static_cast<uint8_t>((rtp_len >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(rtp_len & 0xFF);

    // 12-byte RTP header (V=2, M=1, PT=33 MP2T, SSRC=0xDEADBEEF)
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

    // 零重复打包：所有无独立延迟偏置的设备共享同一份 MPEG-TS 包序列
    std::vector<uint8_t> shared_ts;
    bool built_shared = false;

    for (auto& [id, node] : targets_) {
        if (!node.healthy || node.tcp_data_sock == kInvalidSocket) continue;

        const std::vector<uint8_t>* ts_ptr = nullptr;
        std::vector<uint8_t> local_ts;
        uint32_t node_rtp_ts = base_rtp_ts_90khz;

        if (node.delay_bias_ms == 0) {
            if (!built_shared) {
                shared_ts = shared_packetizer_.packetize_audio_frame(
                    audio_payload, payload_len, base_pts_90khz);
                built_shared = true;
            }
            ts_ptr = &shared_ts;
        } else {
            int64_t delta_90k = sync::MasterMediaClock::us_to_90khz(
                static_cast<int64_t>(node.delay_bias_ms) * 1000LL);
            int64_t adj_pts = static_cast<int64_t>(base_pts_90khz) + delta_90k;
            if (adj_pts < 0) adj_pts = 0;
            node_rtp_ts = static_cast<uint32_t>(static_cast<uint64_t>(adj_pts) & 0xFFFFFFFFULL);
            auto& pack = biased_packetizers_[id];
            local_ts = pack.packetize_audio_frame(
                audio_payload, payload_len, static_cast<uint64_t>(adj_pts));
            ts_ptr = &local_ts;
        }

        // 每包最多 7 个 TS 包发送，单设备失败立即隔离不影响其他设备
        const size_t chunk_max = kMaxTsPerRtp * kTsPktSize;
        for (size_t off = 0; off < ts_ptr->size(); off += chunk_max) {
            size_t len = std::min(chunk_max, ts_ptr->size() - off);
            auto rtp = build_rtp_over_tcp_packet(
                node.rtp_seq++, node_rtp_ts, ts_ptr->data() + off, len);
            if (!net::send_all(node.tcp_data_sock, rtp.data(), rtp.size())) {
                node.healthy = false;
                break;
            }
            node.packets_sent++;
            node.bytes_sent += rtp.size();
        }
    }
}

std::vector<MirrorTargetNode> MultiAudioSender::snapshot_nodes() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MirrorTargetNode> out;
    out.reserve(targets_.size());
    for (const auto& [_, n] : targets_) out.push_back(n);
    return out;
}

} // namespace miplay::media
