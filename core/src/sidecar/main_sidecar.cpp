#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"
#include "sidecar/ws_server.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

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

// 轻量字段提取器（用于 Sidecar 简单 JSON-RPC 2.0 请求）
std::string json_get_str(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

int json_get_int(const std::string& json, const std::string& key, int def_val) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return def_val;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return def_val;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) ++pos;
    try {
        return std::stoi(json.substr(pos));
    } catch (...) {
        return def_val;
    }
}

bool json_get_bool(const std::string& json, const std::string& key, bool def_val) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return def_val;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return def_val;
    auto sub = json.substr(pos + 1, 10);
    if (sub.find("true") != std::string::npos) return true;
    if (sub.find("false") != std::string::npos) return false;
    return def_val;
}

std::string list_interfaces_json() {
    auto ifaces = miplay::net::list_ipv4_interfaces();
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (i) oss << ",";
        oss << "{\"name\":\"" << ifaces[i].name
            << "\",\"ipv4\":\"" << ifaces[i].ipv4
            << "\",\"isLoopback\":" << (ifaces[i].is_loopback ? "true" : "false") << "}";
    }
    oss << "]";
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    uint16_t port = 0;
    std::string token = random_token();
    for (int i = 1; i + 1 < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port") port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--token") token = argv[++i];
    }

    miplay::MultiRoomEngine engine;
    miplay::sidecar::LocalWsServer ws;

    engine.set_event_callback([&ws](const std::string& evt) {
        ws.broadcast_text(evt);
    });

    auto handler = [&engine](const std::string& req) -> std::string {
        std::string id = json_get_str(req, "id");
        if (id.empty()) id = std::to_string(json_get_int(req, "id", 0));
        std::string method = json_get_str(req, "method");

        std::string result = "true";
        if (method == "state.get") {
            result = engine.snapshot_state_json();
        } else if (method == "net.listInterfaces") {
            result = list_interfaces_json();
        } else if (method == "net.setBindIp") {
            engine.set_bind_ip(json_get_str(req, "ip"));
            result = engine.snapshot_state_json();
        } else if (method == "discovery.start") {
            result = engine.start_discovery() ? "true" : "false";
        } else if (method == "discovery.stop") {
            engine.stop_discovery();
        } else if (method == "discovery.refresh") {
            engine.refresh_discovery();
        } else if (method == "device.addManual") {
            std::string dev_id = engine.add_manual_device(
                json_get_str(req, "name"),
                json_get_str(req, "ip"),
                static_cast<uint16_t>(json_get_int(req, "cmdPort", 8899)));
            result = "\"" + dev_id + "\"";
        } else if (method == "device.remove") {
            result = engine.remove_device(json_get_str(req, "deviceId")) ? "true" : "false";
        } else if (method == "multiroom.start") {
            bool lpcm = (json_get_str(req, "codec") == "LPCM");
            uint32_t sr = static_cast<uint32_t>(json_get_int(req, "sampleRate", 48000));
            uint32_t delay = static_cast<uint32_t>(json_get_int(req, "basePlayDelayMs", 350));
            result = engine.start_multiroom(lpcm, sr, delay) ? "true" : "false";
        } else if (method == "multiroom.stop") {
            engine.stop_multiroom();
        } else if (method == "multiroom.addDevice") {
            result = engine.add_device_to_group(json_get_str(req, "deviceId")) ? "true" : "false";
        } else if (method == "multiroom.removeDevice") {
            result = engine.remove_device_from_group(json_get_str(req, "deviceId")) ? "true" : "false";
        } else if (method == "sync.setBaseDelay") {
            engine.set_base_play_delay_ms(static_cast<uint32_t>(json_get_int(req, "delayMs", 350)));
        } else if (method == "sync.setDeviceBias") {
            engine.set_device_delay_bias(json_get_str(req, "deviceId"),
                                         json_get_int(req, "delayBiasMs", 0));
        } else if (method == "volume.setDevice") {
            engine.set_device_volume(json_get_str(req, "deviceId"),
                                     json_get_int(req, "volume", 65));
        } else if (method == "volume.setMaster") {
            engine.set_master_volume(json_get_int(req, "volume", 70));
        } else if (method == "source.setMode") {
            int mode = json_get_int(req, "mode", 0);
            engine.set_source_mode(static_cast<miplay::media::AudioSourceMode>(mode),
                                   json_get_str(req, "param"));
        } else {
            return "{\"type\":\"rpc\",\"id\":\"" + id + "\",\"error\":\"unknown method: " + method + "\"}";
        }
        return "{\"type\":\"rpc\",\"id\":\"" + id + "\",\"result\":" + result + "}";
    };

    if (!ws.start(port, token, handler)) {
        std::cerr << "ERROR: Failed to start LocalWsServer\n";
        return 1;
    }

    // 启动默认 mDNS 发现
    engine.start_discovery();

    // 约定输出 READY 行供 Electron 主进程解析连接参数
    std::cout << "READY {\"port\":" << ws.port() << ",\"token\":\"" << token << "\"}" << std::endl;

    // 每 1 秒推送一次全屋遥测状态
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ws.broadcast_text("{\"type\":\"state\",\"data\":" + engine.snapshot_state_json() + "}");
    }

    engine.stop_multiroom();
    engine.stop_discovery();
    ws.stop();
    return 0;
}
