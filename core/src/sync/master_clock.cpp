#include "sync/master_clock.h"

#include <algorithm>
#include <chrono>
#include <vector>

#if defined(__linux__)
  #include <time.h>
#elif defined(__APPLE__)
  #include <mach/mach_time.h>
#endif

namespace miplay::sync {

uint64_t monotonic_now_us() {
#if defined(__linux__)
    struct timespec ts{};
#if defined(__ANDROID__)
    clock_gettime(CLOCK_BOOTTIME, &ts); // Android elapsedRealtimeNanos() includes device suspend.
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
#else
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#endif
}

void MasterMediaClock::reset(uint32_t sample_rate, uint32_t base_play_delay_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    base_play_delay_ms_ = base_play_delay_ms;
    origin_us_ = monotonic_now_us();
    total_samples_ = 0;
}

uint64_t MasterMediaClock::session_origin_us() const {
    std::lock_guard<std::mutex> lock(mu_);
    return origin_us_;
}

uint32_t MasterMediaClock::base_play_delay_ms() const {
    std::lock_guard<std::mutex> lock(mu_);
    return base_play_delay_ms_;
}

void MasterMediaClock::set_base_play_delay_ms(uint32_t delay_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    base_play_delay_ms_ = delay_ms;
}

MasterMediaClock::FrameTimestamps MasterMediaClock::next_frame_timestamps(uint32_t frame_samples, bool include_delay) {
    std::lock_guard<std::mutex> lock(mu_);
    // 严格按采样数推进，确保 48kHz / 1024 samples 的 90kHz 增量精准等于 1920
    const uint64_t rel_us = (total_samples_ * 1000000ULL) / sample_rate_;
    const uint64_t delay_us = include_delay ? static_cast<uint64_t>(base_play_delay_ms_) * 1000 : 0;
    const uint64_t pts_us = rel_us + delay_us;
    const uint64_t pts_90k = ((total_samples_ * 90000ULL) / sample_rate_) +
                             ((delay_us * 90ULL) / 1000ULL);

    FrameTimestamps out;
    out.linear_pts_90khz = pts_90k;
    out.pts_90khz = pts_90k & ((1ULL << 33) - 1);
    out.rtp_ts_90khz = static_cast<uint32_t>(pts_90k & 0xFFFFFFFFULL);
    out.play_target_us = origin_us_ + pts_us;

    total_samples_ += frame_samples;
    return out;
}

void MasterMediaClock::skip_samples(uint64_t samples) {
    std::lock_guard<std::mutex> lock(mu_);
    total_samples_ += samples;
}

void MasterMediaClock::catch_up_to_now(uint32_t frame_samples, uint64_t now_us) {
    if (frame_samples == 0) return;
    if (now_us == 0) now_us = monotonic_now_us();
    std::lock_guard<std::mutex> lock(mu_);
    if (origin_us_ == 0 || now_us <= origin_us_) return;
    const uint64_t elapsed_us = now_us - origin_us_;
    const uint64_t expected_samples = (elapsed_us * sample_rate_) / 1000000ULL;
    // 稳态允许 2 帧以内微抖；一旦滞后或超前超过 2 帧，立即按整帧边界双向锚定墙钟，防止 PTS 与 TimerService 漂移
    const uint64_t tolerance = static_cast<uint64_t>(frame_samples) * 2ULL;
    if (expected_samples > total_samples_ + tolerance) {
        total_samples_ = (expected_samples / frame_samples) * frame_samples;
    } else if (total_samples_ > expected_samples + tolerance) {
        total_samples_ = (expected_samples / frame_samples) * frame_samples;
    }
}

int64_t MasterMediaClock::us_to_90khz(int64_t delta_us) {
    return (delta_us * 90) / 1000;
}

TrendlineEstimator::TrendlineEstimator(size_t window_size)
    : window_size_(std::max<size_t>(4, window_size)) {}

void TrendlineEstimator::reset() {
    samples_.clear();
    base_t_us_ = 0;
    last_ = {};
}

TrendlineEstimator::Estimate TrendlineEstimator::update(
    uint64_t t_local_us, double raw_offset_us, double rtt_us) {
    if (base_t_us_ == 0) base_t_us_ = t_local_us;
    samples_.push_back({t_local_us, raw_offset_us, rtt_us});
    if (samples_.size() > window_size_) {
        samples_.pop_front();
    }

    // 选取 RTT 最小的 Top-25% (至少 2 个) 样本过滤 Wi-Fi 非对称排队尖峰
    std::vector<Sample> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Sample& a, const Sample& b) { return a.rtt_us < b.rtt_us; });
    size_t keep = std::max<size_t>(1, sorted.size() / 4);
    if (sorted.size() >= 2) keep = std::max<size_t>(2, keep);
    sorted.resize(std::min(keep, sorted.size()));

    double sum_x = 0.0, sum_y = 0.0, sum_rtt = 0.0;
    for (const auto& s : sorted) {
        double x = static_cast<double>(s.t_us - base_t_us_) * 1e-6; // seconds
        sum_x += x;
        sum_y += s.offset_us;
        sum_rtt += s.rtt_us;
    }
    const double n = static_cast<double>(sorted.size());
    const double mean_x = sum_x / n;
    const double mean_y = sum_y / n;

    double num = 0.0, den = 0.0;
    for (const auto& s : sorted) {
        double dx = static_cast<double>(s.t_us - base_t_us_) * 1e-6 - mean_x;
        double dy = s.offset_us - mean_y;
        num += dx * dy;
        den += dx * dx;
    }
    double slope_us_per_s = (den > 1e-6) ? (num / den) : 0.0;
    double cur_x = static_cast<double>(t_local_us - base_t_us_) * 1e-6;
    double est_offset = mean_y + slope_us_per_s * (cur_x - mean_x);

    last_.offset_us = est_offset;
    last_.rtt_ms = (sum_rtt / n) / 1000.0;
    last_.drift_ppm = slope_us_per_s; // 1 us / 1 s == 1 ppm
    last_.sample_count = samples_.size();
    return last_;
}

} // namespace miplay::sync
