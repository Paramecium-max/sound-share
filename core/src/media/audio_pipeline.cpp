#include "media/audio_pipeline.h"
#include "sync/master_clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <propsys.h>
#elif defined(__linux__)
  #include <cerrno>
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace miplay::media {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint64_t kMaxWavBytes = 128ULL * 1024ULL * 1024ULL; // 128 MiB 上限，避免畸形头 OOM

uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool is_safe_linux_source_name(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    for (unsigned char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '@' || c == ':';
        if (!ok) return false;
    }
    return true;
}

#ifdef _WIN32
constexpr CLSID kDeviceEnumeratorClass = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
constexpr IID kDeviceEnumeratorInterface = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
constexpr PROPERTYKEY kDeviceFriendlyName = {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}},14};
constexpr std::array<ERole, 2> kPlaybackRoles{eConsole, eMultimedia};

// Windows exposes default-device notifications through Core Audio. Changing
// the default programmatically uses this Windows audio policy COM interface.
struct IPlaybackPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, const PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
};

bool create_device_enumerator(IMMDeviceEnumerator** enumerator) {
    return SUCCEEDED(CoCreateInstance(kDeviceEnumeratorClass, nullptr, CLSCTX_ALL,
                                      kDeviceEnumeratorInterface, reinterpret_cast<void**>(enumerator)));
}

std::wstring endpoint_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id))) result = id;
    CoTaskMemFree(id);
    return result;
}

std::wstring default_playback_endpoint(IMMDeviceEnumerator* enumerator, ERole role) {
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) return {};
    auto id = endpoint_id(device);
    device->Release();
    return id;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

struct WinRenderEndpointInfo {
    std::wstring id;
    std::wstring name;
};

std::vector<WinRenderEndpointInfo> enum_active_render_endpoints(IMMDeviceEnumerator* enumerator) {
    std::vector<WinRenderEndpointInfo> list;
    if (!enumerator) return list;
    IMMDeviceCollection* devices = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) || !devices) {
        return list;
    }
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(i, &device)) || !device) continue;
        WinRenderEndpointInfo info;
        info.id = endpoint_id(device);
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT prop{};
            if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &prop)) &&
                prop.vt == VT_LPWSTR && prop.pwszVal) {
                info.name = prop.pwszVal;
            }
            PropVariantClear(&prop);
            store->Release();
        }
        device->Release();
        if (!info.id.empty()) {
            if (info.name.empty()) info.name = L"Windows Audio Output";
            list.push_back(std::move(info));
        }
    }
    devices->Release();
    return list;
}

