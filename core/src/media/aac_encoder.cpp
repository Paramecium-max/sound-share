#include "media/aac_encoder.h"

#include <cstring>

#ifdef _WIN32
  #include <windows.h>
  #include <objbase.h>
  #include <mfapi.h>
  #include <mfidl.h>
  #include <mftransform.h>
#endif

namespace miplay::media {

std::vector<uint8_t> AacEncoder::wrap_adts(const uint8_t* raw_aac, size_t len, uint32_t sample_rate, uint32_t channels) {
    // 若输入已经带 ADTS 同步字 0xFFFx，直接返回避免重复封装
    if (len >= 7 && raw_aac[0] == 0xFF && (raw_aac[1] & 0xF0) == 0xF0) {
        return std::vector<uint8_t>(raw_aac, raw_aac + len);
    }
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
    bool ready = false;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    int64_t sample_time_100ns = 0;

    IMFTransform* mft = nullptr;
    DWORD out_buf_size = 8192;

    bool init(uint32_t sr, uint32_t ch, uint32_t bitrate) {
        shutdown();
        sample_rate = sr > 0 ? sr : 48000;
        channels = ch > 0 ? ch : 2;
        sample_time_100ns = 0;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_inited = SUCCEEDED(hr);

        mfplat = LoadLibraryA("mfplat.dll");
        if (!mfplat) { shutdown(); return false; }

        fn_startup = reinterpret_cast<PfnMFStartup>(GetProcAddress(mfplat, "MFStartup"));
        fn_shutdown = reinterpret_cast<PfnMFShutdown>(GetProcAddress(mfplat, "MFShutdown"));
        fn_create_mt = reinterpret_cast<PfnMFCreateMediaType>(GetProcAddress(mfplat, "MFCreateMediaType"));
        fn_create_sample = reinterpret_cast<PfnMFCreateSample>(GetProcAddress(mfplat, "MFCreateSample"));
        fn_create_buf = reinterpret_cast<PfnMFCreateMemoryBuffer>(GetProcAddress(mfplat, "MFCreateMemoryBuffer"));
        if (!fn_startup || !fn_shutdown || !fn_create_mt || !fn_create_sample || !fn_create_buf) {
            shutdown();
            return false;
        }

        // MF_VERSION = 0x00020070 (Win7+), MFSTARTUP_NOSOCKET = 0x1
        if (FAILED(fn_startup(0x00020070, 0x1))) {
            shutdown();
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
            shutdown();
            return false;
        }

        // 配置输入媒体类型：PCM 16-bit Stereo 48kHz
        IMFMediaType* in_type = nullptr;
        if (FAILED(fn_create_mt(&in_type)) || !in_type) { shutdown(); return false; }

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
        if (FAILED(hr)) { shutdown(); return false; }

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
            if (SUCCEEDED(out_hr)) break;
        }
        if (FAILED(out_hr)) { shutdown(); return false; }

        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info)) && info.cbSize > 0) {
            out_buf_size = info.cbSize > 8192 ? info.cbSize : 8192;
        }

        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        ready = true;
        return true;
    }

    void shutdown() {
        ready = false;
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
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

    void encode(const int16_t* pcm, size_t frames, const AdtsCallback& cb) {
        if (!ready || !mft || !cb || frames == 0) return;

        const DWORD byte_len = static_cast<DWORD>(frames * channels * sizeof(int16_t));
        IMFMediaBuffer* in_buf = nullptr;
        IMFSample* in_sample = nullptr;
        if (FAILED(fn_create_buf(byte_len, &in_buf)) || !in_buf) return;

        BYTE* ptr = nullptr;
        if (SUCCEEDED(in_buf->Lock(&ptr, nullptr, nullptr)) && ptr) {
            std::memcpy(ptr, pcm, byte_len);
            in_buf->Unlock();
            in_buf->SetCurrentLength(byte_len);
        }
        if (SUCCEEDED(fn_create_sample(&in_sample)) && in_sample) {
            in_sample->AddBuffer(in_buf);
            const int64_t dur = static_cast<int64_t>((frames * 10000000ULL) / sample_rate);
            in_sample->SetSampleTime(sample_time_100ns);
            in_sample->SetSampleDuration(dur);
            sample_time_100ns += dur;
            mft->ProcessInput(0, in_sample, 0);
            in_sample->Release();
        }
        in_buf->Release();

        // Drain 所有编码完成的 AAC 帧
        for (int tries = 0; tries < 8; ++tries) {
            IMFSample* out_sample = nullptr;
            IMFMediaBuffer* out_buf = nullptr;
            if (FAILED(fn_create_sample(&out_sample)) || !out_sample) break;
            if (FAILED(fn_create_buf(out_buf_size, &out_buf)) || !out_buf) {
                out_sample->Release();
                break;
            }
            out_sample->AddBuffer(out_buf);

            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;
            odb.pSample = out_sample;
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
            if (hr == S_OK) {
                IMFMediaBuffer* contig = nullptr;
                if (SUCCEEDED(out_sample->ConvertToContiguousBuffer(&contig)) && contig) {
                    BYTE* d = nullptr;
                    DWORD cur_len = 0;
                    if (SUCCEEDED(contig->Lock(&d, nullptr, &cur_len)) && d && cur_len > 0) {
                        auto adts = AacEncoder::wrap_adts(d, cur_len, sample_rate, channels);
                        cb(adts.data(), adts.size());
                        contig->Unlock();
                    }
                    contig->Release();
                }
            }
            if (odb.pEvents) odb.pEvents->Release();
            out_buf->Release();
            out_sample->Release();
            if (hr != S_OK) break;
        }
    }
};

#else

struct AacEncoder::Impl {
    bool ready = false;
    bool init(uint32_t, uint32_t, uint32_t) { ready = false; return false; }
    void shutdown() { ready = false; }
    void encode(const int16_t*, size_t, const AdtsCallback&) {}
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

void AacEncoder::encode_pcm(const int16_t* stereo_pcm, size_t sample_frames, const AdtsCallback& cb) {
    if (impl_) impl_->encode(stereo_pcm, sample_frames, cb);
}

} // namespace miplay::media
