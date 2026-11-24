#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
};

struct LoopbackSourceInfo {
    std::string id;
    std::string label;
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

    static void recover_playback_route();
    static bool is_loopback_supported();
    static std::vector<LoopbackSourceInfo> list_loopback_sources();

    // 测试注入：替换 Linux 环回子进程执行命令（仅用于无输出/阻塞退出时限单测）
    void set_loopback_command_override_for_test(std::vector<std::string> argv);

    bool start(uint32_t sample_rate, uint32_t frame_samples,
               PcmFrameCallback pcm_cb, SpectrumCallback spec_cb = nullptr,
               std::function<bool()> has_streaming_receiver = nullptr);
    void stop();
    bool is_running() const { return running_.load(); }

    void push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples);
    AudioTrackMeta current_meta() const;

    // 严格 RIFF/WAV 解析与转换（支持 16/24-bit PCM、32-bit Float、单/双声道、任意常见采样率重采样到 target_rate）
    static bool decode_wav_file(const std::string& path,
                                uint32_t target_rate,
                                std::vector<int16_t>& out_stereo_pcm,
                                AudioTrackMeta& out_meta,
                                std::string& out_error);

private:
    void worker_loop();
    void stop_loopback_process();
    void compute_spectrum(const int16_t* stereo_pcm, size_t frames);
    void spectrum_loop();

    std::atomic<AudioSourceMode> mode_{AudioSourceMode::TestTone};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> source_gen_{1};
    uint32_t sample_rate_ = 48000;
    uint32_t frame_samples_ = 480;

    mutable std::mutex param_mu_;
    std::string source_param_;
    std::string last_error_;
    AudioTrackMeta meta_;
    std::vector<int16_t> wav_pcm_;
    std::string wav_path_;
    uint32_t wav_decoded_rate_ = 48000;
    size_t wav_cursor_ = 0;
    std::vector<std::string> test_loopback_argv_;

    std::mutex ext_mu_;
    std::vector<int16_t> ext_ring_;

#if defined(__linux__)
    std::mutex proc_mu_;
    int loopback_fd_ = -1;
    int loopback_pid_ = -1;
#endif

    PcmFrameCallback pcm_cb_;
    SpectrumCallback spec_cb_;
    std::function<bool()> has_streaming_receiver_;
    std::thread worker_;
    std::thread spectrum_worker_;
    std::mutex spectrum_mu_;
    std::condition_variable spectrum_cv_;
    std::optional<std::array<float, 16>> pending_spectrum_;
};

} // namespace miplay::media