bool contains_icase_w(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    auto to_low = [](wchar_t c) -> wchar_t {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (to_low(haystack[i + j]) != to_low(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// 智能挑选静默接管目标（优先虚拟扬声器 -> 其次未作为主扬声器的 S/PDIF 数字输出 -> 再次其他非默认输出端点）
std::wstring find_best_takeover_render_endpoint(IMMDeviceEnumerator* enumerator, const std::wstring& cur_default_id) {
    auto eps = enum_active_render_endpoints(enumerator);
    std::wstring best_virtual;
    std::wstring fallback_virtual;
    std::wstring best_digital;
    std::wstring any_secondary;

    for (const auto& ep : eps) {
        const bool is_virtual = contains_icase_w(ep.name, L"VB-Audio") ||
                                contains_icase_w(ep.name, L"Virtual") ||
                                contains_icase_w(ep.name, L"CABLE") ||
                                contains_icase_w(ep.name, L"VoiceMeeter") ||
                                contains_icase_w(ep.name, L"Steam Streaming");
        if (is_virtual) {
            if (!contains_icase_w(ep.name, L"16 Ch")) {
                if (best_virtual.empty()) best_virtual = ep.id;
            } else if (fallback_virtual.empty()) {
                fallback_virtual = ep.id;
            }
        }
        if (ep.id != cur_default_id) {
            const bool is_digital = contains_icase_w(ep.name, L"Digital Output") ||
                                    contains_icase_w(ep.name, L"S/PDIF") ||
                                    contains_icase_w(ep.name, L"SPDIF") ||
                                    contains_icase_w(ep.name, L"Optical") ||
                                    contains_icase_w(ep.name, L"数字输出");
            if (is_digital && best_digital.empty()) best_digital = ep.id;
            if (any_secondary.empty()) any_secondary = ep.id;
        }
    }
    if (!best_virtual.empty()) return best_virtual;
    if (!fallback_virtual.empty()) return fallback_virtual;
    if (!best_digital.empty()) return best_digital;
    return any_secondary;
}

std::filesystem::path playback_recovery_file() {
    const DWORD length = GetEnvironmentVariableW(L"SOUNDSHARE_PLAYBACK_RECOVERY_FILE", nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"SOUNDSHARE_PLAYBACK_RECOVERY_FILE", value.data(), length);
    if (count == 0 || count >= length) return {};
    value.resize(count);
    return value;
}

class WindowsPlaybackRoute {
public:
    ~WindowsPlaybackRoute() { stop(); }

    void recover() {
        if (recovery_file_.empty() || GetFileAttributesW(recovery_file_.c_str()) == INVALID_FILE_ATTRIBUTES ||
            !open()) return;
        read_saved_route();
        stop();
    }

    // 将系统默认输出接管到指定或自动挑选的静默 eRender 端点，供原生 eRender WASAPI Loopback 直接抓取
    bool start(const std::wstring& requested_target_id = {}) {
        if (!open()) return false;
        if (read_saved_route() && !restore_endpoints()) {
            stop();
            return false;
        }
        for (size_t i = 0; i < kPlaybackRoles.size(); ++i) {
            previous_[i] = default_playback_endpoint(enumerator_, kPlaybackRoles[i]);
        }
        const std::wstring cur_def = !previous_[1].empty() ? previous_[1] : previous_[0];
        virtual_endpoint_ = requested_target_id.empty()
                                ? find_best_takeover_render_endpoint(enumerator_, cur_def)
                                : requested_target_id;
        if (virtual_endpoint_.empty() || !valid_saved_route() || !record_route()) {
            virtual_endpoint_.clear();
            stop();
            return false;
        }

        for (size_t i = 0; i < kPlaybackRoles.size(); ++i) {
            if (previous_[i] != virtual_endpoint_ &&
                FAILED(policy_->SetDefaultEndpoint(virtual_endpoint_.c_str(), kPlaybackRoles[i]))) {
                stop();
                return false;
            }
        }
        capture_endpoint_ = virtual_endpoint_;
        return true;
    }

    bool is_active() const { return !virtual_endpoint_.empty(); }
    const std::wstring& capture_endpoint() const { return capture_endpoint_; }

    void stop() {
        if (policy_ && !virtual_endpoint_.empty()) {
            if (restore_endpoints() && !recovery_file_.empty()) DeleteFileW(recovery_file_.c_str());
        }
        virtual_endpoint_.clear();
        capture_endpoint_.clear();
        if (policy_) { policy_->Release(); policy_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (com_init_) { CoUninitialize(); com_init_ = false; }
        if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; }
    }

private:
    bool read_saved_route() {
        if (recovery_file_.empty()) return false;
        std::wifstream file(recovery_file_);
        if (!file) return false;
        std::getline(file, virtual_endpoint_);
        for (auto& saved : previous_) std::getline(file, saved);
        if (valid_saved_route()) return true;
        virtual_endpoint_.clear();
        return false;
    }

    bool restore_endpoints() {
        bool restored = true;
        for (size_t i = 0; i < kPlaybackRoles.size(); ++i) {
            const auto role = kPlaybackRoles[i];
            if (previous_[i] != virtual_endpoint_ &&
                default_playback_endpoint(enumerator_, role) == virtual_endpoint_) {
                restored &= SUCCEEDED(policy_->SetDefaultEndpoint(previous_[i].c_str(), role));
            }
        }
        return restored;
    }

    bool open() {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\SoundSharePlaybackRoute");
        if (!mutex_) return false;
        const auto wait = WaitForSingleObject(mutex_, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return false;
        }
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_init_ = SUCCEEDED(hr);
        static const CLSID clsid = {0x870AF99C,0x171D,0x4F9E,{0xAF,0x0D,0xE6,0x3D,0xF4,0x0C,0x2B,0xC9}};
        static const IID iid = {0xF8679F50,0x850A,0x41CF,{0x9C,0x72,0x43,0x0F,0x29,0x02,0x90,0xC8}};
        if ((FAILED(hr) && hr != RPC_E_CHANGED_MODE) || !create_device_enumerator(&enumerator_) ||
            FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, iid, reinterpret_cast<void**>(&policy_)))) {
            stop();
            return false;
        }
        return true;
    }

    bool valid_saved_route() const {
        const auto is_render_id = [](const std::wstring& id) {
            return id.starts_with(L"{0.0.0.") && id.size() < 128;
        };
        return is_render_id(virtual_endpoint_) && std::all_of(previous_.begin(), previous_.end(), is_render_id);
    }

    bool record_route() const {
        if (recovery_file_.empty()) return true;
        auto temporary = recovery_file_;
        temporary += L".tmp";
        std::wofstream file(temporary, std::ios::trunc);
        file << virtual_endpoint_ << L'\n';
        for (const auto& saved : previous_) file << saved << L'\n';
        file.close();
        if (file && MoveFileExW(temporary.c_str(), recovery_file_.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(temporary.c_str());
        return false;
    }

    const std::filesystem::path recovery_file_ = playback_recovery_file();
    std::wstring virtual_endpoint_;
    std::wstring capture_endpoint_;
    std::array<std::wstring, kPlaybackRoles.size()> previous_;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IPlaybackPolicyConfig* policy_ = nullptr;
    HANDLE mutex_ = nullptr;
    bool com_init_ = false;
};

// Windows 原生 WASAPI 独立高频采集引擎：
// - 直接使用设备 GetMixFormat() 原生格式 + 200ms 捕获环形缓冲（彻底杜绝 20ms < 21.33ms AAC 帧长导致的每拍溢出抽干！）
// - 独立高优先级采集线程每 4ms 实时排空 WASAPI 缓冲区并喂入 eRender 静音保活时钟，保证零丢包、零停钟
class WasapiCapture {
public:
    ~WasapiCapture() { stop(); }

    bool start(uint32_t target_rate, size_t frame_samples, const std::wstring& requested_endpoint = {}) {
        stop();
        target_rate_ = target_rate > 0 ? target_rate : 48000;
        frame_samples_ = frame_samples > 0 ? frame_samples : 480;
        requested_endpoint_ = requested_endpoint;
        {
            std::lock_guard<std::mutex> lk(fifo_mu_);
            fifo_.reset(target_rate_, frame_samples_);
        }
        stop_req_.store(false);
        default_changed_.store(false);
        active_.store(false);

        std::mutex init_mu;
        std::condition_variable init_cv;
        bool init_done = false;
        bool init_ok = false;

        capture_th_ = std::thread([this, &init_mu, &init_cv, &init_done, &init_ok]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            const HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool com_init = SUCCEEDED(hr_com);

            static const IID iid_ac  = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
            static const IID iid_acc = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
            static const IID iid_arc = {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
            static const IID iid_ep  = {0x1BE09788,0x6894,0x4089,{0x85,0x86,0x9A,0x2A,0x6C,0x26,0x5A,0xC5}};
            static const GUID kSubIeeeFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
            static const GUID kSubPcm       = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* audio_client = nullptr;
            IAudioCaptureClient* capture_client = nullptr;
            IAudioClient* keepalive_client = nullptr;
            IAudioRenderClient* keepalive_render = nullptr;
            UINT32 keepalive_buf_frames = 0;
            WAVEFORMATEX* mix_fmt = nullptr;
            bool following_default = requested_endpoint_.empty();
            std::wstring active_ep_id;

            auto cleanup = [&]() {
                if (audio_client) { audio_client->Stop(); audio_client->Release(); }
                if (capture_client) capture_client->Release();
                if (keepalive_client) { keepalive_client->Stop(); keepalive_client->Release(); }
                if (keepalive_render) keepalive_render->Release();
                if (mix_fmt) CoTaskMemFree(mix_fmt);
                if (device) device->Release();
                if (enumerator) enumerator->Release();
                if (com_init) CoUninitialize();
            };

            bool ok = false;
            bool is_float32 = false;
            bool is_pcm16 = false;
            bool is_pcm24 = false;
            bool is_pcm32 = false;
            uint32_t src_rate = target_rate_;
            uint32_t src_ch = 2;
            uint32_t src_block_align = 4;

            do {
                if (!create_device_enumerator(&enumerator) || !enumerator) break;
                if (!requested_endpoint_.empty()) {
                    enumerator->GetDevice(requested_endpoint_.c_str(), &device);
                }
                if (!device) {
                    following_default = true;
                    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) &&
                        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
                        break;
                    }
                }
                active_ep_id = endpoint_id(device);

                EDataFlow flow = eRender;
                IMMEndpoint* ep = nullptr;
                if (SUCCEEDED(device->QueryInterface(iid_ep, reinterpret_cast<void**>(&ep))) && ep) {
                    ep->GetDataFlow(&flow);
                    ep->Release();
                }

                if (FAILED(device->Activate(iid_ac, CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void**>(&audio_client))) || !audio_client) {
                    break;
                }

                // 优先读取声卡混音器原生格式，200ms (2,000,000 * 100ns) 充足环形缓冲彻底杜绝溢出
                const DWORD loopback_flag = (flow == eRender) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
                bool use_native_mix = false;
                if (SUCCEEDED(audio_client->GetMixFormat(&mix_fmt)) && mix_fmt && mix_fmt->nChannels >= 1 &&
                    mix_fmt->nSamplesPerSec >= 8000) {
                    bool fmt_supported = false;
                    if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix_fmt->wBitsPerSample == 32) {
                        is_float32 = true;
                        fmt_supported = true;
                    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_PCM && mix_fmt->wBitsPerSample == 16) {
                        is_pcm16 = true;
                        fmt_supported = true;
                    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_PCM && mix_fmt->wBitsPerSample == 24) {
                        is_pcm24 = true;
                        fmt_supported = true;
                    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_PCM && mix_fmt->wBitsPerSample == 32) {
                        is_pcm32 = true;
                        fmt_supported = true;
                    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
                        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
                        if (std::memcmp(&ext->SubFormat, &kSubIeeeFloat, sizeof(GUID)) == 0 &&
                            mix_fmt->wBitsPerSample == 32) {
                            is_float32 = true;
                            fmt_supported = true;
                        } else if (std::memcmp(&ext->SubFormat, &kSubPcm, sizeof(GUID)) == 0 &&
                                   mix_fmt->wBitsPerSample == 16) {
                            is_pcm16 = true;
                            fmt_supported = true;
                        } else if (std::memcmp(&ext->SubFormat, &kSubPcm, sizeof(GUID)) == 0 &&
                                   mix_fmt->wBitsPerSample == 24) {
                            is_pcm24 = true;
                            fmt_supported = true;
                        } else if (std::memcmp(&ext->SubFormat, &kSubPcm, sizeof(GUID)) == 0 &&
                                   mix_fmt->wBitsPerSample == 32) {
                            is_pcm32 = true;
                            fmt_supported = true;
                        }
                    }
                    if (fmt_supported &&
                        SUCCEEDED(audio_client->Initialize(
                            AUDCLNT_SHAREMODE_SHARED, loopback_flag, 2000000, 0, mix_fmt, nullptr))) {
                        use_native_mix = true;
                        src_rate = mix_fmt->nSamplesPerSec;
                        src_ch = mix_fmt->nChannels;
                        src_block_align = mix_fmt->nBlockAlign;
                    }
                }

                if (!use_native_mix) {
                    is_float32 = false;
                    is_pcm24 = false;
                    is_pcm32 = false;
                    is_pcm16 = true;
                    src_rate = target_rate_;
                    src_ch = 2;
                    src_block_align = 4;
                    WAVEFORMATEX fallback{};
                    fallback.wFormatTag = WAVE_FORMAT_PCM;
                    fallback.nChannels = 2;
                    fallback.nSamplesPerSec = target_rate_;
                    fallback.wBitsPerSample = 16;
                    fallback.nBlockAlign = 4;
                    fallback.nAvgBytesPerSec = target_rate_ * 4;
                    const DWORD flags = loopback_flag |
                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                    if (FAILED(audio_client->Initialize(
                            AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &fallback, nullptr))) {
                        break;
                    }
                }

                if (FAILED(audio_client->GetService(iid_acc, reinterpret_cast<void**>(&capture_client))) ||
                    !capture_client) {
                    break;
                }

                // 在 eRender 端点启动 100ms 静音保活渲染流，使混音器时钟恒定以 10ms 周期运转
                if (flow == eRender && mix_fmt &&
                    SUCCEEDED(device->Activate(iid_ac, CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&keepalive_client))) &&
                    keepalive_client) {
                    if (SUCCEEDED(keepalive_client->Initialize(
                            AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix_fmt, nullptr)) &&
                        SUCCEEDED(keepalive_client->GetBufferSize(&keepalive_buf_frames)) &&
                        SUCCEEDED(keepalive_client->GetService(
                            iid_arc, reinterpret_cast<void**>(&keepalive_render))) &&
                        keepalive_render) {
                        BYTE* p = nullptr;
                        if (keepalive_buf_frames > 0 &&
                            SUCCEEDED(keepalive_render->GetBuffer(keepalive_buf_frames, &p))) {
                            keepalive_render->ReleaseBuffer(keepalive_buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                        keepalive_client->Start();
                    }
                }

                if (FAILED(audio_client->Start())) break;
                ok = true;
            } while (false);

            {
                std::lock_guard<std::mutex> lk(init_mu);
                init_ok = ok;
                init_done = true;
                active_.store(ok);
            }
            init_cv.notify_one();

            if (!ok) {
                cleanup();
                return;
            }

            std::unique_ptr<void, decltype(&CloseHandle)> poll_timer(
                CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE),
                &CloseHandle);

            std::vector<int16_t> converted;
            std::vector<int16_t> resampled;
            StreamLinearResampler resampler;
            resampler.reset(src_rate, target_rate_);
            auto next_def_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

            while (!stop_req_.load()) {
                if (keepalive_client && keepalive_render && keepalive_buf_frames > 0) {
                    UINT32 pad = 0;
                    if (SUCCEEDED(keepalive_client->GetCurrentPadding(&pad)) && pad < keepalive_buf_frames / 2) {
                        const UINT32 fill = keepalive_buf_frames - pad;
                        BYTE* p = nullptr;
                        if (fill > 0 && SUCCEEDED(keepalive_render->GetBuffer(fill, &p))) {
                            keepalive_render->ReleaseBuffer(fill, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                    }
                }

                UINT32 pkt_len = 0;
                HRESULT next = S_OK;
                while (!stop_req_.load() &&
                       SUCCEEDED(next = capture_client->GetNextPacketSize(&pkt_len)) && pkt_len > 0) {
                    BYTE* data = nullptr;
                    UINT32 num_frames = 0;
                    DWORD flags = 0;
                    if (FAILED(next = capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr))) {
                        break;
                    }
                    if (num_frames > 0) {
                        converted.resize(static_cast<size_t>(num_frames) * 2);
                        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                            std::fill(converted.begin(), converted.end(), 0);
                        } else if (is_float32) {
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                const float* f = reinterpret_cast<const float*>(data + i * src_block_align);
                                float fl = std::clamp(std::isfinite(f[0]) ? f[0] : 0.0f, -1.0f, 1.0f);
                                float fr = (src_ch >= 2) ? std::clamp(std::isfinite(f[1]) ? f[1] : 0.0f, -1.0f, 1.0f) : fl;
                                converted[i * 2]     = static_cast<int16_t>(std::lround(fl * 32767.0f));
                                converted[i * 2 + 1] = static_cast<int16_t>(std::lround(fr * 32767.0f));
                            }
                        } else if (is_pcm16) {
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                const int16_t* p = reinterpret_cast<const int16_t*>(data + i * src_block_align);
                                converted[i * 2]     = p[0];
                                converted[i * 2 + 1] = (src_ch >= 2) ? p[1] : p[0];
                            }
                        } else if (is_pcm24) {
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                const uint8_t* p = data + i * src_block_align;
                                auto read24 = [](const uint8_t* b) -> int16_t {
                                    int32_t v = static_cast<int32_t>(b[0]) |
                                                (static_cast<int32_t>(b[1]) << 8) |
                                                (static_cast<int32_t>(b[2]) << 16);
                                    if (v & 0x800000) v |= ~0xFFFFFF;
                                    return static_cast<int16_t>(v >> 8);
                                };
                                int16_t l = read24(p);
                                int16_t r = (src_ch >= 2) ? read24(p + 3) : l;
                                converted[i * 2]     = l;
                                converted[i * 2 + 1] = r;
                            }
                        } else if (is_pcm32) {
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                const int32_t* p = reinterpret_cast<const int32_t*>(data + i * src_block_align);
                                converted[i * 2]     = static_cast<int16_t>(p[0] >> 16);
                                converted[i * 2 + 1] = static_cast<int16_t>(((src_ch >= 2) ? p[1] : p[0]) >> 16);
                            }
                        }

                        if (src_rate == target_rate_) {
                            std::lock_guard<std::mutex> lk(fifo_mu_);
                            fifo_.push_interleaved_stereo(converted.data(), num_frames);
                        } else {
                            resampler.process_interleaved_stereo(converted.data(), num_frames, resampled);
                            if (!resampled.empty()) {
                                std::lock_guard<std::mutex> lk(fifo_mu_);
                                fifo_.push_interleaved_stereo(resampled.data(), resampled.size() / 2);
                            }
                        }
                    }
                    capture_client->ReleaseBuffer(num_frames);
                }

                if (FAILED(next)) {
                    active_.store(false);
                    break;
                }

                if (following_default) {
                    const auto now_tp = std::chrono::steady_clock::now();
                    if (now_tp >= next_def_check) {
                        next_def_check = now_tp + std::chrono::milliseconds(500);
                        std::wstring cur = default_playback_endpoint(enumerator, eMultimedia);
                        if (cur.empty()) cur = default_playback_endpoint(enumerator, eConsole);
                        if (!cur.empty() && cur != active_ep_id) {
                            default_changed_.store(true);
                            active_.store(false);
                            break;
                        }
                    }
                }

                // 每 4ms 排空一次 WASAPI 捕获缓冲区，确保 10ms 包到达后 <4ms 即进平滑 FIFO，0 溢出
                if (poll_timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -40000LL; // 4 ms
                    if (!SetWaitableTimer(poll_timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
                        WaitForSingleObject(poll_timer.get(), INFINITE) != WAIT_OBJECT_0) {
                        poll_timer.reset();
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }

            active_.store(false);
            cleanup();
        });

        std::unique_lock<std::mutex> lk(init_mu);
        init_cv.wait(lk, [&]() { return init_done; });
        if (!init_ok) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        stop_req_.store(true);
        if (capture_th_.joinable()) capture_th_.join();
        active_.store(false);
        default_changed_.store(false);
        std::lock_guard<std::mutex> lk(fifo_mu_);
        fifo_.reset(target_rate_, frame_samples_);
    }

    bool is_active() const { return active_.load(); }
    bool default_endpoint_changed() const { return default_changed_.load(); }

    void read_stereo_frame(int16_t* dst, size_t out_frames) {
        if (!active_.load()) {
            std::memset(dst, 0, out_frames * 2 * sizeof(int16_t));
            return;
        }
        std::lock_guard<std::mutex> lk(fifo_mu_);
        fifo_.pop_smooth_frame(dst, out_frames);
    }

private:
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> default_changed_{false};
    uint32_t target_rate_ = 48000;
    size_t frame_samples_ = 480;
    std::wstring requested_endpoint_;
    std::thread capture_th_;
    std::mutex fifo_mu_;
    SmoothLoopbackFifo fifo_;
};

