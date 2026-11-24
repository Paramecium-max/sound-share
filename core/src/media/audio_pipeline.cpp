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

std::wstring find_virtual_endpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    IMMDeviceCollection* devices = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) return {};
    std::wstring id;
    std::wstring line_output;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count && id.empty(); ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(i, &device))) continue;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT name{};
            if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
                const std::wstring_view label(name.pwszVal);
                if (label.find(L"VB-Audio Virtual Cable") != std::wstring_view::npos) {
                    // Recent drivers label the stereo speaker pin using the Windows display language.
                    if (label.find(L"16 Ch") == std::wstring_view::npos) id = endpoint_id(device);
                    else line_output = endpoint_id(device);
                }
            }
            PropVariantClear(&name);
            store->Release();
        }
        device->Release();
    }
    devices->Release();
    return id.empty() ? line_output : id;
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

    static bool is_available() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
        IMMDeviceEnumerator* enumerator = nullptr;
        bool available = false;
        if (create_device_enumerator(&enumerator)) {
            available = !find_virtual_endpoint(enumerator, eRender).empty() &&
                        !find_virtual_endpoint(enumerator, eCapture).empty();
            enumerator->Release();
        }
        if (SUCCEEDED(com)) CoUninitialize();
        return available;
    }

    void recover() {
        if (recovery_file_.empty() || GetFileAttributesW(recovery_file_.c_str()) == INVALID_FILE_ATTRIBUTES ||
            !open()) return;
        read_saved_route();
        stop();
    }

    bool start() {
        if (!open()) return false;
        if (read_saved_route() && !restore_endpoints()) {
            stop();
            return false;
        }
        virtual_endpoint_ = find_virtual_endpoint(enumerator_, eRender);
        capture_endpoint_ = find_virtual_endpoint(enumerator_, eCapture);
        for (size_t i = 0; i < kPlaybackRoles.size(); ++i) {
            previous_[i] = default_playback_endpoint(enumerator_, kPlaybackRoles[i]);
        }
        if (capture_endpoint_.empty() || !valid_saved_route() || !record_route()) {
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
        return true;
    }

    bool is_active() const { return !virtual_endpoint_.empty(); }
    const std::wstring& capture_endpoint() const { return capture_endpoint_; }

    void stop() {
        if (policy_ && !virtual_endpoint_.empty()) {
            if (restore_endpoints() && !recovery_file_.empty()) DeleteFileW(recovery_file_.c_str());
        }
        virtual_endpoint_.clear();
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

class WasapiCapture {
public:
    ~WasapiCapture() { stop(); }

    bool start(uint32_t target_rate, const std::wstring& endpoint) {
        stop();
        target_rate_ = target_rate;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_init_ = SUCCEEDED(hr);

        static const IID iid_ac       = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
        static const IID iid_acc      = {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};

        if (!create_device_enumerator(&enumerator_) ||
            FAILED(enumerator_->GetDevice(endpoint.c_str(), &device_))) {
            stop(); return false;
        }
        if (FAILED(device_->Activate(iid_ac, CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&audio_client_)))) {
            stop(); return false;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = target_rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * sizeof(int16_t);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (FAILED(audio_client_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                1000000, 0, &format, nullptr))) {
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
        if (device_) { device_->Release(); device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (com_init_) { CoUninitialize(); com_init_ = false; }
        pcm_queue_.clear();
    }

    bool is_active() const { return active_; }

    void read_stereo_frame(int16_t* dst, size_t out_frames) {
        if (!active_) {
            std::memset(dst, 0, out_frames * 2 * sizeof(int16_t));
            return;
        }
        UINT32 pkt_len = 0;
        HRESULT next = S_OK;
        while (SUCCEEDED(next = capture_client_->GetNextPacketSize(&pkt_len)) && pkt_len > 0) {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            if (FAILED(next = capture_client_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr))) break;

            const size_t samples = static_cast<size_t>(num_frames) * 2;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                pcm_queue_.insert(pcm_queue_.end(), samples, 0);
            } else {
                const auto* pcm = reinterpret_cast<const int16_t*>(data);
                pcm_queue_.insert(pcm_queue_.end(), pcm, pcm + samples);
            }
            capture_client_->ReleaseBuffer(num_frames);
        }

        if (FAILED(next)) {
            stop();
            std::memset(dst, 0, out_frames * 2 * sizeof(int16_t));
            return;
        }

        const size_t need = out_frames * 2;
        if (pcm_queue_.size() >= need) {
            std::memcpy(dst, pcm_queue_.data(), need * sizeof(int16_t));
            pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + static_cast<ptrdiff_t>(need));
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
    uint32_t target_rate_ = 48000;
    std::vector<int16_t> pcm_queue_;
};
#endif

} // namespace

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
    out.push_back({"default", "VB-CABLE 虚拟扬声器（Windows 投送）"});
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
#ifdef _WIN32
        if (!WindowsPlaybackRoute::is_available()) {
            const std::string err = "投送本机声音需要 VB-CABLE 虚拟扬声器";
            if (err_out) *err_out = err;
            return false;
        }
#endif
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
            meta_.title = "Windows 虚拟扬声器投送";
#else
            meta_.title = "系统全局音频环回";
#endif
            meta_.artist = "Desktop Loopback";
            meta_.album = param.empty() ? "@DEFAULT_MONITOR@" : param;
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
    const uint64_t frame_us = (static_cast<uint64_t>(frame_samples_) * 1000000ULL) / sample_rate_;
    uint64_t next_tick_us = sync::monotonic_now_us();
    uint64_t sample_idx = 0;
    uint64_t spec_div = 0;
    uint64_t last_gen = source_gen_.load();

#ifdef _WIN32
    WindowsPlaybackRoute playback_route;
    WasapiCapture wasapi;
    std::chrono::steady_clock::time_point next_route_attempt{};
    std::unique_ptr<void, decltype(&CloseHandle)> frame_timer(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                               TIMER_MODIFY_STATE | SYNCHRONIZE),
        &CloseHandle);
