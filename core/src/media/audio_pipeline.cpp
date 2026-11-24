#include "media/audio_pipeline.h"
#include "sync/master_clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
#endif

namespace miplay::media {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;

#ifdef _WIN32
// 原生 Windows WASAPI 系统全局声卡环回抓取器 (免装虚拟声卡，支持 Float32/Int16 自动重采样为目标采样率)
class WasapiLoopbackCapturer {
public:
    ~WasapiLoopbackCapturer() { stop(); }

    bool start(uint32_t target_rate) {
        stop();
        target_rate_ = target_rate;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_init_ = SUCCEEDED(hr);

        static const CLSID clsid_mmde = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
        static const IID iid_mmde     = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
        static const IID iid_ac       = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
        static const IID iid_acc      = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};

        if (FAILED(CoCreateInstance(clsid_mmde, nullptr, CLSCTX_ALL, iid_mmde,
                                    reinterpret_cast<void**>(&enumerator_)))) {
            stop(); return false;
        }
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_))) {
            stop(); return false;
        }
        if (FAILED(device_->Activate(iid_ac, CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&audio_client_)))) {
            stop(); return false;
        }
        if (FAILED(audio_client_->GetMixFormat(&mix_fmt_)) || !mix_fmt_) {
            stop(); return false;
        }
        src_rate_ = mix_fmt_->nSamplesPerSec;
        src_ch_ = mix_fmt_->nChannels;
        is_float_ = (mix_fmt_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix_fmt_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mix_fmt_->cbSize >= 22) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_fmt_);
            static const GUID kSubFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
            is_float_ = (std::memcmp(&ext->SubFormat, &kSubFloat, sizeof(GUID)) == 0);
        }

        // 100ms 缓冲，开启 AUDCLNT_STREAMFLAGS_LOOPBACK 全局环回
        REFERENCE_TIME hns_dur = 1000000;
        if (FAILED(audio_client_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                hns_dur, 0, mix_fmt_, nullptr))) {
            stop(); return false;
        }
        if (FAILED(audio_client_->GetService(iid_acc, reinterpret_cast<void**>(&capture_client_)))) {
            stop(); return false;
        }
        if (FAILED(audio_client_->Start())) {
            stop(); return false;
        }
        active_ = true;
        return true;
    }

    void stop() {
        active_ = false;
        if (audio_client_) { audio_client_->Stop(); audio_client_->Release(); audio_client_ = nullptr; }
        if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
        if (mix_fmt_) { CoTaskMemFree(mix_fmt_); mix_fmt_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (com_init_) { CoUninitialize(); com_init_ = false; }
        pcm_queue_.clear();
    }

    bool is_active() const { return active_; }

    // 填充 out_frames 帧 (stereo int16)
    void read_stereo_frame(int16_t* dst, size_t out_frames) {
        if (!active_) {
            std::memset(dst, 0, out_frames * 2 * sizeof(int16_t));
            return;
        }
        // 拉取所有已就绪的 WASAPI 环回包并转换/线性重采样为目标采样率立体声 Int16
        UINT32 pkt_len = 0;
        while (SUCCEEDED(capture_client_->GetNextPacketSize(&pkt_len)) && pkt_len > 0) {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_client_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr))) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
            const double ratio = static_cast<double>(src_rate_) / static_cast<double>(target_rate_);
            const size_t est_out = static_cast<size_t>(std::ceil(num_frames / ratio)) + 1;

            for (size_t o = 0; o < est_out; ++o) {
                double src_pos = o * ratio;
                size_t idx = static_cast<size_t>(src_pos);
                if (idx >= num_frames) break;

                int16_t l = 0, r = 0;
                if (!silent) {
                    if (is_float_ && mix_fmt_->wBitsPerSample == 32) {
                        const float* fptr = reinterpret_cast<const float*>(data) + idx * src_ch_;
                        float fl = std::clamp(fptr[0], -1.0f, 1.0f);
                        float fr = (src_ch_ > 1) ? std::clamp(fptr[1], -1.0f, 1.0f) : fl;
                        l = static_cast<int16_t>(fl * 32767.0f);
                        r = static_cast<int16_t>(fr * 32767.0f);
                    } else if (mix_fmt_->wBitsPerSample == 16) {
                        const int16_t* sptr = reinterpret_cast<const int16_t*>(data) + idx * src_ch_;
                        l = sptr[0];
                        r = (src_ch_ > 1) ? sptr[1] : l;
                    }
                }
                pcm_queue_.push_back(l);
                pcm_queue_.push_back(r);
            }
            capture_client_->ReleaseBuffer(num_frames);
        }

        const size_t need = out_frames * 2;
        if (pcm_queue_.size() >= need) {
            std::memcpy(dst, pcm_queue_.data(), need * sizeof(int16_t));
            pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + static_cast<ptrdiff_t>(need));
            // 避免队列积压超过 120ms
            const size_t max_q = target_rate_ * 2 / 8;
            if (pcm_queue_.size() > max_q) {
                pcm_queue_.erase(pcm_queue_.begin(),
                                 pcm_queue_.begin() + static_cast<ptrdiff_t>(pcm_queue_.size() - max_q));
            }
        } else {
            std::memset(dst, 0, need * sizeof(int16_t));
        }
    }