// Windows MMCSS "Pro Audio" 实时音频线程优先级守卫（动态加载 avrt.dll，零硬依赖）
class WinProAudioThreadGuard {
public:
    WinProAudioThreadGuard() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        avrt_ = LoadLibraryA("avrt.dll");
        if (!avrt_) return;
        using PfnSet = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
        fn_revert_ = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
            GetProcAddress(avrt_, "AvRevertMmThreadCharacteristics"));
        auto fn_set = reinterpret_cast<PfnSet>(
            GetProcAddress(avrt_, "AvSetMmThreadCharacteristicsW"));
        if (fn_set) {
            DWORD task_idx = 0;
            mmcss_handle_ = fn_set(L"Pro Audio", &task_idx);
        }
    }
    ~WinProAudioThreadGuard() {
        if (mmcss_handle_ && fn_revert_) fn_revert_(mmcss_handle_);
        if (avrt_) FreeLibrary(avrt_);
    }
private:
    HMODULE avrt_ = nullptr;
    HANDLE mmcss_handle_ = nullptr;
    BOOL(WINAPI* fn_revert_)(HANDLE) = nullptr;
};
#endif

} // namespace

void StreamLinearResampler::reset(uint32_t src_rate, uint32_t dst_rate) {
    src_rate_ = src_rate > 0 ? src_rate : 48000;
    dst_rate_ = dst_rate > 0 ? dst_rate : 48000;
    step_ = static_cast<double>(src_rate_) / static_cast<double>(dst_rate_);
    phase_ = 0.0;
    has_prev_ = false;
    prev_l_ = 0;
    prev_r_ = 0;
}

