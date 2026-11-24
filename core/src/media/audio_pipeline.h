#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay::media {

enum class AudioSourceMode {
    TestTone = 0,       // 内置柔和和弦测试音 (48kHz Stereo)
    WavFile = 1,        // 内置无损 WAV 文件播放器
    SystemLoopback = 2, // 跨平台系统声卡全局环回抓音 (Linux PipeWire/Pulse / Win WASAPI / macOS SCK)
    ExternalPcm = 3     // 外部推入 PCM
};

struct AudioTrackMeta {
    std::string title = "MiPlay Multi-Room";
    std::string artist = "miplay-core";
    std::string album = "Lossless Stream";
    uint64_t duration_ms = 0;
    uint64_t position_ms = 0;
};

using PcmFrameCallback = std::function<void(const int16_t* stereo_pcm, size_t frame_samples)>;
using SpectrumCallback = std::function<void(const std::array<float, 16>& levels)>;

// 跨平台统一音源采集与前处理管线 (48kHz / 44.1kHz Stereo Int16 PCM 固定帧节拍输出)
class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    void set_source_mode(AudioSourceMode mode, const std::string& param = "");
    AudioSourceMode source_mode() const { return mode_.load(); }
    std::string source_param() const;

    bool start(uint32_t sample_rate, uint32_t frame_samples,
               PcmFrameCallback pcm_cb, SpectrumCallback spec_cb = nullptr);
    void stop();
    bool is_running() const { return running_.load(); }

    void push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples);
    AudioTrackMeta current_meta() const;

private:
    void worker_loop();
    bool load_wav_file(const std::string& path);
    void compute_spectrum(const int16_t* stereo_pcm, size_t frames);

    std::atomic<AudioSourceMode> mode_{AudioSourceMode::TestTone};
    std::atomic<bool> running_{false};
    uint32_t sample_rate_ = 48000;
    uint32_t frame_samples_ = 480; // 10ms @ 48kHz (低延迟 LPCM 基准帧)

    mutable std::mutex param_mu_;
    std::string source_param_;
    AudioTrackMeta meta_;
    std::vector<int16_t> wav_pcm_;
    size_t wav_cursor_ = 0;

    std::mutex ext_mu_;
    std::vector<int16_t> ext_ring_;

    PcmFrameCallback pcm_cb_;
    SpectrumCallback spec_cb_;
    std::thread worker_;
};

} // namespace miplay::media