private:
    bool com_init_ = false;
    bool active_ = false;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    WAVEFORMATEX* mix_fmt_ = nullptr;
    uint32_t target_rate_ = 48000;
    uint32_t src_rate_ = 48000;
    uint16_t src_ch_ = 2;
    bool is_float_ = true;
    std::vector<int16_t> pcm_queue_;
};
#endif

} // namespace

AudioPipeline::~AudioPipeline() {
    stop();
}

void AudioPipeline::set_source_mode(AudioSourceMode mode, const std::string& param) {
    {
        std::lock_guard<std::mutex> lock(param_mu_);
        source_param_ = param;
        if (mode == AudioSourceMode::WavFile && !param.empty()) {
            load_wav_file(param);
        } else if (mode == AudioSourceMode::TestTone) {
            meta_.title = "全屋同步校准测试音 (A4/C#5/E5)";
            meta_.artist = "MiPlay Core Synthesizer";
            meta_.album = "Multi-Room Sync Test";
            meta_.duration_ms = 0;
        } else if (mode == AudioSourceMode::SystemLoopback) {
#ifdef _WIN32
            meta_.title = "Windows WASAPI 全局音频环回";
#else
            meta_.title = "系统全局音频环回";
#endif
            meta_.artist = "Desktop Loopback";
            meta_.album = param.empty() ? "Default Output" : param;
            meta_.duration_ms = 0;
        }
    }
    mode_.store(mode);
}

std::string AudioPipeline::source_param() const {
    std::lock_guard<std::mutex> lock(param_mu_);
    return source_param_;
}

AudioTrackMeta AudioPipeline::current_meta() const {
    std::lock_guard<std::mutex> lock(param_mu_);
    return meta_;
}

bool AudioPipeline::load_wav_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    char hdr[44]{};
    ifs.read(hdr, 44);
    if (ifs.gcount() < 44 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    ifs.seekg(12, std::ios::beg);
    uint32_t data_size = 0;
    while (ifs) {
        char id[4]{};
        uint32_t sz = 0;
        ifs.read(id, 4);
        ifs.read(reinterpret_cast<char*>(&sz), 4);
        if (!ifs) break;
        if (std::memcmp(id, "data", 4) == 0) {
            data_size = sz;
            break;
        }
        ifs.seekg(sz, std::ios::cur);
    }
    if (data_size == 0) return false;
    wav_pcm_.resize(data_size / 2);
    ifs.read(reinterpret_cast<char*>(wav_pcm_.data()), static_cast<std::streamsize>(data_size));
    wav_cursor_ = 0;

    auto slash = path.find_last_of("/\\");
    meta_.title = (slash == std::string::npos) ? path : path.substr(slash + 1);
    meta_.artist = "Local WAV Lossless";
    meta_.album = "Built-in Player";
    meta_.duration_ms = (wav_pcm_.size() / 2) * 1000ULL / std::max<uint32_t>(1, sample_rate_);
    meta_.position_ms = 0;
    return true;
}

bool AudioPipeline::start(uint32_t sample_rate, uint32_t frame_samples,
                          PcmFrameCallback pcm_cb, SpectrumCallback spec_cb) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    frame_samples_ = frame_samples > 0 ? frame_samples : 480;
    pcm_cb_ = std::move(pcm_cb);
    spec_cb_ = std::move(spec_cb);
    running_.store(true);
    worker_ = std::thread(&AudioPipeline::worker_loop, this);
    return true;
}

void AudioPipeline::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void AudioPipeline::push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples) {
    std::lock_guard<std::mutex> lock(ext_mu_);
    ext_ring_.insert(ext_ring_.end(), stereo_pcm, stereo_pcm + frame_samples * 2);
    const size_t max_samples = sample_rate_ * 2;
    if (ext_ring_.size() > max_samples) {
        ext_ring_.erase(ext_ring_.begin(),
                        ext_ring_.begin() + static_cast<ptrdiff_t>(ext_ring_.size() - max_samples));
    }
}

