#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"
#include "sidecar/ws_server.h"

#include <atomic>
#include <charconv>
#include <stdexcept>
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

struct RpcError { std::string message; };

const Value* field(const Value::Object& object, std::string_view key) {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

template<class T>
T parameter(const Value::Object& object, const char* key, T fallback) {
    if (const auto* value = field(object, key)) {
        if (const auto* typed = std::get_if<T>(&value->data)) return *typed;
        throw std::invalid_argument(std::string("无效参数 ") + key);
    }
    return fallback;
}

int integer(const Value::Object& object, const char* key, int fallback, int min, int max) {
    auto number = parameter(object, key, miplay::json::Number{std::to_string(fallback)});
    int parsed;
    const auto end = number.text.data() + number.text.size();
    const auto result = std::from_chars(number.text.data(), end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < min || parsed > max) {
        throw std::invalid_argument(std::string("参数超出范围或不是整数 ") + key);
    }
    return parsed;
}

std::string ipv4(const Value::Object& object, bool allow_auto) {
    auto ip = parameter(object, "ip", std::string{});
    if (allow_auto && ip.empty() && field(object, "ip")) return ip;
    if (ip.find('\0') != std::string::npos || miplay::net::ipv4_to_u32_host_order(ip) == 0) {
        throw std::invalid_argument("无效的 IPv4 地址");
    }
    return ip;
}

Value list_interfaces() {
    Value::Array result;
    for (const auto& iface : miplay::net::list_ipv4_interfaces()) {
        result.emplace_back(Value::Object{
            {"name", iface.name}, {"ipv4", iface.ipv4}, {"isLoopback", iface.is_loopback},
        });
    }
    return result;
}

Value call(miplay::MultiRoomEngine& engine, const std::string& method, const Value::Object& params) {
    auto text = [&](const char* key, std::string fallback = {}) {
        return parameter(params, key, std::move(fallback));
    };
    auto device = [&] {
        auto id = text("deviceId");
        if (id.empty()) throw std::invalid_argument("缺少 deviceId");
        return id;
    };
    if (method == "state.get") return engine.snapshot_state_value();
    if (method == "net.listInterfaces") return list_interfaces();
    if (method == "net.setBindIp") {
        engine.set_bind_ip(ipv4(params, true));
        return engine.snapshot_state_value();
    }
    if (method == "discovery.start") return engine.start_discovery();
    if (method == "discovery.stop") {
        engine.stop_discovery();
    } else if (method == "discovery.refresh") {
        engine.refresh_discovery(parameter(params, "automatic", false));
    } else if (method == "device.addManual") {
        return engine.add_manual_device(text("name"), ipv4(params, false),
                                        static_cast<uint16_t>(integer(params, "cmdPort", 8899, 1, 65535)));
    } else if (method == "device.remove") {
        return engine.remove_device(device());
    } else if (method == "multiroom.start") {
        const auto codec = text("codec", "AAC_LC");
        if (codec != "LPCM" && codec != "AAC_LC") throw std::invalid_argument("无效的音频编码");
        const auto rate = integer(params, "sampleRate", 48000, 1, 192000);
        const auto delay = integer(params, "basePlayDelayMs", 350, 100, 1000);
        if (!engine.start_multiroom(codec == "LPCM", rate, delay)) {
            auto error = engine.last_error();
            throw RpcError{error.empty() ? "启动全屋播放失败" : error};
        }
    } else if (method == "multiroom.stop") {
        engine.stop_multiroom();
    } else if (method == "multiroom.addDevice") {
        return engine.add_device_to_group(device());
    } else if (method == "multiroom.removeDevice") {
        return engine.remove_device_from_group(device());
    } else if (method == "sync.setBaseDelay") {
        engine.set_base_play_delay_ms(integer(params, "delayMs", 350, 100, 1000));
    } else if (method == "sync.setDeviceBias") {
        engine.set_device_delay_bias(device(), integer(params, "delayBiasMs", 0, -300, 300));
    } else if (method == "volume.setDevice") {
        engine.set_device_volume(device(), integer(params, "volume", 65, 0, 100));
    } else if (method == "volume.setMaster") {
        engine.set_master_volume(integer(params, "volume", 70, 0, 100));
    } else if (method == "source.listLoopback") {
        Value::Array result;
        for (const auto& source : miplay::MultiRoomEngine::list_loopback_sources()) {
            result.emplace_back(Value::Object{{"id", source.id}, {"label", source.label}});
        }
        return result;
    } else if (method == "source.setMode") {
        auto mode = static_cast<miplay::media::AudioSourceMode>(integer(params, "mode", 0, 0, 3));
        std::string error;
        if (!engine.set_source_mode(mode, text("param"), &error)) {
            throw RpcError{error.empty() ? "切换音源失败" : error};
        }
    } else if (method == "media.setMetadata") {
        miplay::cmd::MediaInfoPayload info;
        info.title = text("title");
        info.artist = text("artist");
        info.album = text("album");
        info.duration_ms = static_cast<uint64_t>(integer(params, "durationMs", 0, 0, 86400000));
        info.position_ms = static_cast<uint64_t>(integer(params, "positionMs", 0, 0, 86400000));
        info.lyrics = text("lyrics");
        info.lyric_line = text("lyricLine");
        auto src = text("sourceName");
        if (!src.empty()) info.source_name = std::move(src);
        engine.set_media_metadata(info, parameter(params, "broadcast", true));
    } else if (method == "media.setPosition") {
        const auto pos = static_cast<uint64_t>(integer(params, "positionMs", 0, 0, 86400000));
        engine.set_media_position_ms(pos, parameter(params, "broadcast", true));
    } else if (method == "media.loadLrcFile") {
        std::string error;
        if (!engine.load_lrc_file(text("path"), &error)) {
            throw RpcError{error.empty() ? "加载 LRC 歌词失败" : error};
        }
    } else if (method == "media.setRemoteControlHandledByHost") {
        engine.set_remote_control_handled_by_host(parameter(params, "handled", false));
    } else {
        throw RpcError{"unknown method: " + method};
    }
    return true;
}

std::string handle_request(miplay::MultiRoomEngine& engine, const std::string& text) {
    Value::Object reply{{"type", "rpc"}, {"id", Value{}}};
    try {
        const auto request = miplay::json::parse(text);
        const auto* body = std::get_if<Value::Object>(&request.data);
        if (!body) throw std::invalid_argument("请求必须为 JSON 对象");
        if (const auto* id = field(*body, "id")) {
            if (!std::holds_alternative<std::string>(id->data) &&
                !std::holds_alternative<miplay::json::Number>(id->data) &&
                !std::holds_alternative<std::nullptr_t>(id->data)) {
                throw std::invalid_argument("无效的 RPC ID");
            }
            reply["id"] = *id;
        }
        const auto method = parameter(*body, "method", std::string{});
        if (method.empty()) throw std::invalid_argument("缺少 method");
        auto params = body;
        if (const auto* nested = field(*body, "params")) {
            params = std::get_if<Value::Object>(&nested->data);
            if (!params) throw std::invalid_argument("params 必须为 JSON 对象");
        }
        reply["result"] = call(engine, method, *params);
    } catch (const RpcError& error) {
        reply["error"] = error.message;
    } catch (const std::exception& error) {
        reply["error"] = error.what();
    }
    return miplay::json::dump(reply);
}

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

    auto handler = [&engine](const std::string& request) { return handle_request(engine, request); };

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