void StreamLinearResampler::process_interleaved_stereo(const int16_t* in_stereo, size_t in_frames,
                                                       std::vector<int16_t>& out_stereo) {
    out_stereo.clear();
    if (!in_stereo || in_frames == 0) return;
    if (src_rate_ == dst_rate_) {
        out_stereo.assign(in_stereo, in_stereo + in_frames * 2);
        return;
    }

    // 精确跨包连续插值：
    // - 首包有效索引为 [0 .. in_frames-1]，仅计算 phase_ < in_frames-1，留下末样本为 prev_
    // - 后续包虚拟序列为 [prev_, in[0], ..., in[in_frames-1]]（共 in_frames+1 点），
    //   计算 0 <= phase_ < in_frames，使每个跨包边界点都严格在 prev_ 与 in[0] 之间线性插值，零平顶毛刺！
    const size_t approx_out = static_cast<size_t>(static_cast<double>(in_frames) / step_) + 4;
    out_stereo.reserve(approx_out * 2);

    if (!has_prev_) {
        const double limit = static_cast<double>(in_frames - 1);
        while (phase_ < limit) {
            const size_t i0 = static_cast<size_t>(phase_);
            const size_t i1 = i0 + 1;
            const double frac = phase_ - static_cast<double>(i0);
            const double ol = in_stereo[i0 * 2] * (1.0 - frac) + in_stereo[i1 * 2] * frac;
            const double or_ = in_stereo[i0 * 2 + 1] * (1.0 - frac) + in_stereo[i1 * 2 + 1] * frac;
            out_stereo.push_back(static_cast<int16_t>(std::clamp(std::lround(ol), -32768L, 32767L)));
            out_stereo.push_back(static_cast<int16_t>(std::clamp(std::lround(or_), -32768L, 32767L)));
            phase_ += step_;
        }
        phase_ -= limit;
        prev_l_ = in_stereo[(in_frames - 1) * 2];
        prev_r_ = in_stereo[(in_frames - 1) * 2 + 1];
        has_prev_ = true;
        return;
    }

    const double limit = static_cast<double>(in_frames);
    while (phase_ < limit) {
        const size_t idx0 = static_cast<size_t>(phase_);
        const double frac = phase_ - static_cast<double>(idx0);
        int16_t l0, r0, l1, r1;
        if (idx0 == 0) {
            l0 = prev_l_;
            r0 = prev_r_;
            l1 = in_stereo[0];
            r1 = in_stereo[1];
        } else {
            l0 = in_stereo[(idx0 - 1) * 2];
            r0 = in_stereo[(idx0 - 1) * 2 + 1];
            l1 = in_stereo[idx0 * 2];
            r1 = in_stereo[idx0 * 2 + 1];
        }
        const double ol = l0 * (1.0 - frac) + l1 * frac;
        const double or_ = r0 * (1.0 - frac) + r1 * frac;
        out_stereo.push_back(static_cast<int16_t>(std::clamp(std::lround(ol), -32768L, 32767L)));
        out_stereo.push_back(static_cast<int16_t>(std::clamp(std::lround(or_), -32768L, 32767L)));
        phase_ += step_;
    }
    phase_ -= limit;
    prev_l_ = in_stereo[(in_frames - 1) * 2];
    prev_r_ = in_stereo[(in_frames - 1) * 2 + 1];
}

void SmoothLoopbackFifo::reset(uint32_t sample_rate, size_t frame_samples) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    frame_samples_ = frame_samples > 0 ? frame_samples : 480;
    // 建立充足的防抖水位（~45ms 目标中心），吸收 WASAPI 10ms 分包边界与 OS 调度微抖，避免误抽干
    const size_t ms20 = (static_cast<size_t>(sample_rate_) * 20) / 1000;
    const size_t ms45 = (static_cast<size_t>(sample_rate_) * 45) / 1000;
    const size_t ms30 = (static_cast<size_t>(sample_rate_) * 30) / 1000;
    const size_t ms10 = (static_cast<size_t>(sample_rate_) * 10) / 1000;
    preroll_frames_    = frame_samples_ + ms20;
    target_frames_     = std::max(frame_samples_ * 2, ms45);
    low_water_frames_  = frame_samples_ + ms10;
    high_water_frames_ = target_frames_ + ms30;
    max_frames_        = std::max(high_water_frames_ + frame_samples_ * 2, static_cast<size_t>(sample_rate_) / 4);
    primed_ = false;
    last_l_ = 0;
    last_r_ = 0;
    ring_.clear();
}

void SmoothLoopbackFifo::push_interleaved_stereo(const int16_t* stereo_pcm, size_t frames) {
    if (!stereo_pcm || frames == 0) return;
    ring_.insert(ring_.end(), stereo_pcm, stereo_pcm + frames * 2);
}

