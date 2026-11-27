#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace miplay::sync {

// 平台单调高精度微秒时钟 (对齐红米 K90 TimerService 时间轴)
uint64_t monotonic_now_us();

// 统一全屋主时钟：负责生成单调递增的 90kHz PTS 与会话基准 TimeOffset
class MasterMediaClock {
public:
    MasterMediaClock() = default;

    // 启动新一轮全屋同步会话，锁定基准 TimeOffset (us) 与采样率
    void reset(uint32_t sample_rate, uint32_t base_play_delay_ms);

    uint64_t session_origin_us() const;
    uint32_t base_play_delay_ms() const;
    void set_base_play_delay_ms(uint32_t delay_ms);

    // 推进 frame_samples 个音频采样，返回这一帧对应的 90kHz PTS (33-bit) 与 RTP timestamp (32-bit)
    struct FrameTimestamps {
        uint64_t linear_pts_90khz = 0; // 单调无回绕 90kHz 时间戳
        uint64_t pts_90khz = 0;        // 33-bit MPEG-TS PES PTS
        uint32_t rtp_ts_90khz = 0;     // 32-bit RTP Timestamp
        uint64_t play_target_us = 0;   // 绝对物理目标播放时刻 (us, 主时钟域)
    };
    FrameTimestamps next_frame_timestamps(uint32_t frame_samples, bool include_delay = true);
    void skip_samples(uint64_t samples);

    // 当发生设备重路由、授权等待或进程挂起导致样本时钟明显落后实时墙钟时，按整帧倍数追齐到当前墙钟
    void catch_up_to_now(uint32_t frame_samples, uint64_t now_us = 0);

    // 按绝对微秒偏移转换为 90kHz 增量 (用于单设备延迟微调)
    static int64_t us_to_90khz(int64_t delta_us);

private:
    mutable std::mutex mu_;
    uint64_t origin_us_ = 0;
    uint64_t total_samples_ = 0;
    uint32_t sample_rate_ = 48000;
    uint32_t base_play_delay_ms_ = 300;
};

// 滑动窗口 Min-RTT 过滤 + 在线线性回归漂移估算器 (对应系统 MiPlay_TrendlineEstimator)
class TrendlineEstimator {
public:
    explicit TrendlineEstimator(size_t window_size = 16);

    struct Estimate {
        double offset_us = 0.0;
        double rtt_ms = 0.0;
        double drift_ppm = 0.0;
        size_t sample_count = 0;
    };

    Estimate update(uint64_t t_local_us, double raw_offset_us, double rtt_us);
    void reset();

private:
    struct Sample {
        uint64_t t_us;
        double offset_us;
        double rtt_us;
    };
    size_t window_size_;
    std::deque<Sample> samples_;
    uint64_t base_t_us_ = 0;
    Estimate last_{};
};

} // namespace miplay::sync
