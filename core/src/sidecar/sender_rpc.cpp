#include "sidecar/sender_rpc.h"
#include "platform/net_platform.h"
#include "media/process_loopback.h"

#include <charconv>
#include <stdexcept>

namespace miplay::sidecar {
namespace {
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
    int parsed = fallback;
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

Value call(miplay::MultiRoomEngine& engine, const std::string& method, const Value::Object& params, bool reject_self) {
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
        auto ip = ipv4(params, false);
        auto port = integer(params, "cmdPort", 8899, 1, 65535);
        if (reject_self && miplay::net::is_local_ipv4(ip)) throw RpcError{"不能把本机接收台添加为发送目标"};
        return engine.add_manual_device(text("name"), ip,
                                        static_cast<uint16_t>(port));
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
    } else if (method == "app.listAudioApps") {
        Value::Array result;
        for (const auto& app : miplay::media::list_audio_apps()) {
            result.emplace_back(Value::Object{
                {"uid", app.pid},
                {"packageName", app.image},
                {"label", app.label.empty() ? app.image : app.label},
            });
        }
        return result;
    } else if (method == "source.setMode") {
        auto mode = static_cast<miplay::media::AudioSourceMode>(integer(params, "mode", 0, 0, 3));
        std::string error;
        if (!engine.set_source_mode(mode, text("param"), &error)) {
            throw RpcError{error.empty() ? "切换音源失败" : error};
        }
    } else if (method == "source.setMuteLocalApp") {
        engine.set_mute_local_app(parameter(params, "muteLocalApp", false));
        return engine.snapshot_state_value();
    } else if (method == "wav.seek") {
        engine.seek_wav_ms(integer(params, "positionMs", 0, 0, 86400000));
    } else if (method == "wav.pause") {
        engine.set_wav_paused(parameter(params, "paused", true));
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

} // namespace

std::string handle_request(miplay::MultiRoomEngine& engine, const std::string& text, bool reject_self) {
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
        reply["result"] = call(engine, method, *params, reject_self);
    } catch (const RpcError& error) {
        reply["error"] = error.message;
    } catch (const std::exception& error) {
        reply["error"] = error.what();
    }
    return miplay::json::dump(reply);
}

} // namespace miplay::sidecar
