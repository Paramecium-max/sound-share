#pragma once

#include "media/media_control.h"
#include "media/per_app_route.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace miplay::media {

enum class AudioSourceMode {
    TestTone = 0,       // 内置柔和和弦测试音 (48kHz Stereo)
    WavFile = 1,        // 内置无损 WAV 文件播放器
    SystemLoopback = 2, // 跨平台系统声卡全局环回抓音 (Linux PipeWire/Pulse / Win WASAPI)
    ExternalPcm = 3     // 外部推入 PCM
};

struct AudioTrackMeta {
    std::string title = "MiPlay Multi-Room";
    std::string artist = "miplay-core";
    std::string album = "Lossless Stream";
    uint64_t duration_ms = 0;
    uint64_t position_ms = 0;
    std::string lyrics;
    std::string lyric_line;
    bool paused = false;
};

struct LoopbackSourceInfo {
    std::string id;
    std::string label;
};

// Continuous interpolation with a streaming low-pass filter before downsampling.
class StreamLinearResampler {
public:
    void reset(uint32_t src_rate, uint32_t dst_rate);
    void process_interleaved_stereo(const int16_t* in_stereo, size_t in_frames, std::vector<int16_t>& out_stereo);

private:
    uint32_t src_rate_ = 48000;
    uint32_t dst_rate_ = 48000;
    uint64_t phase_ = 0;
    bool has_prev_ = false;
    std::array<double, 2> previous_{};
    static constexpr size_t kFilterTaps = 95;
    std::array<double, kFilterTaps> filter_{};
    std::array<std::array<double, 2>, kFilterTaps> history_{};
    size_t history_pos_ = 0;
    size_t warmup_ = 0;
    std::array<double, 2> initial_{};
};

enum class CaptureSampleFormat { Float32, Pcm16, Pcm24, Pcm32 };

class StereoDownmixer {
public:
    StereoDownmixer(CaptureSampleFormat format, size_t channels, uint32_t speaker_mask, size_t stride);
    void convert(const uint8_t* input, size_t frames, int16_t* output) const;
private:
    CaptureSampleFormat format_;
    size_t stride_;
    size_t sample_bytes_;
    std::vector<std::array<double, 2>> weights_;
};

// 环回采集平滑弹性抖动缓冲（Pre-roll 水位建立 + ±1 样本连续相位插值时钟漂移补偿 + 欠载尾样本衰减 + 溢出交叉淡入淡出）
class SmoothLoopbackFifo {
public:
    void reset(uint32_t sample_rate = 48000, size_t frame_samples = 480);
    void push_interleaved_stereo(const int16_t* stereo_pcm, size_t frames);
    void pop_smooth_frame(int16_t* dst_stereo, size_t out_frames);

    size_t available_frames() const { return ring_.size() / 2; }
    bool is_primed() const { return primed_; }

private:
    uint32_t sample_rate_ = 48000;
    size_t frame_samples_ = 480;
    size_t preroll_frames_ = 1200;
    size_t target_frames_ = 1920;
    size_t low_water_frames_ = 768;
    size_t high_water_frames_ = 3120;
    size_t max_frames_ = 9600;
    bool primed_ = false;
    bool splice_pending_ = false;
    int16_t last_l_ = 0;
    int16_t last_r_ = 0;
    std::deque<int16_t> ring_;
};

using PcmFrameCallback = std::function<void(const int16_t* stereo_pcm, size_t frame_samples)>;
using SpectrumCallback = std::function<void(const std::array<float, 16>& levels)>;