void SmoothLoopbackFifo::pop_smooth_frame(int16_t* dst, size_t out_frames) {
    if (!dst || out_frames == 0) return;

    auto fade_tail_to_zero = [&](size_t start_frame) {
        int32_t l = last_l_;
        int32_t r = last_r_;
        for (size_t i = start_frame; i < out_frames; ++i) {
            l = (l * 240) / 256;
            r = (r * 240) / 256;
            if (std::abs(l) < 2) l = 0;
            if (std::abs(r) < 2) r = 0;
            dst[i * 2]     = static_cast<int16_t>(l);
            dst[i * 2 + 1] = static_cast<int16_t>(r);
        }
        last_l_ = static_cast<int16_t>(l);
        last_r_ = static_cast<int16_t>(r);
    };

    if (!primed_) {
        if (available_frames() < preroll_frames_) {
            fade_tail_to_zero(0);
            return;
        }
        primed_ = true;
    }

    // 极端积压保护（>200ms）：裁剪到目标水位，并在拼接点做 64 样本线性交叉淡化，杜绝硬切爆音
    if (available_frames() > max_frames_) {
        const size_t drop = available_frames() - target_frames_;
        const size_t xfade = std::min<size_t>({64, drop, target_frames_});
        std::vector<int16_t> head_save(xfade * 2);
        for (size_t i = 0; i < xfade * 2; ++i) {
            head_save[i] = ring_[i];
        }
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(drop * 2));
        for (size_t i = 0; i < xfade; ++i) {
            const double w = static_cast<double>(i + 1) / static_cast<double>(xfade + 1);
            const double l = head_save[i * 2] * (1.0 - w) + ring_[i * 2] * w;
            const double r = head_save[i * 2 + 1] * (1.0 - w) + ring_[i * 2 + 1] * w;
            ring_[i * 2]     = static_cast<int16_t>(std::clamp(std::lround(l), -32768L, 32767L));
            ring_[i * 2 + 1] = static_cast<int16_t>(std::clamp(std::lround(r), -32768L, 32767L));
        }
    }

    const size_t avail = available_frames();
    // 软时钟漂移补偿（±1 样本连续一阶相位插值）：消化声卡晶振与系统单调时钟之间的 ppm 级频偏
    if (out_frames >= 4 && avail >= high_water_frames_ && avail >= out_frames + 1) {
        const size_t consume = out_frames + 1;
        const double step = static_cast<double>(consume) / static_cast<double>(out_frames);
        for (size_t i = 0; i < out_frames; ++i) {
            const double pos = static_cast<double>(i) * step;
            const size_t i0 = static_cast<size_t>(pos);
            const size_t i1 = std::min(i0 + 1, consume - 1);
            const double frac = pos - static_cast<double>(i0);
            const double l = ring_[i0 * 2] * (1.0 - frac) + ring_[i1 * 2] * frac;
            const double r = ring_[i0 * 2 + 1] * (1.0 - frac) + ring_[i1 * 2 + 1] * frac;
            dst[i * 2]     = static_cast<int16_t>(std::clamp(std::lround(l), -32768L, 32767L));
            dst[i * 2 + 1] = static_cast<int16_t>(std::clamp(std::lround(r), -32768L, 32767L));
        }
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(consume * 2));
        last_l_ = dst[(out_frames - 1) * 2];
        last_r_ = dst[(out_frames - 1) * 2 + 1];
        return;
    }

    if (out_frames >= 4 && avail < low_water_frames_ && avail >= out_frames) {
        const size_t consume = out_frames - 1;
        const double step = static_cast<double>(consume) / static_cast<double>(out_frames);
        for (size_t i = 0; i < out_frames; ++i) {
            const double pos = static_cast<double>(i) * step;
            const size_t i0 = static_cast<size_t>(pos);
            const size_t i1 = std::min(i0 + 1, out_frames - 1);
            const double frac = pos - static_cast<double>(i0);
            const double l = ring_[i0 * 2] * (1.0 - frac) + ring_[i1 * 2] * frac;
            const double r = ring_[i0 * 2 + 1] * (1.0 - frac) + ring_[i1 * 2 + 1] * frac;
            dst[i * 2]     = static_cast<int16_t>(std::clamp(std::lround(l), -32768L, 32767L));
            dst[i * 2 + 1] = static_cast<int16_t>(std::clamp(std::lround(r), -32768L, 32767L));
        }
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(consume * 2));
        last_l_ = dst[(out_frames - 1) * 2];
        last_r_ = dst[(out_frames - 1) * 2 + 1];
        return;
    }

    if (avail >= out_frames) {
        for (size_t i = 0; i < out_frames * 2; ++i) {
            dst[i] = ring_[i];
        }
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(out_frames * 2));
        last_l_ = dst[(out_frames - 1) * 2];
        last_r_ = dst[(out_frames - 1) * 2 + 1];
        return;
    }

    // 真实源饥饿（avail < out_frames）：先输出全部已缓存样本，再平滑衰减至零并重新进入预缓冲
    for (size_t i = 0; i < avail * 2; ++i) {
        dst[i] = ring_[i];
    }
    if (avail > 0) {
        last_l_ = dst[(avail - 1) * 2];
        last_r_ = dst[(avail - 1) * 2 + 1];
    }
    ring_.clear();
    primed_ = false;
    fade_tail_to_zero(avail);
}

AudioPipeline::~AudioPipeline() {
    stop();
}

void AudioPipeline::recover_playback_route() {
#ifdef _WIN32
    WindowsPlaybackRoute route;
    route.recover();
#endif
}

