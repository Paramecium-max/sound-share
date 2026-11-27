#include "media/process_loopback.h"

#include <charconv>

#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <winver.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#endif

namespace miplay::media {

std::optional<uint32_t> process_loopback_pid(std::string_view param) {
    if (!param.starts_with("process:")) return std::nullopt;
    param.remove_prefix(8);
    uint32_t pid{};
    const auto end = param.data() + param.size();
    const auto result = std::from_chars(param.data(), end, pid);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return pid;
}

#ifdef _WIN32
namespace {

constexpr IID kUnknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
constexpr IID kAgile = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
constexpr IID kCompleted = {0x41d949ab,0x9862,0x444a,{0x80,0xf6,0xc2,0x61,0x33,0x4d,0xa5,0xeb}};
constexpr IID kAudioClient = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};

class Activation final : public IActivateAudioInterfaceCompletionHandler {
public:
    // ABI layout of AUDIOCLIENT_ACTIVATION_PARAMS (process loopback).
    struct Parameters { DWORD type; DWORD process; DWORD mode; };
    static_assert(sizeof(Parameters) == 12 && alignof(Parameters) == alignof(DWORD));
    explicit Activation(DWORD pid) : params{1, pid ? pid : GetCurrentProcessId(), pid ? 0U : 1U} {
        property.vt = VT_BLOB;
        property.blob = {sizeof(params), reinterpret_cast<BYTE*>(&params)};
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != kUnknown && iid != kCompleted && iid != kAgile) return E_NOINTERFACE;
        *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = InterlockedDecrement(&refs);
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activated = E_FAIL;
        IUnknown* object = nullptr;
        std::lock_guard lock(mutex);
        status = operation->GetActivateResult(&activated, &object);
        if (SUCCEEDED(status)) {
            status = FAILED(activated) ? activated : E_NOINTERFACE;
            if (SUCCEEDED(activated) && object) {
                status = object->QueryInterface(kAudioClient, reinterpret_cast<void**>(&client));
            }
        }
        if (object) object->Release();
        done = true;
        cv.notify_one();
        return S_OK;
    }
    HRESULT wait(IAudioClient** out) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::milliseconds(2500), [this] { return done; })) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        *out = client;
        client = nullptr;
        return status;
    }

    Parameters params;
    PROPVARIANT property{};

private:
    ~Activation() { if (client) client->Release(); }
    LONG refs = 1;
    std::mutex mutex;
    std::condition_variable cv;
    IAudioClient* client = nullptr;
    bool done = false;
    HRESULT status = E_FAIL;
};

std::wstring process_path(uint32_t pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring wide(32768, L'\0');
    DWORD size = static_cast<DWORD>(wide.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, wide.data(), &size);
    CloseHandle(process);
    if (!ok || !size) return {};
    wide.resize(size);
    return wide;
}

std::string wide_to_utf8_str(std::wstring_view wide) {
    if (wide.empty()) return {};
    const auto bytes = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

std::string process_image(std::wstring wide) {
    wide.erase(0, wide.find_last_of(L"\\/") + 1);
    return wide_to_utf8_str(wide);
}

std::string process_friendly_label(const std::wstring& full_path, const std::string& fallback_image) {
    DWORD dummy = 0;
    const DWORD info_size = GetFileVersionInfoSizeW(full_path.c_str(), &dummy);
    if (info_size > 0 && info_size <= 1024 * 1024) {
        std::vector<uint8_t> ver_buf(info_size);
        if (GetFileVersionInfoW(full_path.c_str(), 0, info_size, ver_buf.data())) {
            struct LangAndCodePage { WORD language; WORD code_page; };
            std::vector<std::wstring> prefixes;
            LangAndCodePage* trans = nullptr;
            UINT trans_bytes = 0;
            if (VerQueryValueW(ver_buf.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<LPVOID*>(&trans), &trans_bytes) &&
                trans && trans_bytes >= sizeof(LangAndCodePage)) {
                const size_t count = trans_bytes / sizeof(LangAndCodePage);
                for (size_t i = 0; i < count; ++i) {
                    wchar_t hex[32]{};
                    swprintf(hex, 32, L"%04x%04x", trans[i].language, trans[i].code_page);
                    prefixes.emplace_back(hex);
                }
            }
            for (const wchar_t* common : {L"080404b0", L"040904b0", L"040904e4", L"080403a8"}) {
                if (std::find(prefixes.begin(), prefixes.end(), common) == prefixes.end()) {
                    prefixes.emplace_back(common);
                }
            }
            for (const wchar_t* field : {L"FileDescription", L"ProductName"}) {
                for (const auto& lc : prefixes) {
                    const std::wstring sub = L"\\StringFileInfo\\" + lc + L"\\" + field;
                    LPWSTR val = nullptr;
                    UINT val_chars = 0;
                    if (VerQueryValueW(ver_buf.data(), sub.c_str(),
                                       reinterpret_cast<LPVOID*>(&val), &val_chars) &&
                        val && val_chars > 0) {
                        std::wstring_view sv(val);
                        if (const auto nul = sv.find(L'\0'); nul != std::wstring_view::npos) {
                            sv = sv.substr(0, nul);
                        }
                        while (!sv.empty() && std::iswspace(sv.front())) sv.remove_prefix(1);
                        while (!sv.empty() && std::iswspace(sv.back())) sv.remove_suffix(1);
                        if (!sv.empty()) return wide_to_utf8_str(sv);
                    }
                }
            }
        }
    }
    if (fallback_image.size() > 4) {
        const auto tail = fallback_image.substr(fallback_image.size() - 4);
        if (tail == ".exe" || tail == ".EXE") {
            return fallback_image.substr(0, fallback_image.size() - 4);
        }
    }
    return fallback_image;
}

} // namespace