// 跨平台统一音源采集与前处理管线 (48kHz / 44.1kHz Stereo Int16 PCM 固定帧节拍输出)
class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    // 切换音源并校验输入；失败时保留上一个有效状态并返回 false，err_out 填充错误说明
    bool set_source_mode(AudioSourceMode mode, const std::string& param = "", std::string* err_out = nullptr);
    AudioSourceMode source_mode() const { return mode_.load(); }
    std::string source_param() const;
    std::string last_error() const;
    bool is_process_loopback_fallback() const { return process_loopback_fallback_.load(); }
    void set_mute_local_app(bool mute);
    bool is_mute_local_app() const { return mute_local_app_.load(); }
    SilentRouteStatus mute_local_status() const;

    static void recover_playback_route();
    static bool is_loopback_supported();
    static std::vector<LoopbackSourceInfo> list_loopback_sources();

    // 测试注入：替换 Linux 环回子进程执行命令（仅用于无输出/阻塞退出时限单测）
    void set_loopback_command_override_for_test(std::vector<std::string> argv);

    bool start(uint32_t sample_rate, uint32_t frame_samples,
               PcmFrameCallback pcm_cb, SpectrumCallback spec_cb = nullptr,
               std::function<bool()> has_streaming_receiver = nullptr,
               std::function<void(uint64_t)> skip_samples = nullptr,
               uint64_t session_origin_us = 0);

    static uint64_t skipped_capture_samples(uint64_t scheduled_samples, uint32_t sample_rate,
                                            uint32_t frame_samples, uint64_t elapsed_us);
    void stop();
    bool is_running() const { return running_.load(); }

    void push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples);
    AudioTrackMeta current_meta() const;
    void set_wav_paused(bool paused);
    bool is_wav_paused() const { return wav_paused_.load(); }
    void seek_wav_ms(uint64_t position_ms);
    void set_wav_lrc_text(const std::string& lrc_text);

    // 严格 RIFF/WAV 解析与转换（支持 16/24-bit PCM、32-bit Float、单/双声道、任意常见采样率重采样到 target_rate）
    static bool decode_wav_file(const std::string& path,
                                uint32_t target_rate,
                                std::vector<int16_t>& out_stereo_pcm,
                                AudioTrackMeta& out_meta,
                                std::string& out_error);

private:
    void worker_loop();
    void stop_loopback_process();
    void queue_spectrum(const int16_t* stereo_pcm, size_t frames);
    void spectrum_loop();

    std::atomic<AudioSourceMode> mode_{AudioSourceMode::TestTone};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> source_gen_{1};
    uint32_t sample_rate_ = 48000;
    uint32_t frame_samples_ = 480;

    std::recursive_mutex publication_mu_;
    mutable std::mutex param_mu_;
    std::string source_param_;
    std::string last_error_;
    AudioTrackMeta meta_;
    std::vector<int16_t> wav_pcm_;
    std::string wav_path_;
    uint32_t wav_decoded_rate_ = 48000;
    size_t wav_cursor_ = 0;
    std::atomic<bool> wav_paused_{false};
    LrcDocument wav_lrc_;
    std::vector<std::string> test_loopback_argv_;

    std::mutex ext_mu_;
    SmoothLoopbackFifo ext_fifo_;
    std::atomic<bool> process_loopback_fallback_{false};
    std::atomic<bool> mute_local_app_{false};
    mutable std::mutex route_status_mu_;
    SilentRouteStatus mute_local_status_{};

#if defined(__linux__) && !defined(__ANDROID__)
    // Exclusively owned by the audio worker.
    int loopback_fd_ = -1;
    int loopback_pid_ = -1;
#endif

    PcmFrameCallback pcm_cb_;
    SpectrumCallback spec_cb_;
    std::function<bool()> has_streaming_receiver_;
    std::function<void(uint64_t)> skip_samples_;
    uint64_t session_origin_us_ = 0;
    std::thread worker_;
    std::thread spectrum_worker_;
    std::mutex spectrum_mu_;
    std::condition_variable spectrum_cv_;
    static constexpr size_t kSpectrumSamples = 1024;
    struct SpectrumFrame {
        std::array<int16_t, kSpectrumSamples * 2> pcm{};
        size_t frames = 0;
    };
    std::optional<SpectrumFrame> pending_spectrum_;
};

} // namespace miplay::media
