#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"
#include "sidecar/ws_server.h"
#include "sidecar/sender_rpc.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>

namespace {
void disable_windows_power_throttling() {
    struct WinPowerThrottlingState {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    using PfnSetProcessInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;
    auto fn = reinterpret_cast<PfnSetProcessInformation>(
        GetProcAddress(k32, "SetProcessInformation"));
    if (!fn) return;
    // ProcessPowerThrottling = 4; bit 0 = EXECUTION_SPEED, bit 2 = IGNORE_TIMER_RESOLUTION
    // 禁用 Windows 11 后台 EcoQoS 小核降频与定时器劣化，保障后台全屋推流时钟稳定
    WinPowerThrottlingState state{1, 0x1U | 0x4U, 0U};
    fn(GetCurrentProcess(), 4, &state, sizeof(state));
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}
} // namespace
#endif

namespace {

std::atomic<bool> g_stop{false};

void signal_handler(int) {
    g_stop.store(true);
}

std::string random_token() {
    static const char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string out(32, '0');
    for (char& c : out) c = kHex[gen() % 16];
    return out;
}

using miplay::json::Value;

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    uint16_t port = 0;
    std::string token = random_token();
#ifdef _WIN32
    bool exit_on_stdin_close = false;
#endif
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--token" && i + 1 < argc) token = argv[++i];
#ifdef _WIN32
        else if (arg == "--exit-on-stdin-close") exit_on_stdin_close = true;
#endif
    }

#ifdef _WIN32
    disable_windows_power_throttling();
#endif
    miplay::media::AudioPipeline::recover_playback_route();
    miplay::MultiRoomEngine engine;
    miplay::sidecar::LocalWsServer ws;

    engine.set_event_callback([&ws](const std::string& evt) {
        ws.broadcast_text(evt);
    });

    auto handler = [&engine](const std::string& request) { return miplay::sidecar::handle_request(engine, request); };

    if (!ws.start(port, token, handler)) {
        std::cerr << "ERROR: Failed to start LocalWsServer\n";
        return 1;
    }

    // 启动默认 mDNS 发现
    engine.start_discovery();

    // 约定输出 READY 行供 Electron 主进程解析连接参数
    std::cout << "READY " << miplay::json::dump(Value::Object{{"port", ws.port()}, {"token", token}}) << std::endl;

    auto next_state = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!g_stop.load()) {
#ifdef _WIN32
        // Electron owns the other end. Closing its pipe lets WASAPI restore the
        // original playback route before this Windows process exits.
        if (exit_on_stdin_close && !PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE),
                                                 nullptr, 0, nullptr, nullptr, nullptr)) break;
#endif
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_state) {
            engine.tick_media_sync();
            ws.broadcast_text("{\"type\":\"state\",\"data\":" + engine.snapshot_state_json() + "}");
            next_state = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ws.stop();
    engine.stop_multiroom();
    engine.stop_discovery();
    return 0;
}
