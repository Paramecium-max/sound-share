#include "media/aac_encoder.h"
#include "media/aac_timeline.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <thread>

#ifdef _WIN32
  #include <windows.h>
  #include <objbase.h>
  #include <mfapi.h>
  #include <mfidl.h>
  #include <mferror.h>
  #include <mftransform.h>
#endif

namespace miplay::media {

std::vector<uint8_t> AacEncoder::wrap_adts(const uint8_t* raw_aac, size_t len, uint32_t sample_rate, uint32_t channels) {
    if (!raw_aac || len == 0 || len > 8191 - 7) return {};
    const size_t frame_len = len + 7;
    uint8_t freq_idx = 3; // 48000 Hz
    if (sample_rate == 44100) freq_idx = 4;
    else if (sample_rate == 32000) freq_idx = 5;
    else if (sample_rate == 24000) freq_idx = 6;
    const uint8_t ch = static_cast<uint8_t>(channels & 0x07);

    std::vector<uint8_t> out(frame_len);
    out[0] = 0xFF;
    out[1] = 0xF9; // MPEG-2, no CRC (与红米 K90 真机抓包 0xFF 0xF9 0x4C 完全一致)
    out[2] = static_cast<uint8_t>((1 << 6) | (freq_idx << 2) | (ch >> 2));
    out[3] = static_cast<uint8_t>(((ch & 3) << 6) | ((frame_len >> 11) & 3));
    out[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
    out[5] = static_cast<uint8_t>(((frame_len & 7) << 5) | 0x1F);
    out[6] = 0xFC;
    if (len > 0) std::memcpy(out.data() + 7, raw_aac, len);
    return out;
}

namespace {
size_t adts_size(const uint8_t* data, size_t len) {
    if (len < 7 || data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return 0;
    const size_t size = ((data[3] & 3u) << 11) | (data[4] << 3) | (data[5] >> 5);
    const size_t header_size = (data[1] & 1u) ? 7 : 9;
    return size > header_size && size <= len && (data[6] & 3u) == 0 ? size : 0;
}

bool emit_adts_stream(const uint8_t* data, size_t len, const AacEncoder::AdtsCallback& cb) {
    if (len == 0) return false;
    for (size_t off = 0; off < len;) {
        const size_t size = adts_size(data + off, len - off);
        if (!size) return false;
        off += size;
    }
    for (size_t off = 0; off < len;) {
        const size_t size = adts_size(data + off, len - off);
        std::vector<uint8_t> frame(data + off, data + off + size);
        if ((frame[1] & 1u) != 0) frame[1] = 0xF9;
        cb(frame.data(), frame.size());
        off += size;
    }
    return true;
}
} // namespace

void AacEncoder::emit_adts_frames(const uint8_t* data, size_t len, uint32_t sample_rate,
                                  uint32_t channels, const AdtsCallback& cb) {
    if (!data || !len || !cb) return;
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) {
        emit_adts_stream(data, len, cb);
    } else {
        auto wrapped = wrap_adts(data, len, sample_rate, channels);
        if (!wrapped.empty()) cb(wrapped.data(), wrapped.size());
    }
}

#ifdef _WIN32

struct AacEncoder::Impl {
    using PfnMFStartup = HRESULT(WINAPI*)(ULONG, DWORD);
    using PfnMFShutdown = HRESULT(WINAPI*)();
    using PfnMFCreateMediaType = HRESULT(WINAPI*)(IMFMediaType**);
    using PfnMFCreateSample = HRESULT(WINAPI*)(IMFSample**);
    using PfnMFCreateMemoryBuffer = HRESULT(WINAPI*)(DWORD, IMFMediaBuffer**);

    HMODULE mfplat = nullptr;
    PfnMFStartup fn_startup = nullptr;
    PfnMFShutdown fn_shutdown = nullptr;
    PfnMFCreateMediaType fn_create_mt = nullptr;
    PfnMFCreateSample fn_create_sample = nullptr;
    PfnMFCreateMemoryBuffer fn_create_buf = nullptr;

    bool com_inited = false;
    bool mf_started = false;
    std::atomic<bool> ready{false};
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    bool output_adts = false;
    bool provides_samples = false;
    bool streaming_started = false;
    AacTimeline timeline;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread owner;
    std::packaged_task<bool()> pending;
    bool stopping = false;


    IMFTransform* mft = nullptr;
    DWORD out_buf_size = 8192;

    bool init_transform(uint32_t sr, uint32_t ch, uint32_t bitrate) {
        sample_rate = sr > 0 ? sr : 48000;
        channels = ch > 0 ? ch : 2;
        timeline.clear();

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_inited = SUCCEEDED(hr);
        if (!com_inited) return false;

        mfplat = LoadLibraryA("mfplat.dll");
        if (!mfplat) { return false; }

        fn_startup = reinterpret_cast<PfnMFStartup>(GetProcAddress(mfplat, "MFStartup"));
        fn_shutdown = reinterpret_cast<PfnMFShutdown>(GetProcAddress(mfplat, "MFShutdown"));
        fn_create_mt = reinterpret_cast<PfnMFCreateMediaType>(GetProcAddress(mfplat, "MFCreateMediaType"));
        fn_create_sample = reinterpret_cast<PfnMFCreateSample>(GetProcAddress(mfplat, "MFCreateSample"));
        fn_create_buf = reinterpret_cast<PfnMFCreateMemoryBuffer>(GetProcAddress(mfplat, "MFCreateMemoryBuffer"));
        if (!fn_startup || !fn_shutdown || !fn_create_mt || !fn_create_sample || !fn_create_buf) {
            return false;
        }

        // MF_VERSION = 0x00020070 (Win7+), MFSTARTUP_NOSOCKET = 0x1
        if (FAILED(fn_startup(0x00020070, 0x1))) {
            return false;
        }
        mf_started = true;

        // CLSID_AACMFTEncoder = {93AF0C51-2275-45d2-A35B-F2BA21CAED00}
        static const CLSID kClsidAacEnc = {
            0x93AF0C51, 0x2275, 0x45D2, {0xA3, 0x5B, 0xF2, 0xBA, 0x21, 0xCA, 0xED, 0x00}
        };
        static const IID kIidImfTransform = {
            0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}
        };

        hr = CoCreateInstance(kClsidAacEnc, nullptr, CLSCTX_INPROC_SERVER,
                              kIidImfTransform, reinterpret_cast<void**>(&mft));
        if (FAILED(hr) || !mft) {
            return false;
        }

        // 配置输入媒体类型：PCM 16-bit Stereo 48kHz
        IMFMediaType* in_type = nullptr;
        if (FAILED(fn_create_mt(&in_type)) || !in_type) { return false; }

        static const GUID kMajorAudio = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
        static const GUID kSubPcm     = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
        static const GUID kSubAac     = {0x00001610,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

        static const GUID kAttrMajorType      = {0x48eba18e,0xf8c9,0x4687,{0xbf,0x11,0x0a,0x74,0xc9,0xf9,0x6a,0x8f}};
        static const GUID kAttrSubtype        = {0xf7e34c9a,0x42e8,0x4714,{0xb7,0x4b,0xcb,0x29,0xd7,0x2c,0x35,0xe5}};
        static const GUID kAttrAudioChannels  = {0x37e48bf5,0x645e,0x4c5b,{0x89,0xde,0xad,0xa9,0xe2,0x9b,0x69,0x6a}};
        static const GUID kAttrAudioRate      = {0x5faeeae7,0x0290,0x4c31,{0x9e,0x8a,0xc5,0x34,0xf6,0x8d,0x9d,0xba}};
        static const GUID kAttrAudioBits      = {0xf2deb57f,0x40fa,0x4764,{0xaa,0x33,0xed,0x4f,0x2d,0x1f,0xf6,0x69}};
        static const GUID kAttrAudioAvgBytes  = {0x1aab75c8,0xcfef,0x451c,{0xab,0x95,0xac,0x03,0x4b,0x8e,0x17,0x31}};
        static const GUID kAttrAacPayloadType = {0xbfbabe79,0x7434,0x4d1c,{0x94,0xf0,0x72,0xa3,0xb9,0xe1,0x71,0x88}};
        static const GUID kAttrAacProfileLvl  = {0x293275a4,0x7259,0x4f92,{0x8f,0x79,0x53,0x44,0x7b,0x7a,0x02,0x6c}};

        in_type->SetGUID(kAttrMajorType, kMajorAudio);
        in_type->SetGUID(kAttrSubtype, kSubPcm);
        in_type->SetUINT32(kAttrAudioBits, 16);
        in_type->SetUINT32(kAttrAudioChannels, channels);
        in_type->SetUINT32(kAttrAudioRate, sample_rate);
        hr = mft->SetInputType(0, in_type, 0);
        in_type->Release();
        if (FAILED(hr)) { return false; }

        // 配置输出媒体类型：优先 raw AAC (payloadType=0，自行包 0xFF 0xF9 ADTS 头)；
        // 若系统 MFT 仅接受 payloadType=1（自带 ADTS 头）则自动无缝回退
        uint32_t avg_bytes = (bitrate >= 192000) ? 24000 : 16000;
        HRESULT out_hr = E_FAIL;
        for (uint32_t pt : {0u, 1u}) {
            IMFMediaType* out_type = nullptr;
            if (FAILED(fn_create_mt(&out_type)) || !out_type) continue;
            out_type->SetGUID(kAttrMajorType, kMajorAudio);
            out_type->SetGUID(kAttrSubtype, kSubAac);
            out_type->SetUINT32(kAttrAudioBits, 16);
            out_type->SetUINT32(kAttrAudioChannels, channels);
            out_type->SetUINT32(kAttrAudioRate, sample_rate);
            out_type->SetUINT32(kAttrAudioAvgBytes, avg_bytes);
            out_type->SetUINT32(kAttrAacPayloadType, pt);
            out_type->SetUINT32(kAttrAacProfileLvl, 0x29);
            out_hr = mft->SetOutputType(0, out_type, 0);
            out_type->Release();
            if (SUCCEEDED(out_hr)) {
                output_adts = pt == 1;
                break;
            }
        }
        if (FAILED(out_hr)) { return false; }

        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
        out_buf_size = info.cbSize > 8192 ? info.cbSize : 8192;
        provides_samples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        if (FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
        streaming_started = true;
        return SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    bool init(uint32_t sr, uint32_t ch, uint32_t bitrate) {
        shutdown();
        std::promise<bool> startup;
        auto result = startup.get_future();
        owner = std::thread([this, sr, ch, bitrate, startup = std::move(startup)]() mutable {
            ready.store(init_transform(sr, ch, bitrate));
            startup.set_value(ready.load());
            while (ready.load()) {
                std::packaged_task<bool()> job;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || pending.valid(); });
                    if (!pending.valid()) break;
                    job = std::move(pending);
                }
                job();
            }
            release_transform();
        });
        const bool ok = result.get();
        if (!ok) shutdown();
        return ok;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_one();
        if (owner.joinable()) owner.join();
        stopping = false;
    }

    void release_transform() {
        ready.store(false);
        if (mft) {
            if (streaming_started) {
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                if (SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) {
                    drain([](const uint8_t*, size_t, uint64_t) {});
                }
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
                streaming_started = false;
            }
            mft->Release();
            mft = nullptr;
        }
        if (mf_started && fn_shutdown) {
            fn_shutdown();
            mf_started = false;
        }
        if (mfplat) {
            FreeLibrary(mfplat);
            mfplat = nullptr;
        }
        if (com_inited) {
            CoUninitialize();
            com_inited = false;
        }
    }

    struct ComRelease {
        void operator()(IUnknown* obj) const { if (obj) obj->Release(); }
    };
    template<class T> using ComPtr = std::unique_ptr<T, ComRelease>;

    ComPtr<IMFSample> create_sample() {
        IMFSample* sample = nullptr;
        const auto hr = fn_create_sample(&sample);
        ComPtr<IMFSample> result(sample);
        if (FAILED(hr)) return {};
        return result;
    }

    ComPtr<IMFMediaBuffer> create_buffer(DWORD size) {
        IMFMediaBuffer* buffer = nullptr;
        const auto hr = fn_create_buf(size, &buffer);
        ComPtr<IMFMediaBuffer> result(buffer);
        if (FAILED(hr)) return {};
        return result;
    }

    class LockedBuffer {
    public:
        explicit LockedBuffer(IMFMediaBuffer* buffer) : buffer_(buffer) {}
        ~LockedBuffer() { if (locked_) buffer_->Unlock(); }
        HRESULT lock(BYTE** data, DWORD* length, DWORD* capacity) {
            const auto hr = buffer_->Lock(data, capacity, length);
            locked_ = SUCCEEDED(hr);
            return hr;
        }
        HRESULT unlock() {
            locked_ = false;
            return buffer_->Unlock();
        }
    private:
        IMFMediaBuffer* buffer_;
        bool locked_ = false;
    };

    bool drain(const TimedAdtsCallback& cb) {
        for (int tries = 0; tries < 16; ++tries) {
            ComPtr<IMFSample> sample;
            if (!provides_samples) {
                sample = create_sample();
                auto buffer = create_buffer(out_buf_size);
                if (!sample || !buffer || FAILED(sample->AddBuffer(buffer.get()))) return false;
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = sample.get();
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFCollection> events(output.pEvents);
            if (output.pSample != sample.get()) sample.reset(output.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(hr) || !sample) return false;

            IMFMediaBuffer* raw_buffer = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&raw_buffer)) || !raw_buffer) return false;
            ComPtr<IMFMediaBuffer> buffer(raw_buffer);
            BYTE* data = nullptr;
            DWORD len = 0;
            DWORD capacity = 0;
            LockedBuffer locked(buffer.get());
            if (FAILED(locked.lock(&data, &len, &capacity))) return false;
            std::vector<uint8_t> bytes;
            if (data && len && len <= capacity) bytes.assign(data, data + len);
            if (FAILED(locked.unlock()) || bytes.empty()) return false;

            // AAC-LC emits one AU per accepted 1024-sample block, in input order.
            // Keep the original linear 90 kHz timestamp even when the transform buffers output.
            bool mapped = true;
            auto emit = [&](const uint8_t* au, size_t size) {
                const auto pts = timeline.take_output();
                if (!pts) {
                    mapped = false;
                    return;
                }
                cb(au, size, *pts);
            };
            if (output_adts) {
                if (!emit_adts_stream(bytes.data(), bytes.size(), emit)) return false;
            } else {
                auto au = AacEncoder::wrap_adts(bytes.data(), bytes.size(), sample_rate, channels);
                if (au.empty()) return false;
                emit(au.data(), au.size());
            }
            if (!mapped) return false;
        }
        return false;
    }

    bool submit(const int16_t* pcm, uint64_t pts, const TimedAdtsCallback& cb) {
        auto buffer = create_buffer(1024 * channels * sizeof(int16_t));
        auto sample = create_sample();
        if (!buffer || !sample) return false;

        const DWORD bytes = 1024 * channels * sizeof(int16_t);
        BYTE* ptr = nullptr;
        DWORD capacity = 0;
        LockedBuffer locked(buffer.get());
        if (FAILED(locked.lock(&ptr, nullptr, &capacity))) return false;
        if (ptr && capacity >= bytes) std::memcpy(ptr, pcm, bytes);
        if (FAILED(locked.unlock()) || !ptr || capacity < bytes || FAILED(buffer->SetCurrentLength(bytes)) ||
            FAILED(sample->AddBuffer(buffer.get()))) return false;

        const auto time = static_cast<int64_t>(pts * 1000 / 9);
        const auto end = static_cast<int64_t>((pts + 1024 * 90000ULL / sample_rate) * 1000 / 9);
        if (FAILED(sample->SetSampleTime(time)) || FAILED(sample->SetSampleDuration(end - time))) return false;

        return timeline.submit(pts, [&] {
            const auto hr = mft->ProcessInput(0, sample.get(), 0);
            if (SUCCEEDED(hr)) return AacInputResult::Accepted;
            if (hr == MF_E_NOTACCEPTING) return AacInputResult::Backpressure;
            return AacInputResult::Failed;
        }, [&] { return drain(cb); });
    }

    bool encode(const int16_t* pcm, size_t frames, uint64_t pts, const TimedAdtsCallback& cb) {
        if (!pcm || frames != 1024 || !cb) return false;
        std::packaged_task<bool()> task([&] {
            if (submit(pcm, pts, cb)) return true;
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            timeline.clear();
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            return false;
        });
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex);
            if (!ready.load() || stopping || pending.valid()) return false;
            pending = std::move(task);
        }
        cv.notify_one();
        return result.get();
    }
};

#else

struct AacEncoder::Impl {
    bool ready = false;
    bool init(uint32_t, uint32_t, uint32_t) { ready = false; return false; }
    void shutdown() { ready = false; }
    bool encode(const int16_t*, size_t, uint64_t, const TimedAdtsCallback&) { return false; }
};

#endif

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() { shutdown(); }

bool AacEncoder::init(uint32_t sample_rate, uint32_t channels, uint32_t bitrate) {
    return impl_->init(sample_rate, channels, bitrate);
}

void AacEncoder::shutdown() {
    if (impl_) impl_->shutdown();
}

bool AacEncoder::is_available() const {
    return impl_ && impl_->ready;
}

bool AacEncoder::encode_pcm(const int16_t* pcm, size_t frames, uint64_t pts,
                            const TimedAdtsCallback& cb) {
    return impl_->encode(pcm, frames, pts, cb);
}

} // namespace miplay::media