bool AudioPipeline::is_loopback_supported() {
#if defined(_WIN32) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::vector<LoopbackSourceInfo> AudioPipeline::list_loopback_sources() {
    std::vector<LoopbackSourceInfo> out;
#if defined(__linux__)
    out.push_back({"@DEFAULT_MONITOR@", "系统默认输出环回 (@DEFAULT_MONITOR@)"});
    FILE* fp = popen("pactl list short sources 2>/dev/null", "r");
    if (fp) {
        char line[512];
        while (std::fgets(line, sizeof(line), fp)) {
            std::istringstream iss(line);
            std::string idx, name;
            if (iss >> idx >> name) {
                if (name.find(".monitor") != std::string::npos && is_safe_linux_source_name(name)) {
                    out.push_back({name, name});
                }
            }
        }
        pclose(fp);
    }
#elif defined(_WIN32)
    out.push_back({"default", "系统默认输出直抓（原生 WASAPI 零延迟，免虚拟声卡）"});
    out.push_back({"@TAKEOVER_AUTO@", "智能静默接管（自动路由至本机数字/虚拟输出并静音主外放）"});
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
        IMMDeviceEnumerator* enumerator = nullptr;
        if (create_device_enumerator(&enumerator) && enumerator) {
            for (const auto& ep : enum_active_render_endpoints(enumerator)) {
                const std::string id_u8 = wide_to_utf8(ep.id);
                const std::string name_u8 = wide_to_utf8(ep.name);
                if (!id_u8.empty() && !name_u8.empty()) {
                    out.push_back({"loopback:" + id_u8, "直抓：" + name_u8});
                    out.push_back({"takeover:" + id_u8, "接管并重定向至：" + name_u8});
                }
            }
            enumerator->Release();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
#endif
    return out;
}

void AudioPipeline::set_loopback_command_override_for_test(std::vector<std::string> argv) {
    std::lock_guard<std::mutex> lock(param_mu_);
    test_loopback_argv_ = std::move(argv);
    source_gen_.fetch_add(1);
}

bool AudioPipeline::decode_wav_file(const std::string& path,
                                    uint32_t target_rate,
                                    std::vector<int16_t>& out_stereo_pcm,
                                    AudioTrackMeta& out_meta,
                                    std::string& out_error) {
    out_stereo_pcm.clear();
    out_error.clear();
    if (target_rate == 0) target_rate = 48000;

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        out_error = "无法打开 WAV 文件";
        return false;
    }
    const std::streamoff file_size_off = ifs.tellg();
    if (file_size_off < 44) {
        out_error = "WAV 文件过短或已损坏";
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_off);
    if (file_size > kMaxWavBytes) {
        out_error = "WAV 文件超过 128 MB 大小限制";
        return false;
    }
    ifs.seekg(0, std::ios::beg);

    uint8_t riff_hdr[12]{};
    ifs.read(reinterpret_cast<char*>(riff_hdr), 12);
    if (ifs.gcount() != 12 || std::memcmp(riff_hdr, "RIFF", 4) != 0 || std::memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        out_error = "不是有效的 RIFF/WAVE 文件";
        return false;
    }

    const uint32_t riff_declared_size = read_u32_le(riff_hdr + 4);
    const uint64_t container_limit = std::min<uint64_t>(file_size, 8ULL + static_cast<uint64_t>(riff_declared_size));

    bool has_fmt = false;
    uint16_t fmt_tag = 0;
    uint16_t channels = 0;
    uint32_t src_rate = 0;
    uint16_t bits_per_sample = 0;
    uint16_t block_align = 0;

    bool has_data = false;
    uint64_t data_offset = 0;
    uint32_t data_size = 0;

    uint64_t cursor = 12;
    while (cursor + 8 <= container_limit) {
        ifs.seekg(static_cast<std::streamoff>(cursor), std::ios::beg);
        uint8_t chdr[8]{};
        ifs.read(reinterpret_cast<char*>(chdr), 8);
        if (ifs.gcount() != 8) {
            out_error = "WAV 块头读取不完整";
            return false;
        }
        const uint32_t chunk_sz = read_u32_le(chdr + 4);
        const uint64_t payload_start = cursor + 8;
        if (payload_start + static_cast<uint64_t>(chunk_sz) > file_size) {
            out_error = "WAV 块长度超出文件实际边界";
            return false;
        }

        if (std::memcmp(chdr, "fmt ", 4) == 0) {
            if (chunk_sz < 16) {
                out_error = "WAV fmt 块不完整";
                return false;
            }
            std::vector<uint8_t> fmt_buf(std::min<uint32_t>(chunk_sz, 64));
            ifs.read(reinterpret_cast<char*>(fmt_buf.data()), static_cast<std::streamsize>(fmt_buf.size()));
            if (static_cast<size_t>(ifs.gcount()) != fmt_buf.size()) {
                out_error = "读取 WAV fmt 块失败";
                return false;
            }
            fmt_tag = read_u16_le(fmt_buf.data() + 0);
            channels = read_u16_le(fmt_buf.data() + 2);
            src_rate = read_u32_le(fmt_buf.data() + 4);
            block_align = read_u16_le(fmt_buf.data() + 12);
            bits_per_sample = read_u16_le(fmt_buf.data() + 14);
            if (fmt_tag == 0xFFFE && fmt_buf.size() >= 40) {
                // WAVE_FORMAT_EXTENSIBLE: 前两字节为真实 SubFormat tag (1=PCM, 3=IEEE Float)
                fmt_tag = read_u16_le(fmt_buf.data() + 24);
            }
            has_fmt = true;
        } else if (std::memcmp(chdr, "data", 4) == 0) {
            data_offset = payload_start;
            data_size = chunk_sz;
            has_data = true;
            if (has_fmt) break;
        }

        // RIFF 规范：奇数长度块后带 1 字节填充对齐到偶数字节边界
        const uint64_t padded_sz = static_cast<uint64_t>(chunk_sz) + (chunk_sz & 1U);
        if (payload_start + padded_sz < cursor) break;
        cursor = payload_start + padded_sz;
    }

    if (!has_fmt || !has_data) {
        out_error = !has_fmt ? "WAV 缺少 fmt 格式块" : "WAV 缺少 data 音频数据块";
        return false;
    }
    if (channels != 1 && channels != 2) {
        out_error = "仅支持单声道或立体声 WAV（当前声道数: " + std::to_string(channels) + "）";
        return false;
    }
    if (src_rate < 8000 || src_rate > 192000) {
        out_error = "不支持的 WAV 采样率: " + std::to_string(src_rate) + " Hz";
        return false;
    }
    const bool is_pcm16 = (fmt_tag == 1 && bits_per_sample == 16);
    const bool is_pcm24 = (fmt_tag == 1 && bits_per_sample == 24);
    const bool is_flt32 = (fmt_tag == 3 && bits_per_sample == 32);
    if (!is_pcm16 && !is_pcm24 && !is_flt32) {
        out_error = "仅支持 16/24 位 PCM 或 32 位浮点 WAV";
        return false;
    }

    const uint32_t expected_align = static_cast<uint32_t>(channels) * (bits_per_sample / 8U);
    if (expected_align == 0 || (block_align != 0 && block_align != expected_align)) {
        out_error = "WAV blockAlign 与声道/位宽不一致";
        return false;
    }
    if (data_size == 0 || (data_size % expected_align) != 0) {
        out_error = "WAV data 长度未按采样帧对齐（存在截断或奇数字节）";
        return false;
    }

    std::vector<uint8_t> raw(data_size);
    ifs.seekg(static_cast<std::streamoff>(data_offset), std::ios::beg);
    ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(data_size));
    if (static_cast<uint64_t>(ifs.gcount()) != data_size) {
        out_error = "WAV 数据读取短缺（文件已截断）";
        return false;
    }

    const size_t src_frames = data_size / expected_align;
    if (src_frames == 0) {
        out_error = "WAV 不包含有效音频帧";
        return false;
    }

    // 统一转为立体声 int16
    std::vector<int16_t> stereo_src(src_frames * 2);
    const size_t bytes_per_sample = bits_per_sample / 8U;
    auto read_sample_i16 = [&](const uint8_t* p) -> int16_t {
        if (is_pcm16) {
            return static_cast<int16_t>(read_u16_le(p));
        }
        if (is_pcm24) {
            int32_t v = static_cast<int32_t>(p[0]) |
                        (static_cast<int32_t>(p[1]) << 8) |
                        (static_cast<int32_t>(p[2]) << 16);
            if (v & 0x800000) v |= ~0xFFFFFF; // 符号扩展
            return static_cast<int16_t>(v >> 8);
        }
        float f = 0.0f;
        std::memcpy(&f, p, sizeof(float));
        if (!std::isfinite(f)) f = 0.0f;
        f = std::clamp(f, -1.0f, 1.0f);
        return static_cast<int16_t>(std::lround(f * 32767.0f));
    };

    for (size_t i = 0; i < src_frames; ++i) {
        const uint8_t* fp = raw.data() + i * expected_align;
        int16_t l = read_sample_i16(fp);
        int16_t r = (channels == 2) ? read_sample_i16(fp + bytes_per_sample) : l;
        stereo_src[i * 2] = l;
        stereo_src[i * 2 + 1] = r;
    }

    // 如采样率与目标不一致，执行线性重采样到 target_rate
    if (src_rate == target_rate) {
        out_stereo_pcm = std::move(stereo_src);
    } else {
        const double ratio = static_cast<double>(src_rate) / static_cast<double>(target_rate);
        const size_t dst_frames = std::max<size_t>(1, static_cast<size_t>(std::llround(src_frames / ratio)));
        out_stereo_pcm.resize(dst_frames * 2);
        for (size_t o = 0; o < dst_frames; ++o) {
            double pos = o * ratio;
            size_t i0 = static_cast<size_t>(pos);
            if (i0 >= src_frames) i0 = src_frames - 1;
            size_t i1 = std::min(i0 + 1, src_frames - 1);
            double frac = pos - static_cast<double>(i0);
            double l = stereo_src[i0 * 2] * (1.0 - frac) + stereo_src[i1 * 2] * frac;
            double r = stereo_src[i0 * 2 + 1] * (1.0 - frac) + stereo_src[i1 * 2 + 1] * frac;
            out_stereo_pcm[o * 2] = static_cast<int16_t>(std::clamp(std::lround(l), -32768L, 32767L));
            out_stereo_pcm[o * 2 + 1] = static_cast<int16_t>(std::clamp(std::lround(r), -32768L, 32767L));
        }
    }

    auto slash = path.find_last_of("/\\");
    out_meta.title = (slash == std::string::npos) ? path : path.substr(slash + 1);
    out_meta.artist = "Local WAV (" + std::to_string(src_rate / 1000) + "." +
                      std::to_string((src_rate % 1000) / 100) + "kHz " +
                      std::to_string(bits_per_sample) + "bit)";
    out_meta.album = "Built-in Player";
    out_meta.duration_ms = (out_stereo_pcm.size() / 2) * 1000ULL / target_rate;
    out_meta.position_ms = 0;
    return true;
}