#elif defined(__linux__)
    std::vector<uint8_t> loopback_acc;
    const size_t frame_bytes = frame.size() * sizeof(int16_t);
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
                    "--latency-msec=20",
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
        loopback_acc.clear();
    };
#endif

    while (running_.load()) {
        const auto cur_mode = mode_.load();
        const uint64_t cur_gen = source_gen_.load();
        std::fill(frame.begin(), frame.end(), 0);

#ifdef _WIN32
        const bool route_audio = cur_mode == AudioSourceMode::SystemLoopback &&
                                 has_streaming_receiver_ && has_streaming_receiver_();
        if (!route_audio) {
            if (wasapi.is_active()) wasapi.stop();
            if (playback_route.is_active()) playback_route.stop();
            next_route_attempt = {};
        }
#elif defined(__linux__)
        if (cur_mode != AudioSourceMode::SystemLoopback || cur_gen != last_gen) {
            stop_loopback_process();
            loopback_acc.clear();
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
            if (route_audio && !wasapi.is_active() && now >= next_route_attempt) {
                next_route_attempt = now + std::chrono::seconds(1);
                if (!playback_route.is_active()) playback_route.start();
                if (playback_route.is_active() && !wasapi.start(sample_rate_, playback_route.capture_endpoint())) {
                    playback_route.stop();
                }
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
            }
            if (fd >= 0) {
                // 使用可中断的 poll + 非阻塞 read，保证无输出采集源时也能在 ~20ms 内响应停机或切源
                uint8_t tmp[4096];
                const uint64_t deadline = sync::monotonic_now_us() + std::min<uint64_t>(frame_us, 25000ULL);
                while (running_.load() && mode_.load() == AudioSourceMode::SystemLoopback &&
                       source_gen_.load() == last_gen && loopback_acc.size() < frame_bytes) {
                    struct pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    int pr = ::poll(&pfd, 1, 10);
                    if (pr > 0) {
                        if (pfd.revents & POLLIN) {
                            ssize_t n = ::read(fd, tmp, sizeof(tmp));
                            if (n > 0) {
                                loopback_acc.insert(loopback_acc.end(), tmp, tmp + n);
                            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                                stop_loopback_process();
                                break;
                            }
                        } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                            stop_loopback_process();
                            break;
                        }
                    } else if (pr == 0) {
                        if (sync::monotonic_now_us() >= deadline) break;
                    } else if (errno != EINTR) {
                        stop_loopback_process();
                        break;
                    }
                }
                if (loopback_acc.size() >= frame_bytes) {
                    std::memcpy(frame.data(), loopback_acc.data(), frame_bytes);
                    loopback_acc.erase(loopback_acc.begin(),
                                       loopback_acc.begin() + static_cast<ptrdiff_t>(frame_bytes));
                    if (loopback_acc.size() > frame_bytes * 6) {
                        loopback_acc.erase(loopback_acc.begin(),
                                           loopback_acc.begin() + static_cast<ptrdiff_t>(loopback_acc.size() - frame_bytes * 2));
                    }
                }
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

        next_tick_us += frame_us;
        uint64_t now = sync::monotonic_now_us();
        if (next_tick_us > now) {
            // 分段短休眠，确保 stop() 响应迅速
            uint64_t rem = next_tick_us - now;
            while (running_.load() && rem > 0) {
                const auto sleep_us = std::min<uint64_t>(rem, 10000ULL);
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
        } else if (now - next_tick_us > 100000ULL) {
            next_tick_us = now;
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
