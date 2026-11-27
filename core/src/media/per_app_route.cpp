#include "media/per_app_route.h"

#include <algorithm>
#include <charconv>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#endif

namespace miplay::media {

std::string PerAppSilentRouter::serialize_recovery_entries(const std::vector<RecoveryRouteEntry>& entries) {
    std::ostringstream oss;
    for (const auto& entry : entries) {
        if (entry.pid == 0) continue;
        std::string safe_token = entry.prior_token;
        safe_token.erase(std::remove_if(safe_token.begin(), safe_token.end(), [](unsigned char c) {
            return c == '\r' || c == '\n';
        }), safe_token.end());
        oss << entry.pid << '|' << entry.creation_time << '|' << safe_token << '\n';
    }
    return oss.str();
}

std::vector<RecoveryRouteEntry> PerAppSilentRouter::parse_recovery_entries(const std::string& content) {
    std::vector<RecoveryRouteEntry> out;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        const auto p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) continue;

        uint32_t pid = 0;
        uint64_t created = 0;
        auto r1 = std::from_chars(line.data(), line.data() + p1, pid);
        auto r2 = std::from_chars(line.data() + p1 + 1, line.data() + p2, created);
        if (r1.ec != std::errc{} || r1.ptr != line.data() + p1 || pid == 0) continue;
        if (r2.ec != std::errc{} || r2.ptr != line.data() + p2) continue;

        RecoveryRouteEntry entry;
        entry.pid = pid;
        entry.creation_time = created;
        entry.prior_token = line.substr(p2 + 1);
        out.push_back(std::move(entry));
    }
    return out;
}

#ifndef _WIN32

struct PerAppSilentRouter::Impl {
    SilentRouteStatus st{};
};

PerAppSilentRouter::~PerAppSilentRouter() {
    delete impl_;
    impl_ = nullptr;
}

void PerAppSilentRouter::recover_stale_routes() {}

bool PerAppSilentRouter::has_secondary_render_endpoint() {
    return false;
}

bool PerAppSilentRouter::apply_for_process_tree(uint32_t, SilentRouteStatus* out_status) {
    SilentRouteStatus st;
    st.supported = false;
    st.active = false;
    st.hint = "当前平台不支持 Windows 单应用静默副端点分流";
    if (out_status) *out_status = st;
    return false;
}

void PerAppSilentRouter::restore() {}

SilentRouteStatus PerAppSilentRouter::status() const {
    return impl_ ? impl_->st : SilentRouteStatus{};
}

#else