bool is_legacy_process_loopback_windows() {
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto get_version = reinterpret_cast<GetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return get_version && get_version(&version) == 0 &&
           (version.dwMajorVersion < 10 ||
            (version.dwMajorVersion == 10 && version.dwBuildNumber < 20348));
}

HRESULT activate_process_loopback(uint32_t pid, IAudioClient** client) {
    using Activate = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*,
        IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
    // Keep the system DLL loaded while Windows owns asynchronous operations.
    static const auto module = LoadLibraryExW(L"Mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto activate = module ? reinterpret_cast<Activate>(
        GetProcAddress(module, "ActivateAudioInterfaceAsync")) : nullptr;
    if (!client) return E_POINTER;
    *client = nullptr;
    if (pid == GetCurrentProcessId()) return E_INVALIDARG;
    if (!activate) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    auto* handler = new Activation(pid);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT status = activate(L"VAD\\Process_Loopback", kAudioClient, &handler->property, handler, &operation);
    if (SUCCEEDED(status)) status = handler->wait(client);
    if (operation) operation->Release();
    handler->Release();
    return status;
}
#endif

std::vector<AudioApp> list_audio_apps() {
    std::vector<AudioApp> apps;
#ifdef _WIN32
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return apps;
    constexpr CLSID enumerator_id = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
    constexpr IID device_id = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
    constexpr IID manager_id = {0x77aa99a0,0x1bd6,0x484f,{0x8b,0xc7,0x2c,0x65,0x4c,0x9a,0x9b,0x6f}};
    constexpr IID control_id = {0xbfb7ff88,0x7239,0x4fc9,{0x8f,0xa2,0x07,0xc9,0x50,0xbe,0x9c,0x6d}};
    IMMDeviceEnumerator* devices = nullptr;
    IMMDeviceCollection* outputs = nullptr;
    std::set<DWORD> pids;
    if (SUCCEEDED(CoCreateInstance(enumerator_id, nullptr, CLSCTX_ALL, device_id, reinterpret_cast<void**>(&devices))) &&
        SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs))) {
        UINT count = 0;
        outputs->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* output = nullptr;
            if (FAILED(outputs->Item(i, &output))) continue;
            IAudioSessionManager2* manager = nullptr;
            if (SUCCEEDED(output->Activate(manager_id, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager)))) {
                IAudioSessionEnumerator* sessions = nullptr;
                if (SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                    int length = 0;
                    sessions->GetCount(&length);
                    for (int j = 0; j < length; ++j) {
                        IAudioSessionControl* control = nullptr;
                        if (FAILED(sessions->GetSession(j, &control))) continue;
                        IAudioSessionControl2* details = nullptr;
                        AudioSessionState state = AudioSessionStateExpired;
                        control->GetState(&state);
                        if (state != AudioSessionStateExpired &&
                            SUCCEEDED(control->QueryInterface(control_id, reinterpret_cast<void**>(&details)))) {
                            DWORD pid = 0;
                            if (details->IsSystemSoundsSession() == S_FALSE && details->GetProcessId(&pid) == S_OK &&
                                pid && pid != GetCurrentProcessId()) pids.insert(pid);
                            details->Release();
                        }
                        control->Release();
                    }
                    sessions->Release();
                }
                manager->Release();
            }
            output->Release();
        }
    }
    if (outputs) outputs->Release();
    if (devices) devices->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    // Chromium often publishes the audio session from its audio utility process.
    // Promote it only through ancestors running the same executable.
    std::map<DWORD, DWORD> parents;
    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(processes, &entry)) {
            do { parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID); }
            while (Process32NextW(processes, &entry));
        }
        CloseHandle(processes);
    }
    std::set<DWORD> roots;
    for (const auto pid : pids) {
        const auto path = process_path(pid);
        if (path.empty()) continue;
        DWORD root = pid;
        for (unsigned depth = 0; depth < 16 && parents.contains(root); ++depth) {
            const auto parent = parents.at(root);
            if (!parent || parent == root || _wcsicmp(path.c_str(), process_path(parent).c_str()) != 0) break;
            root = parent;
        }
        if (roots.insert(root).second) {
            const auto root_path = (root == pid) ? path : process_path(root);
            const auto& effective_path = root_path.empty() ? path : root_path;
            auto img = process_image(effective_path);
            auto lbl = process_friendly_label(effective_path, img);
            apps.push_back({root, std::move(img), std::move(lbl)});
        }
    }
    std::sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) {
        return a.label < b.label;
    });
#endif
    return apps;
}

} // namespace miplay::media
