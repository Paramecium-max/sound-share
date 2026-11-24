#pragma once

#include "media/ts_packetizer.h"
#include "platform/net_platform.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace miplay::media {

struct MirrorTargetNode {
    std::string device_id;
    std::string peer_ip;
    SocketHandle tcp_data_sock = kInvalidSocket; // MultiPort 绑定的专用音频数据 TCP Socket
    uint16_t rtp_seq = 0;
    int32_t delay_bias_ms = 0;                   // 单设备延迟微调 (-300..+300ms)
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    bool healthy = true;
};

// 全屋一源多发 RTP-over-TCP 分发引擎 (支持热插拔动态加减设备、独立微调延迟、故障隔离)
class MultiAudioSender {
public:
    MultiAudioSender() = default;
    ~MultiAudioSender();

    void set_stream_type(TsAudioStreamType type);

    // 动态添加或替换某台设备的音频数据通道 (零中断影响其他设备)
    void add_or_update_target(const std::string& device_id,
                              const std::string& peer_ip,
                              SocketHandle data_sock,
                              int32_t delay_bias_ms = 0);

    void remove_target(const std::string& device_id);
    void set_target_delay_bias(const std::string& device_id, int32_t delay_bias_ms);
    void clear_all();
    size_t active_target_count() const;

    // 打包并一源多发同一帧音频到全屋所有设备 (完全一致的主时钟 PTS + 可选单设备微调)
    void broadcast_audio_frame(
        const uint8_t* audio_payload,
        size_t payload_len,
        uint64_t base_pts_90khz,
        uint32_t base_rtp_ts_90khz);

    // 构造单个 RFC 2326 RTP-over-TCP 帧 (供单测与复用)
    static std::vector<uint8_t> build_rtp_over_tcp_packet(
        uint16_t rtp_seq,
        uint32_t rtp_ts_90khz,
        const uint8_t* ts_bytes,
        size_t ts_len,
        uint32_t ssrc = 0xDEADBEEFU);

    std::vector<MirrorTargetNode> snapshot_nodes() const;

private:
    mutable std::mutex mu_;
    TsAudioStreamType stream_type_ = TsAudioStreamType::WfdLpcm;
    TsPacketizer shared_packetizer_{TsAudioStreamType::WfdLpcm};
    std::map<std::string, MirrorTargetNode> targets_;
    std::map<std::string, TsPacketizer> biased_packetizers_; // 仅当设备有非零 delay_bias 时使用
};

} // namespace miplay::media