namespace {

using HSTRING_OPAQUE = void*;
using PfnRoGetActivationFactory = HRESULT(WINAPI*)(HSTRING_OPAQUE, REFIID, void**);
using PfnWindowsCreateString = HRESULT(WINAPI*)(const wchar_t*, UINT32, HSTRING_OPAQUE*);
using PfnWindowsDeleteString = HRESULT(WINAPI*)(HSTRING_OPAQUE);
using PfnWindowsGetStringRawBuffer = const wchar_t*(WINAPI*)(HSTRING_OPAQUE, UINT32*);

// Exact 28-slot VTable for Windows.Media.Internal.AudioPolicyConfig (Win10 1803+ / Win11).
struct IAudioPolicyConfigFactoryRaw {
    struct Vtbl {
        // IUnknown (0..2)
        HRESULT(STDMETHODCALLTYPE* QueryInterface)(IAudioPolicyConfigFactoryRaw*, REFIID, void**);
        ULONG(STDMETHODCALLTYPE* AddRef)(IAudioPolicyConfigFactoryRaw*);
        ULONG(STDMETHODCALLTYPE* Release)(IAudioPolicyConfigFactoryRaw*);
        // IInspectable (3..5)
        HRESULT(STDMETHODCALLTYPE* GetIids)(IAudioPolicyConfigFactoryRaw*, ULONG*, IID**);
        HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(IAudioPolicyConfigFactoryRaw*, HSTRING_OPAQUE*);
        HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(IAudioPolicyConfigFactoryRaw*, int*);
        // Internal slots (6..24)
        void* slot6_add_CtxVolumeChange;
        void* slot7_remove_CtxVolumeChanged;
        void* slot8_add_RingerVibrateStateChanged;
        void* slot9_remove_RingerVibrateStateChange;
        void* slot10_SetVolumeGroupGainForId;
        void* slot11_GetVolumeGroupGainForId;
        void* slot12_GetActiveVolumeGroupForEndpointId;
        void* slot13_GetVolumeGroupsForEndpoint;
        void* slot14_GetCurrentVolumeContext;
        void* slot15_SetVolumeGroupMuteForId;
        void* slot16_GetVolumeGroupMuteForId;
        void* slot17_SetRingerVibrateState;
        void* slot18_GetRingerVibrateState;
        void* slot19_SetPreferredChatApplication;
        void* slot20_ResetPreferredChatApplication;
        void* slot21_GetPreferredChatApplication;
        void* slot22_GetCurrentChatApplications;
        void* slot23_add_ChatContextChanged;
        void* slot24_remove_ChatContextChanged;
        // Target methods (25..27)
        HRESULT(STDMETHODCALLTYPE* SetPersistedDefaultAudioEndpoint)(
            IAudioPolicyConfigFactoryRaw*, UINT32 processId, EDataFlow flow, ERole role, HSTRING_OPAQUE deviceId);
        HRESULT(STDMETHODCALLTYPE* GetPersistedDefaultAudioEndpoint)(
            IAudioPolicyConfigFactoryRaw*, UINT32 processId, EDataFlow flow, ERole role, HSTRING_OPAQUE* deviceId);
        HRESULT(STDMETHODCALLTYPE* ClearAllPersistedApplicationDefaultEndpoints)(
            IAudioPolicyConfigFactoryRaw*);
    };
    Vtbl* lpVtbl;
};

static const IID kIidAudioPolicyWin11 = {
    0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}
};
static const IID kIidAudioPolicyWin10 = {
    0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}
};

struct CombaseApi {
    HMODULE mod = nullptr;
    PfnRoGetActivationFactory ro_get_factory = nullptr;
    PfnWindowsCreateString create_string = nullptr;
    PfnWindowsDeleteString delete_string = nullptr;
    PfnWindowsGetStringRawBuffer get_raw_buffer = nullptr;

    bool load() {
        mod = LoadLibraryW(L"combase.dll");
        if (!mod) return false;
        ro_get_factory = reinterpret_cast<PfnRoGetActivationFactory>(GetProcAddress(mod, "RoGetActivationFactory"));
        create_string = reinterpret_cast<PfnWindowsCreateString>(GetProcAddress(mod, "WindowsCreateString"));
        delete_string = reinterpret_cast<PfnWindowsDeleteString>(GetProcAddress(mod, "WindowsDeleteString"));
        get_raw_buffer = reinterpret_cast<PfnWindowsGetStringRawBuffer>(GetProcAddress(mod, "WindowsGetStringRawBuffer"));
        return ro_get_factory && create_string && delete_string && get_raw_buffer;
    }

    ~CombaseApi() {
        if (mod) FreeLibrary(mod);
    }