bool AudioPipeline::set_source_mode(AudioSourceMode mode, const std::string& param, std::string* err_out) {
    if (mode == AudioSourceMode::SystemLoopback) {
        if (!is_loopback_supported()) {
            std::string err = "当前平台暂未实现系统全局环回抓音，请使用本地 WAV 或测试音";
            {
                std::lock_guard<std::mutex> lock(param_mu_);
                last_error_ = err;
            }
            if (err_out) *err_out = err;
            return false;
        }
#if defined(__linux__)
        if (!param.empty() && !is_safe_linux_source_name(param)) {
            std::string err = "无效的 Linux 环回音源名称";
            {
                std::lock_guard<std::mutex> lock(param_mu_);
                last_error_ = err;
            }
            if (err_out) *err_out = err;
            return false;
        }
#endif
    }

    if (mode == AudioSourceMode::WavFile) {
        if (param.empty()) {
            std::string err = "未指定 WAV 文件路径";
            {
                std::lock_guard<std::mutex> lock(param_mu_);
                last_error_ = err;
            }
            if (err_out) *err_out = err;
            return false;
        }
        uint32_t target_sr = 48000;
        {
            std::lock_guard<std::mutex> lock(param_mu_);
            target_sr = sample_rate_ > 0 ? sample_rate_ : 48000;
        }
        std::vector<int16_t> decoded;
        AudioTrackMeta new_meta;
        std::string err;
        if (!decode_wav_file(param, target_sr, decoded, new_meta, err)) {
            // 加载失败时完整保留上一个有效状态，不污染当前曲目与音源模式
            {
                std::lock_guard<std::mutex> lock(param_mu_);
                last_error_ = err;
            }
            if (err_out) *err_out = err;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(param_mu_);
            wav_pcm_ = std::move(decoded);
            wav_path_ = param;
            wav_decoded_rate_ = target_sr;
            wav_cursor_ = 0;
            source_param_ = param;
            meta_ = std::move(new_meta);
            last_error_.clear();
        }
        mode_.store(mode);
        source_gen_.fetch_add(1);
        stop_loopback_process();
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(param_mu_);
        source_param_ = param;
        last_error_.clear();
        if (mode == AudioSourceMode::TestTone) {
            meta_.title = "全屋同步校准测试音 (A4/C#5/E5)";
            meta_.artist = "MiPlay Core Synthesizer";
            meta_.album = "Multi-Room Sync Test";
            meta_.duration_ms = 0;
            meta_.position_ms = 0;
        } else if (mode == AudioSourceMode::SystemLoopback) {
#ifdef _WIN32
            meta_.title = "Windows 系统音频直抓 / 输出接管";
#else
            meta_.title = "系统全局音频环回";
#endif
            meta_.artist = "Native WASAPI Loopback";
            meta_.album = param.empty() ? "default" : param;
            meta_.duration_ms = 0;
            meta_.position_ms = 0;
        }
    }
    mode_.store(mode);
    source_gen_.fetch_add(1);
    stop_loopback_process();
    return true;
}

std::string AudioPipeline::source_param() const {
    std::lock_guard<std::mutex> lock(param_mu_);
    return source_param_;
}

std::string AudioPipeline::last_error() const {
    std::lock_guard<std::mutex> lock(param_mu_);
    return last_error_;
}

AudioTrackMeta AudioPipeline::current_meta() const {
    std::lock_guard<std::mutex> lock(param_mu_);
    return meta_;
}

void AudioPipeline::stop_loopback_process() {
#if defined(__linux__)
    int fd = -1;
    int pid = -1;
    {
        std::lock_guard<std::mutex> lk(proc_mu_);
        fd = loopback_fd_;
        pid = loopback_pid_;
        loopback_fd_ = -1;
        loopback_pid_ = -1;
    }
    if (fd >= 0) {
        ::close(fd);
    }
    if (pid > 0) {
        ::kill(pid, SIGTERM);
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 15; ++i) {
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
    }
#endif
}

bool AudioPipeline::start(uint32_t sample_rate, uint32_t frame_samples,
                          PcmFrameCallback pcm_cb, SpectrumCallback spec_cb,
                          std::function<bool()> has_streaming_receiver) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    frame_samples_ = frame_samples > 0 ? frame_samples : 480;

    // 若已加载 WAV 且目标采样率变化，自动按新采样率重新解码对齐播速
    {
        std::string wav_path_copy;
        uint32_t decoded_sr = 0;
        {
            std::lock_guard<std::mutex> lock(param_mu_);
            wav_path_copy = wav_path_;
            decoded_sr = wav_decoded_rate_;
        }
        if (!wav_path_copy.empty() && decoded_sr != sample_rate_) {
            std::vector<int16_t> redecoded;
            AudioTrackMeta new_meta;
            std::string err;
            if (decode_wav_file(wav_path_copy, sample_rate_, redecoded, new_meta, err)) {
                std::lock_guard<std::mutex> lock(param_mu_);
                wav_pcm_ = std::move(redecoded);
                wav_decoded_rate_ = sample_rate_;
                wav_cursor_ = 0;
                meta_ = std::move(new_meta);
            }
        }
    }

    pcm_cb_ = std::move(pcm_cb);
    spec_cb_ = std::move(spec_cb);
    has_streaming_receiver_ = std::move(has_streaming_receiver);
    running_.store(true);
    try {
        if (spec_cb_) spectrum_worker_ = std::thread(&AudioPipeline::spectrum_loop, this);
        worker_ = std::thread(&AudioPipeline::worker_loop, this);
    } catch (...) {
        stop();
        throw;
    }
    return true;
}

void AudioPipeline::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(spectrum_mu_);
        pending_spectrum_.reset();
    }
    spectrum_cv_.notify_one();
    stop_loopback_process();
    if (worker_.joinable()) worker_.join();
    if (spectrum_worker_.joinable()) spectrum_worker_.join();
    stop_loopback_process();
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
    {
        // Spectrum is replaceable UI data. The audio worker must never wait for publication.
        std::unique_lock<std::mutex> lock(spectrum_mu_, std::try_to_lock);
        if (!lock.owns_lock() || !running_.load()) return;
        pending_spectrum_ = bands;
    }
    spectrum_cv_.notify_one();
}

void AudioPipeline::spectrum_loop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(spectrum_mu_);
        spectrum_cv_.wait(lock, [this] { return pending_spectrum_ || !running_.load(); });
        if (!running_.load()) return;
        auto bands = *pending_spectrum_;
        pending_spectrum_.reset();
        lock.unlock();
        spec_cb_(bands);
    }
}

void AudioPipeline::worker_loop() {
    std::vector<int16_t> frame(frame_samples_ * 2, 0);
    // 使用有理数样点锚点计算绝对节拍时刻，彻底消除 1024/48000=21333.33us 的整数截断漂移
    uint64_t tick_origin_us = sync::monotonic_now_us();
    uint64_t tick_samples = 0;
    uint64_t sample_idx = 0;
    uint64_t spec_div = 0;
    uint64_t last_gen = source_gen_.load();

#ifdef _WIN32
    WinProAudioThreadGuard pro_audio_guard;
    WindowsPlaybackRoute playback_route;
    WasapiCapture wasapi;
    std::chrono::steady_clock::time_point next_route_attempt{};
    std::chrono::steady_clock::time_point next_default_check{};
    bool was_streaming_active = false;
    std::unique_ptr<void, decltype(&CloseHandle)> frame_timer(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                               TIMER_MODIFY_STATE | SYNCHRONIZE),
        &CloseHandle);