void AudioPipeline::compute_spectrum(const int16_t* stereo_pcm, size_t frames) {
    if (!spec_cb_ || frames == 0) return;
    std::array<float, 16> bands{};
    const size_t step = std::max<size_t>(1, frames / 16);
    for (size_t b = 0; b < 16; ++b) {
        double sum_sq = 0.0;
        size_t cnt = 0;
        for (size_t i = b * step; i < std::min(frames, (b + 1) * step); ++i) {
            double s = (static_cast<double>(stereo_pcm[i * 2]) +
                        static_cast<double>(stereo_pcm[i * 2 + 1])) * 0.5 / 32768.0;
            sum_sq += s * s;
            ++cnt;
        }
        float rms = cnt > 0 ? static_cast<float>(std::sqrt(sum_sq / cnt)) : 0.0f;
        bands[b] = std::min(1.0f, rms * 3.5f);
    }
    spec_cb_(bands);
}

void AudioPipeline::worker_loop() {
    std::vector<int16_t> frame(frame_samples_ * 2, 0);
    const uint64_t frame_us = (static_cast<uint64_t>(frame_samples_) * 1000000ULL) / sample_rate_;
    uint64_t next_tick_us = sync::monotonic_now_us();
    uint64_t sample_idx = 0;
    uint64_t spec_div = 0;

#ifdef _WIN32
    WasapiLoopbackCapturer wasapi;
#else
    FILE* loopback_pipe = nullptr;
    auto close_pipe = [&]() {
        if (loopback_pipe) {
            pclose(loopback_pipe);
            loopback_pipe = nullptr;
        }
    };
#endif

    while (running_.load()) {
        const auto cur_mode = mode_.load();
        std::fill(frame.begin(), frame.end(), 0);

#ifdef _WIN32
        if (cur_mode != AudioSourceMode::SystemLoopback && wasapi.is_active()) {
            wasapi.stop();
        }
#else
        if (cur_mode != AudioSourceMode::SystemLoopback && loopback_pipe) {
            close_pipe();
        }
#endif

        if (cur_mode == AudioSourceMode::TestTone) {
            for (size_t i = 0; i < frame_samples_; ++i) {
                double t = static_cast<double>(sample_idx++) / sample_rate_;
                double env = 0.18 + 0.10 * std::sin(kTwoPi * 1.0 * t);
                double sig = 0.5 * std::sin(kTwoPi * 440.0 * t) +
                             0.3 * std::sin(kTwoPi * 554.37 * t) +
                             0.2 * std::sin(kTwoPi * 659.25 * t);
                int16_t s = static_cast<int16_t>(sig * env * 32767.0);
                frame[i * 2] = s;
                frame[i * 2 + 1] = s;
            }
        } else if (cur_mode == AudioSourceMode::WavFile) {
            std::lock_guard<std::mutex> lock(param_mu_);
            if (!wav_pcm_.empty()) {
                for (size_t i = 0; i < frame_samples_ * 2; ++i) {
                    frame[i] = wav_pcm_[wav_cursor_];
                    wav_cursor_ = (wav_cursor_ + 1) % wav_pcm_.size();
                }
                meta_.position_ms = (wav_cursor_ / 2) * 1000ULL / sample_rate_;
            }
        } else if (cur_mode == AudioSourceMode::ExternalPcm) {
            std::lock_guard<std::mutex> lock(ext_mu_);
            const size_t need = frame_samples_ * 2;
            if (ext_ring_.size() >= need) {
                std::memcpy(frame.data(), ext_ring_.data(), need * sizeof(int16_t));
                ext_ring_.erase(ext_ring_.begin(), ext_ring_.begin() + static_cast<ptrdiff_t>(need));
            }
        } else if (cur_mode == AudioSourceMode::SystemLoopback) {
#ifdef _WIN32
            if (!wasapi.is_active()) {
                wasapi.start(sample_rate_);
            }
            wasapi.read_stereo_frame(frame.data(), frame_samples_);
#elif defined(__linux__)
            if (!loopback_pipe) {
                std::string cmd = "parec --format=s16le --channels=2 --rate=" +
                                  std::to_string(sample_rate_) +
                                  " --latency-msec=20 -d @DEFAULT_MONITOR@ 2>/dev/null";
                loopback_pipe = popen(cmd.c_str(), "r");
            }
            if (loopback_pipe) {
                size_t got = std::fread(frame.data(), sizeof(int16_t), frame.size(), loopback_pipe);
                if (got == 0) close_pipe();
            }
#endif
        }

        if (pcm_cb_) {
            pcm_cb_(frame.data(), frame_samples_);
        }
        if (++spec_div % 3 == 0) {
            compute_spectrum(frame.data(), frame_samples_);
        }

        next_tick_us += frame_us;
        uint64_t now = sync::monotonic_now_us();
        if (next_tick_us > now) {
            std::this_thread::sleep_for(std::chrono::microseconds(next_tick_us - now));
        } else if (now - next_tick_us > 100000ULL) {
            next_tick_us = now;
        }
    }

#ifdef _WIN32
    wasapi.stop();
#else
    close_pipe();
#endif
}

} // namespace miplay::media