    IAudioPolicyConfigFactoryRaw* acquire_factory() const {
        if (!ro_get_factory || !create_string || !delete_string) return nullptr;
        static constexpr wchar_t kClassName[] = L"Windows.Media.Internal.AudioPolicyConfig";
        HSTRING_OPAQUE hclass = nullptr;
        if (FAILED(create_string(kClassName, static_cast<UINT32>(wcslen(kClassName)), &hclass)) || !hclass) {
            return nullptr;
        }
        IAudioPolicyConfigFactoryRaw* factory = nullptr;
        HRESULT hr = ro_get_factory(hclass, kIidAudioPolicyWin11, reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            hr = ro_get_factory(hclass, kIidAudioPolicyWin10, reinterpret_cast<void**>(&factory));
        }
        delete_string(hclass);
        return SUCCEEDED(hr) ? factory : nullptr;
    }
};

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring to_lower_copy(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

std::wstring format_policy_token(const std::wstring& short_id) {
    return L"\\\\?\\SWD#MMDEVAPI#" + short_id + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

std::filesystem::path recovery_file_path() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::filesystem::path base = (len > 0 && len < MAX_PATH) ? std::filesystem::path(buf) : std::filesystem::temp_directory_path();
    return base / L"SoundFlow" / L"per_app_route_recovery.txt";
}

uint64_t query_process_creation_time(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME created{}, exit_t{}, kernel_t{}, user_t{};
    uint64_t res = 0;
    if (GetProcessTimes(h, &created, &exit_t, &kernel_t, &user_t)) {
        res = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(h);
    return res;
}

std::vector<uint32_t> collect_process_tree_pids(uint32_t root_pid) {
    std::vector<uint32_t> result;
    if (root_pid == 0) return result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        result.push_back(root_pid);
        return result;
    }
    std::map<uint32_t, std::vector<uint32_t>> children;
    std::set<uint32_t> alive;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            alive.insert(pe.th32ProcessID);
            children[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (!alive.count(root_pid)) return result;
    std::set<uint32_t> visited;
    std::vector<uint32_t> stack{root_pid};
    while (!stack.empty()) {
        uint32_t cur = stack.back();
        stack.pop_back();
        if (!visited.insert(cur).second) continue;
        result.push_back(cur);
        if (auto it = children.find(cur); it != children.end()) {
            for (uint32_t child : it->second) {
                if (!visited.count(child)) stack.push_back(child);
            }
        }
    }
    return result;
}

struct SecondaryEndpoint {
    std::wstring short_id;
    std::string friendly_name;
    int score = 0;
};

std::wstring read_device_friendly_name(IMMDevice* dev) {
    IPropertyStore* store = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    store->Release();
    return out;
}

bool pick_secondary_render_endpoint(SecondaryEndpoint& out_ep) {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool need_uninit = SUCCEEDED(com_hr);

    bool found = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        std::wstring default_id;
        IMMDevice* def_dev = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def_dev)) && def_dev) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def_dev->GetId(&id)) && id) {
                default_id = id;
                CoTaskMemFree(id);
            }
            def_dev->Release();
        }

        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT count = 0;
            col->GetCount(&count);
            SecondaryEndpoint best;
            best.score = -1;
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(col->Item(i, &dev)) || !dev) continue;
                LPWSTR id = nullptr;
                std::wstring sid;
                if (SUCCEEDED(dev->GetId(&id)) && id) {
                    sid = id;
                    CoTaskMemFree(id);
                }
                if (!sid.empty() && sid != default_id) {
                    const std::wstring wname = read_device_friendly_name(dev);
                    const std::wstring lower = to_lower_copy(wname);
                    int score = 10;
                    if (lower.find(L"vb-audio") != std::wstring::npos ||
                        lower.find(L"cable") != std::wstring::npos ||
                        lower.find(L"virtual") != std::wstring::npos) {
                        score = 100;
                    } else if (lower.find(L"digital") != std::wstring::npos ||
                               lower.find(L"spdif") != std::wstring::npos ||
                               lower.find(L"s/pdif") != std::wstring::npos ||
                               lower.find(L"optical") != std::wstring::npos) {
                        score = 60;
                    }
                    if (score > best.score) {
                        best.short_id = sid;
                        best.friendly_name = wide_to_utf8(wname);
                        best.score = score;
                        found = true;
                    }
                }
                dev->Release();
            }
            col->Release();
            if (found) out_ep = std::move(best);
        }
        enumerator->Release();
    }

    if (need_uninit) CoUninitialize();
    return found;
}

bool set_pid_endpoint(CombaseApi& api, IAudioPolicyConfigFactoryRaw* factory,
                      uint32_t pid, const std::wstring& full_token) {
    HSTRING_OPAQUE hstr = nullptr;
    if (!full_token.empty()) {
        if (FAILED(api.create_string(full_token.c_str(), static_cast<UINT32>(full_token.size()), &hstr))) {
            return false;
        }
    }
    const HRESULT hr1 = factory->lpVtbl->SetPersistedDefaultAudioEndpoint(factory, pid, eRender, eMultimedia, hstr);
    const HRESULT hr2 = factory->lpVtbl->SetPersistedDefaultAudioEndpoint(factory, pid, eRender, eConsole, hstr);
    if (hstr) api.delete_string(hstr);
    return SUCCEEDED(hr1) || SUCCEEDED(hr2);
}

