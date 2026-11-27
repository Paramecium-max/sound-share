#include "media/process_loopback.h"

#include <charconv>

#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
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
        HRESULT result = E_FAIL;
        IUnknown* object = nullptr;
        std::lock_guard lock(mutex);
        if (SUCCEEDED(operation->GetActivateResult(&result, &object)) && SUCCEEDED(result) && object) {
            object->QueryInterface(kAudioClient, reinterpret_cast<void**>(&client));
        }
        if (object) object->Release();
        done = true;
        cv.notify_one();
        return S_OK;
    }
    IAudioClient* wait() {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(2500), [this] { return done; });
        auto* result = client;
        client = nullptr;
        return result;
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

std::string process_image(std::wstring wide) {
    wide.erase(0, wide.find_last_of(L"\\/") + 1);
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

} // namespace

IAudioClient* activate_process_loopback(uint32_t pid) {
    using Activate = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*,
        IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
    // Keep the system DLL loaded while Windows owns asynchronous operations.
    static const auto module = LoadLibraryExW(L"Mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto activate = module ? reinterpret_cast<Activate>(
        GetProcAddress(module, "ActivateAudioInterfaceAsync")) : nullptr;
    if (!activate || pid == GetCurrentProcessId()) return nullptr;
    auto* handler = new Activation(pid);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    IAudioClient* client = nullptr;
    if (SUCCEEDED(activate(L"VAD\\Process_Loopback", kAudioClient, &handler->property, handler, &operation))) {
        client = handler->wait();
    }
    if (operation) operation->Release();
    handler->Release();
    return client;
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
        if (roots.insert(root).second) apps.push_back({root, process_image(path)});
    }
    std::sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) {
        return a.image < b.image;
    });
#endif
    return apps;
}

} // namespace miplay::media
