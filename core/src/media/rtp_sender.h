#pragma once

#include "media/ts_packetizer.h"
#include "platform/net_platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay::media {

struct MirrorTargetNode {
    std::string device_id;
    uint64_t session_id = 0;
    std::string peer_ip;
    SocketHandle tcp_data_sock = kInvalidSocket; // MultiPort 绑定的专用音频数据 TCP Socket
    uint16_t rtp_seq = 0;
    int32_t delay_bias_ms = 0;                   // 单设备延迟微调 (-300..+300ms)
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t dropped_frames = 0;
    bool healthy = true;
    bool owns_socket = true;
};

// 全屋一源多发 RTP-over-TCP 分发引擎：
// - 每设备独立有界队列 + 独立 TsPacketizer（出队发送前才分配 CC 与 RTP Seq，淘汰旧帧零断号）
// - 首字节超时安全跳过整帧，半包超时严格隔离防 TCP 帧边界错位
// - PTS 单调防重叠保护，动态调小延迟不倒流
class MultiAudioSender {
public:
    static constexpr size_t kMaxQueuedFramesPerTarget = 24;

    MultiAudioSender() = default;
    ~MultiAudioSender();

    void set_stream_type(TsAudioStreamType type);

    // 动态添加或替换某台设备的音频数据通道 (session_id != 0 时按唯一会话标识防旧覆盖新)
    void add_or_update_target(const std::string& device_id,
                              const std::string& peer_ip,
                              SocketHandle data_sock,
                              int32_t delay_bias_ms = 0,
                              bool owns_socket = true,
                              uint64_t session_id = 0);

    // session_id == 0 表示按 device_id 无条件删除；非 0 表示仅当当前目标 session_id 匹配时删除
    void remove_target(const std::string& device_id, uint64_t session_id = 0);
    void set_target_delay_bias(const std::string& device_id, int32_t delay_bias_ms);
    void clear_all();
    size_t active_target_count() const;
    bool has_streaming_target() const;

    // 入队原始音频帧载荷与基准 90kHz PTS（不在采集线程执行 TS 拆包或等待慢 Socket）
    void broadcast_audio_frame(
        const uint8_t* audio_payload,
        size_t payload_len,
        uint64_t base_pts_90khz,
        uint32_t base_rtp_ts_90khz);

    static std::vector<uint8_t> build_rtp_over_tcp_packet(
        uint16_t rtp_seq,
        uint32_t rtp_ts_90khz,
        const uint8_t* ts_bytes,
        size_t ts_len,
        uint32_t ssrc = 0xDEADBEEFU,
        bool marker = true);

    std::vector<MirrorTargetNode> snapshot_nodes() const;

private:
    struct QueuedFrame {
        std::shared_ptr<const std::vector<uint8_t>> payload;
        uint64_t base_pts_90khz = 0;
    };

    struct ActiveTarget {
        std::string device_id;
        uint64_t session_id = 0;
        std::string peer_ip;
        SocketHandle tcp_data_sock = kInvalidSocket;
        bool owns_socket = true;
        TsAudioStreamType stream_type = TsAudioStreamType::WfdLpcm;
        std::atomic<int32_t> delay_bias_ms{0};
        std::atomic<uint64_t> packets_sent{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> dropped_frames{0};
        std::atomic<bool> healthy{true};
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> force_pat_pmt{true};
        std::atomic<uint16_t> rtp_seq_snap{0};

        std::mutex q_mu;
        std::condition_variable q_cv;
        std::deque<QueuedFrame> queue;
        std::thread writer_th;

        void start_writer();
        void stop_and_join();
    };

    mutable std::mutex mu_;
    TsAudioStreamType stream_type_ = TsAudioStreamType::WfdLpcm;
    std::map<std::string, std::shared_ptr<ActiveTarget>> targets_;
};

} // namespace miplay::media