std::wstring get_pid_endpoint(CombaseApi& api, IAudioPolicyConfigFactoryRaw* factory, uint32_t pid) {
    HSTRING_OPAQUE hstr = nullptr;
    if (FAILED(factory->lpVtbl->GetPersistedDefaultAudioEndpoint(factory, pid, eRender, eMultimedia, &hstr)) || !hstr) {
        return {};
    }
    UINT32 len = 0;
    const wchar_t* raw = api.get_raw_buffer(hstr, &len);
    std::wstring out = (raw && len > 0) ? std::wstring(raw, len) : std::wstring{};
    api.delete_string(hstr);
    return out;
}

void write_recovery_file(const std::vector<RecoveryRouteEntry>& entries) {
    const auto path = recovery_file_path();
    std::error_code ec;
    if (entries.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (ofs) {
        const auto text = PerAppSilentRouter::serialize_recovery_entries(entries);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace

struct PerAppSilentRouter::Impl {
    mutable std::mutex mu;
    SilentRouteStatus st{};
    uint32_t root_pid = 0;
    std::wstring target_short_id;
    std::wstring target_full_token;
    std::map<uint32_t, RecoveryRouteEntry> routed;
    std::atomic<bool> running{false};
    std::thread watcher_thread;
    std::thread keepalive_thread;

    void start_keepalive(const std::wstring& short_id) {
        keepalive_thread = std::thread([this, short_id]() {
            const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool need_uninit = SUCCEEDED(com_hr);

            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* client = nullptr;
            IAudioRenderClient* render = nullptr;
            WAVEFORMATEX* mix_fmt = nullptr;

            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
                if (SUCCEEDED(enumerator->GetDevice(short_id.c_str(), &device)) && device) {
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                   reinterpret_cast<void**>(&client))) && client) {
                        if (SUCCEEDED(client->GetMixFormat(&mix_fmt)) && mix_fmt) {
                            if (SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix_fmt, nullptr))) {
                                UINT32 buf_frames = 0;
                                if (SUCCEEDED(client->GetBufferSize(&buf_frames)) && buf_frames > 0 &&
                                    SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) && render) {
                                    BYTE* data = nullptr;
                                    if (SUCCEEDED(render->GetBuffer(buf_frames, &data))) {
                                        render->ReleaseBuffer(buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                                    }
                                    if (SUCCEEDED(client->Start())) {
                                        while (running.load()) {
                                            Sleep(40);
                                            UINT32 pad = 0;
                                            if (FAILED(client->GetCurrentPadding(&pad))) break;
                                            UINT32 avail = (buf_frames > pad) ? (buf_frames - pad) : 0;
                                            if (avail > 0 && SUCCEEDED(render->GetBuffer(avail, &data))) {
                                                render->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
                                            }
                                        }
                                        client->Stop();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (mix_fmt) CoTaskMemFree(mix_fmt);
            if (render) render->Release();
            if (client) client->Release();
            if (device) device->Release();
            if (enumerator) enumerator->Release();
            if (need_uninit) CoUninitialize();
        });
    }

    void sync_children_once() {
        const auto pids = collect_process_tree_pids(root_pid);
        if (pids.empty()) return;

        std::vector<uint32_t> newly_found;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (uint32_t pid : pids) {
                if (routed.find(pid) == routed.end()) newly_found.push_back(pid);
            }
        }
        if (newly_found.empty()) return;

        const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool need_uninit = SUCCEEDED(com_hr);
        CombaseApi api;
        if (api.load()) {
            if (auto* factory = api.acquire_factory()) {
                std::vector<RecoveryRouteEntry> snapshot;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    for (uint32_t pid : newly_found) {
                        std::wstring prior = get_pid_endpoint(api, factory, pid);
                        if (set_pid_endpoint(api, factory, pid, target_full_token)) {
                            RecoveryRouteEntry entry;
                            entry.pid = pid;
                            entry.creation_time = query_process_creation_time(pid);
                            entry.prior_token = wide_to_utf8(prior);
                            routed[pid] = std::move(entry);
                        }
                    }
                    for (const auto& [_, e] : routed) snapshot.push_back(e);
                }
                write_recovery_file(snapshot);
                factory->lpVtbl->Release(factory);
            }
        }
        if (need_uninit) CoUninitialize();
    }

    void restore_all() {
        running.store(false);
        if (watcher_thread.joinable()) watcher_thread.join();
        if (keepalive_thread.joinable()) keepalive_thread.join();

        std::map<uint32_t, RecoveryRouteEntry> to_restore;
        {
            std::lock_guard<std::mutex> lk(mu);
            to_restore.swap(routed);
            root_pid = 0;
            target_short_id.clear();
            target_full_token.clear();
            st.active = false;
            st.endpoint_name.clear();
        }
        if (!to_restore.empty()) {
            const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool need_uninit = SUCCEEDED(com_hr);
            CombaseApi api;
            if (api.load()) {
                if (auto* factory = api.acquire_factory()) {
                    for (const auto& [pid, entry] : to_restore) {
                        const uint64_t now_created = query_process_creation_time(pid);
                        if (now_created != 0 && (entry.creation_time == 0 || now_created == entry.creation_time)) {
                            set_pid_endpoint(api, factory, pid, utf8_to_wide(entry.prior_token));
                        }
                    }
                    factory->lpVtbl->Release(factory);
                }
            }
            if (need_uninit) CoUninitialize();
        }
        write_recovery_file({});
    }
};

PerAppSilentRouter::~PerAppSilentRouter() {
    restore();
    delete impl_;
    impl_ = nullptr;
}

void PerAppSilentRouter::recover_stale_routes() {
    const auto path = recovery_file_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    ifs.close();

    const auto entries = parse_recovery_entries(ss.str());
    if (!entries.empty()) {
        const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool need_uninit = SUCCEEDED(com_hr);
        CombaseApi api;
        if (api.load()) {
            if (auto* factory = api.acquire_factory()) {
                for (const auto& entry : entries) {
                    const uint64_t current_created = query_process_creation_time(entry.pid);
                    if (current_created != 0 &&
                        (entry.creation_time == 0 || current_created == entry.creation_time)) {
                        set_pid_endpoint(api, factory, entry.pid, utf8_to_wide(entry.prior_token));
                    }
                }
                factory->lpVtbl->Release(factory);
            }
        }
        if (need_uninit) CoUninitialize();
    }
    std::filesystem::remove(path, ec);
}

bool PerAppSilentRouter::has_secondary_render_endpoint() {
    SecondaryEndpoint ep;
    return pick_secondary_render_endpoint(ep);
}

bool PerAppSilentRouter::apply_for_process_tree(uint32_t root_pid, SilentRouteStatus* out_status) {
    if (!impl_) impl_ = new Impl();
    if (impl_->running.load() && impl_->root_pid == root_pid) {
        if (out_status) *out_status = status();
        return true;
    }
    restore();

    SilentRouteStatus st;
    SecondaryEndpoint ep;
    if (!pick_secondary_render_endpoint(ep)) {
        st.supported = false;
        st.active = false;
        st.hint = "未检测到空闲副输出口，可接入 HDMI 显示器音频或安装 VB-Cable 开启本机消音";
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->st = st;
        }
        if (out_status) *out_status = st;
        return false;
    }

    st.supported = true;
    st.endpoint_name = ep.friendly_name;
    impl_->root_pid = root_pid;
    impl_->target_short_id = ep.short_id;
    impl_->target_full_token = format_policy_token(ep.short_id);
    impl_->running.store(true);
    impl_->sync_children_once();

    bool routed_any = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        routed_any = !impl_->routed.empty();
    }
    if (!routed_any) {
        impl_->running.store(false);
        st.active = false;
        st.hint = "Windows 音频策略路由接口不可用，已保持同响抓取";
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->st = st;
        }
        if (out_status) *out_status = st;
        return false;
    }

    st.active = true;
    st.hint.clear();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->st = st;
    }
    impl_->start_keepalive(ep.short_id);
    impl_->watcher_thread = std::thread([impl = impl_]() {
        while (impl->running.load()) {
            for (int i = 0; i < 20 && impl->running.load(); ++i) Sleep(50);
            if (impl->running.load()) impl->sync_children_once();
        }
    });
    if (out_status) *out_status = st;
    return true;
}

void PerAppSilentRouter::restore() {
    if (impl_) impl_->restore_all();
}

SilentRouteStatus PerAppSilentRouter::status() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->st;
}

#endif

} // namespace miplay::media