#elif defined(__linux__)
    std::vector<uint8_t> loopback_rem_bytes;
    SmoothLoopbackFifo linux_fifo;
    linux_fifo.reset(sample_rate_, frame_samples_);
    auto spawn_linux_loopback = [&]() {
        stop_loopback_process();
        std::vector<std::string> argv;
        {
            std::lock_guard<std::mutex> lk(param_mu_);
            if (!test_loopback_argv_.empty()) {
                argv = test_loopback_argv_;
            } else {
                std::string dev = source_param_.empty() ? "@DEFAULT_MONITOR@" : source_param_;
                argv = {
                    "parec",
                    "--format=s16le",
                    "--channels=2",
                    "--rate=" + std::to_string(sample_rate_),
                    "--latency-msec=10",
                    "-d",
                    dev
                };
            }
        }
        int pipefd[2]{-1, -1};
        if (::pipe(pipefd) != 0) return;
        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return;
        }
        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (auto& a : argv) cargv.push_back(a.data());
            cargv.push_back(nullptr);
            ::execvp(cargv[0], cargv.data());
            _exit(127);
        }
        ::close(pipefd[1]);
        int flags = ::fcntl(pipefd[0], F_GETFL, 0);
        if (flags >= 0) ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        {
            std::lock_guard<std::mutex> lk(proc_mu_);
            if (!running_.load()) {
                ::close(pipefd[0]);
                ::kill(pid, SIGTERM);
                ::waitpid(pid, nullptr, 0);
                return;
            }
            loopback_fd_ = pipefd[0];
            loopback_pid_ = static_cast<int>(pid);
        }
        loopback_rem_bytes.clear();
        linux_fifo.reset(sample_rate_, frame_samples_);
    };
#endif

    while (running_.load()) {
        const auto cur_mode = mode_.load();
        const uint64_t cur_gen = source_gen_.load();
        std::fill(frame.begin(), frame.end(), 0);

#ifdef _WIN32
        const bool is_loopback_mode = (cur_mode == AudioSourceMode::SystemLoopback);
        const bool is_streaming = has_streaming_receiver_ && has_streaming_receiver_();
        std::string win_param;
        if (is_loopback_mode) {
            std::lock_guard<std::mutex> lk(param_mu_);
            win_param = source_param_;
        }
        const bool want_takeover = is_loopback_mode &&
                                   (win_param == "@TAKEOVER_AUTO@" || win_param.rfind("takeover:", 0) == 0);
        if (!is_loopback_mode || cur_gen != last_gen ||
            (want_takeover && is_streaming != was_streaming_active)) {
            if (wasapi.is_active()) wasapi.stop();
            if (playback_route.is_active()) playback_route.stop();
            next_route_attempt = {};
        }
        was_streaming_active = is_streaming;
#elif defined(__linux__)
        if (cur_mode != AudioSourceMode::SystemLoopback || cur_gen != last_gen) {
            stop_loopback_process();
            loopback_rem_bytes.clear();
            linux_fifo.reset(sample_rate_, frame_samples_);
        }
#endif
        last_gen = cur_gen;

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
            const auto now = std::chrono::steady_clock::now();
            // 热跟随系统默认输出设备切换（例如用户插拔耳机或切换任务栏输出设备）
            if (wasapi.is_active() && !want_takeover && now >= next_default_check) {
                next_default_check = now + std::chrono::milliseconds(500);
                if (wasapi.default_endpoint_changed()) {
                    wasapi.stop();
                    next_route_attempt = {};
                }
            }
            if (!wasapi.is_active() && now >= next_route_attempt) {
                next_route_attempt = now + std::chrono::milliseconds(800);
                std::wstring target_ep;
                if (want_takeover && is_streaming) {
                    std::wstring req_takeover;
                    if (win_param.rfind("takeover:", 0) == 0) {
                        req_takeover = utf8_to_wide(std::string_view(win_param).substr(9));
                    }
                    if (playback_route.start(req_takeover)) {
                        target_ep = playback_route.capture_endpoint();
                    }
                } else if (win_param.rfind("loopback:", 0) == 0) {
                    target_ep = utf8_to_wide(std::string_view(win_param).substr(9));
                }
                // 优先直接用原生 WASAPI eRender Loopback 抓取目标端点；若静默接管无可用次级端点则无缝回退默认输出直抓
                if (!wasapi.start(sample_rate_, frame_samples_, target_ep)) {
                    if (playback_route.is_active()) playback_route.stop();
                    if (!target_ep.empty()) {
                        wasapi.start(sample_rate_, frame_samples_, {});
                    }
                }
                tick_origin_us = sync::monotonic_now_us();
                tick_samples = 0;
            }
            if (wasapi.is_active()) wasapi.read_stereo_frame(frame.data(), frame_samples_);
#elif defined(__linux__)
            int fd = -1;
            {
                std::lock_guard<std::mutex> lk(proc_mu_);
                fd = loopback_fd_;
            }
            if (fd < 0 && running_.load()) {
                spawn_linux_loopback();
                std::lock_guard<std::mutex> lk(proc_mu_);
                fd = loopback_fd_;
                tick_origin_us = sync::monotonic_now_us();
                tick_samples = 0;
            }
            if (fd >= 0) {
                // 非阻塞吸尽当前管道可读数据推入平滑缓冲，不在稳态阻塞拖慢节拍
                uint8_t tmp[8192];
                for (;;) {
                    ssize_t n = ::read(fd, tmp, sizeof(tmp));
                    if (n > 0) {
                        loopback_rem_bytes.insert(loopback_rem_bytes.end(), tmp, tmp + n);
                    } else if (n == 0) {
                        stop_loopback_process();
                        break;
                    } else {
                        if (errno == EINTR) continue;
                        if (errno != EAGAIN && errno != EWOULDBLOCK) stop_loopback_process();
                        break;
                    }
                }
                const size_t full_frames = loopback_rem_bytes.size() / 4;
                if (full_frames > 0) {
                    std::vector<int16_t> aligned(full_frames * 2);
                    std::memcpy(aligned.data(), loopback_rem_bytes.data(), full_frames * 4);
                    linux_fifo.push_interleaved_stereo(aligned.data(), full_frames);
                    loopback_rem_bytes.erase(loopback_rem_bytes.begin(),
                                             loopback_rem_bytes.begin() + static_cast<ptrdiff_t>(full_frames * 4));
                }
                linux_fifo.pop_smooth_frame(frame.data(), frame_samples_);
            }
#endif
        }

        if (!running_.load()) break;
        if (pcm_cb_) {
            pcm_cb_(frame.data(), frame_samples_);
        }
        if (++spec_div % 3 == 0) {
            compute_spectrum(frame.data(), frame_samples_);
        }

        tick_samples += frame_samples_;
        const uint64_t next_tick_us = tick_origin_us + (tick_samples * 1000000ULL) / sample_rate_;
        uint64_t now = sync::monotonic_now_us();
        if (next_tick_us > now) {
            uint64_t rem = next_tick_us - now;
            while (running_.load() && rem > 0) {
                const auto sleep_us = std::min<uint64_t>(rem, 25000ULL);
#ifdef _WIN32
                if (frame_timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>(sleep_us) * 10;
                    if (!SetWaitableTimer(frame_timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
                        WaitForSingleObject(frame_timer.get(), INFINITE) != WAIT_OBJECT_0) {
                        frame_timer.reset();
                    }
                } else
#endif
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                }
                uint64_t cur = sync::monotonic_now_us();
                rem = (next_tick_us > cur) ? (next_tick_us - cur) : 0;
            }
        } else if (now - next_tick_us > 80000ULL) {
            tick_origin_us = now;
            tick_samples = 0;
        }
    }

#ifdef _WIN32
    wasapi.stop();
    playback_route.stop();
#elif defined(__linux__)
    stop_loopback_process();
#endif
}

} // namespace miplay::media
