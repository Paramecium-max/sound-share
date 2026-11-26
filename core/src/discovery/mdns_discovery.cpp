#include "discovery/mdns_discovery.h"
#include "discovery/probe_burst.h"
#include "sync/master_clock.h"
#include "util/utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <set>
#include <optional>
#include <utility>

#ifndef _WIN32
  #include <poll.h>
#endif

namespace miplay::discovery {

namespace {

constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr uint16_t kMiPlayMdnsPort = 5355;
constexpr uint16_t kStdMdnsPort = 5353;

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool is_valid_utf8_printable_name(const uint8_t* p, size_t n) {
    if (!p || n == 0 || n > 128) return false;
    const std::string_view name(reinterpret_cast<const char*>(p), n);
    return utf8::valid(name) && name.find_first_not_of(' ') != std::string_view::npos &&
           std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

// 解析小米 _lyra-mdns TXT AppData (Base64) 中的二进制 TLV 链：
// 实机抓包结构: [7B 或 9B 头部含版本/标志/4B实例ID] + 完整 TLV 链 (1B Type + 1B Len + Val)，
// 其中 T=0x02 固定为 UTF-8 设备真实名称（例如 "SoundFlow-Pad" / "帅小白的REDMI K90 Pro Max"）。
std::string parse_lyra_appdata_device_name(const std::vector<uint8_t>& raw) {
    if (raw.size() < 10) return "";
    // 优先测试标准头部偏移 7 (v0) 与 9 (v2)，其次回退探测 5..16
    static constexpr size_t kOffsets[] = {7, 9, 8, 6, 10, 11, 12, 5, 13, 14, 15, 16};
    std::string fallback_name;
    for (size_t start : kOffsets) {
        if (start + 3 > raw.size()) continue;
        size_t pos = start;
        bool chain_ok = true;
        bool has_anchor_tag = false;
        std::string found_name;
        while (pos < raw.size()) {
            if (pos + 2 > raw.size()) {
                chain_ok = false;
                break;
            }
            const uint8_t tag = raw[pos];
            const uint8_t len = raw[pos + 1];
            pos += 2;
            if (pos + len > raw.size()) {
                chain_ok = false;
                break;
            }
            if (tag == 0x00 || tag == 0x0A) has_anchor_tag = true;
            if (tag == 0x02 && len >= 2 && is_valid_utf8_printable_name(raw.data() + pos, len)) {
                found_name.assign(reinterpret_cast<const char*>(raw.data() + pos), len);
            }
            pos += len;
        }
        if (chain_ok && pos == raw.size() && !found_name.empty()) {
            if (has_anchor_tag) return found_name;
            if (fallback_name.empty()) fallback_name = found_name;
        }
    }
    return fallback_name;
}

bool is_benchmark_or_tun_ipv4(const std::string& ip) {
    // 198.18.0.0/15 (198.18.x.x - 198.19.x.x)：RFC 2544，常见于 Clash/代理 TUN 虚拟网卡
    return ip.rfind("198.18.", 0) == 0 || ip.rfind("198.19.", 0) == 0;
}

bool is_usable_device_ipv4(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0" || ip == "255.255.255.255") return false;
    if (ip.rfind("127.", 0) == 0) return false;
    if (ip.rfind("169.254.", 0) == 0) return false;
    if (is_benchmark_or_tun_ipv4(ip)) return false;
    const auto addr = net::ipv4_to_u32_host_order(ip);
    return addr != 0 && (addr >> 24) < 224;
}

bool is_hex_id_name(const std::string& name) {
    if (name.size() != 8) return false;
    for (unsigned char c : name) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

std::string clean_instance_label(const std::string& raw) {
    // 如 "SoundFlow-Pad(F8TnlfFbXH)" -> "SoundFlow-Pad"
    auto lp = raw.find('(');
    if (lp != std::string::npos && lp > 0 && raw.back() == ')') {
        return raw.substr(0, lp);
    }
    return raw;
}

bool is_generic_name(const std::string& name, const std::string& ip) {
    if (name.empty()) return true;
    if (name == "MiPlay-" + ip) return true;
    if (name[0] == '_') return true;
    if (is_hex_id_name(name)) return true;
    return false;
}

bool apps_list_contains_miplay(const std::string& apps_val) {
    // 例: "[5]" 或 "[1,5,8]" 或 "5"
    size_t i = 0;
    while (i < apps_val.size()) {
        while (i < apps_val.size() && (apps_val[i] < '0' || apps_val[i] > '9')) ++i;
        if (i >= apps_val.size()) break;
        size_t start = i;
        while (i < apps_val.size() && apps_val[i] >= '0' && apps_val[i] <= '9') ++i;
        if (apps_val.compare(start, i - start, "5") == 0) return true;
    }
    return false;
}

std::vector<uint8_t> base64_decode_simple(const std::string& in) {
    static constexpr int8_t kTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    const size_t actual = in.find('=');
    const size_t size = actual == std::string::npos ? in.size() : actual;
    if (size % 4 == 1 || (actual != std::string::npos &&
        (in.size() % 4 != 0 || in.size() - size > 2 || in.find_first_not_of('=', size) != std::string::npos))) {
        return {};
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < size; ++i) {
        const int8_t value = kTable[static_cast<uint8_t>(in[i])];
        if (value < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    if ((acc & ((1u << bits) - 1)) != 0) return {};
    return out;
}

using Labels = std::vector<std::string>;

std::optional<Labels> decode_name(const uint8_t* data, size_t len, size_t& offset, size_t end) {
    Labels labels;
    size_t pos = offset;
    size_t expanded = 1;
    bool jumped = false;
    unsigned pointers = 0;
    while (pos < end) {
        const uint8_t byte = data[pos++];
        if (byte == 0) {
            if (!jumped) offset = pos;
            return labels;
        }
        if ((byte & 0xC0) == 0xC0) {
            if (pos >= end || ++pointers > 16) return std::nullopt;
            const size_t pointer = ((byte & 0x3F) << 8) | data[pos++];
            if (pointer < 12 || pointer >= pos - 2) return std::nullopt;
            if (!jumped) offset = pos;
            jumped = true;
            end = len;
            pos = pointer;
            continue;
        }
        if (byte > 63 || byte > end - pos || (expanded += byte + 1) > 255) return std::nullopt;
        labels.emplace_back(reinterpret_cast<const char*>(data + pos), byte);
        pos += byte;
    }
    return std::nullopt;
}

enum class ServiceType { None, Lan, Connect, Lyra };

ServiceType service_type(const Labels& name, size_t start) {
    if (name.size() != start + 3 || to_lower_ascii(name[start + 2]) != "local") return ServiceType::None;
    const auto label = to_lower_ascii(name[start]);
    const auto proto = to_lower_ascii(name[start + 1]);
    if (label == "_miplay_lan" && proto == "_tcp") return ServiceType::Lan;
    if (label == "_mi-connect" && proto == "_udp") return ServiceType::Connect;
    if (label == "_lyra-mdns" && proto == "_udp") return ServiceType::Lyra;
    return ServiceType::None;
}

std::string name_key(const Labels& name) {
    std::string key;
    for (const auto& label : name) {
        key.push_back(static_cast<char>(label.size()));
        key += to_lower_ascii(label);
    }
    return key;
}

uint16_t read16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

std::string mac_key(std::string_view mac) {
    std::string key;
    for (unsigned char c : mac) {
        if (c == ':' || c == '-') continue;
        if (c >= 'A' && c <= 'F') c = static_cast<unsigned char>(c + 'a' - 'A');
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return {};
        key.push_back(static_cast<char>(c));
    }
    if (key.size() != 12 || key == "000000000000" || key == "ffffffffffff") return {};
    return key;
}

std::string hash_key(const std::string& hash) {
    // Short advertised fragments are useful for display, but can't identify a migrating device.
    if (hash.size() < 8 || hash.find_first_not_of(
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/-_=") != std::string::npos) return {};
    return "hash:" + hash;
}

} // namespace

std::vector<uint8_t> MdnsDiscovery::build_ptr_query_packet(const std::string& domain, bool request_unicast) {
    std::vector<uint8_t> pkt(12, 0);
    pkt[5] = 1; // QDCOUNT = 1
    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0 && len < 64) {
            pkt.push_back(static_cast<uint8_t>(len));
            pkt.insert(pkt.end(), domain.begin() + static_cast<ptrdiff_t>(start),
                       domain.begin() + static_cast<ptrdiff_t>(dot));
        }
        start = dot + 1;
    }
    pkt.push_back(0);    // root label
    pkt.push_back(0x00); // QTYPE = PTR (12)
    pkt.push_back(0x0C);
    // RFC 6762 §5.4: bit 15 = QU (unicast response requested), bit 0..14 = IN (1)
    pkt.push_back(request_unicast ? 0x80 : 0x00);
    pkt.push_back(0x01);
    return pkt;
}

std::vector<ServiceAnnouncement> MdnsDiscovery::parse_announcements(
    const uint8_t* data, size_t len, const std::string& src_ip) {
    if (!data || len < 12 || (data[2] & 0x80) == 0) return {};
    size_t offset = 12;
    for (size_t i = 0; i < read16(data + 4); ++i) {
        if (!decode_name(data, len, offset, len) || len - offset < 4) return {};
        offset += 4;
    }

    struct Instance {
        std::string label;
        ServiceType type = ServiceType::None;
        std::string host;
        uint16_t srv_port = 0;
        std::map<std::string, std::string> txt;
        uint32_t ttl = UINT32_MAX;
        bool live = false;
        bool goodbye = false;
        bool invalid = false;
    };
    std::map<std::string, Instance> instances;
    std::multimap<std::string, std::pair<std::string, uint32_t>> addresses;
    auto instance_for = [&](const Labels& labels, ServiceType type, uint32_t ttl) -> Instance& {
        auto& instance = instances[name_key(labels)];
        instance.type = type;
        instance.label = labels.front();
        if (ttl == 0) instance.goodbye = true;
        else { instance.live = true; instance.ttl = std::min(instance.ttl, ttl); }
        return instance;
    };

    const size_t records = read16(data + 6) + read16(data + 8) + read16(data + 10);
    for (size_t i = 0; i < records; ++i) {
        auto owner = decode_name(data, len, offset, len);
        if (!owner || len - offset < 10) return {};
        const uint16_t rrtype = read16(data + offset);
        const uint16_t rrclass = read16(data + offset + 2) & 0x7FFF;
        const uint32_t ttl = (static_cast<uint32_t>(read16(data + offset + 4)) << 16) | read16(data + offset + 6);
        const uint16_t size = read16(data + offset + 8);
        offset += 10;
        if (size > len - offset) return {};
        size_t pos = offset;
        offset += size;
        if (rrclass != 1) continue;
        if (rrtype == 12) {
            const auto type = service_type(*owner, 0);
            if (type == ServiceType::None) continue;
            auto target = decode_name(data, len, pos, offset);
            if (!target || pos != offset) return {};
            if (service_type(*target, 1) == type) instance_for(*target, type, ttl);
            continue;
        }
        if (rrtype == 1 && size == 4 && ttl) {
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, data + pos, ip, sizeof(ip));
            if (is_usable_device_ipv4(ip)) addresses.emplace(name_key(*owner), std::pair{std::string(ip), ttl});
            continue;
        }
        const auto type = service_type(*owner, 1);
        if (type == ServiceType::None || (rrtype != 33 && rrtype != 16)) continue;
        auto& instance = instance_for(*owner, type, ttl);
        if (ttl == 0) continue;
        if (rrtype == 33) {
            if (size < 7) return {};
            const auto port = read16(data + pos + 4);
            pos += 6;
            auto host = decode_name(data, len, pos, offset);
            if (!host || host->empty() || pos != offset) return {};
            const auto host_key = name_key(*host);
            if (!instance.host.empty() && (instance.host != host_key || instance.srv_port != port)) {
                instance.invalid = true;
            }
            instance.host = host_key;
            instance.srv_port = port;
        } else {
            while (pos < offset) {
                const size_t length = data[pos++];
                if (length > offset - pos) return {};
                const std::string_view kv(reinterpret_cast<const char*>(data + pos), length);
                pos += length;
                const auto eq = kv.find('=');
                if (eq == std::string_view::npos || eq == 0) continue;
                const auto key = to_lower_ascii(std::string(kv.substr(0, eq)));
                const auto value = kv.substr(eq + 1);
                if (!utf8::valid(kv)) {
                    instance.invalid = true;
                    continue;
                }
                const auto [it, added] = instance.txt.emplace(key, value);
                if (!added && it->second != value) instance.invalid = true;
            }
        }
    }

    std::vector<ServiceAnnouncement> out;
    for (auto& [key, instance] : instances) {
        if (instance.invalid) continue;
        if (instance.goodbye && !instance.live) {
            out.push_back({key, src_ip, 0, {}});
            continue;
        }
        DiscoveredDevice dev;
        dev.ip = src_ip;
        auto [first, last] = addresses.equal_range(instance.host);
        if (first != last) {
            auto address = std::min_element(first, last, [&](const auto& a, const auto& b) {
                return std::pair{a.second.first != src_ip, a.second.first} <
                       std::pair{b.second.first != src_ip, b.second.first};
            });
            dev.ip = address->second.first;
            instance.ttl = std::min(instance.ttl, address->second.second);
        }
        // MiPlay replies commonly omit SRV/A. The unicast or multicast sender is then the endpoint.
        if (!is_usable_device_ipv4(dev.ip)) continue;

        auto txt = [&](std::initializer_list<const char*> keys) -> std::string {
            for (const char* k : keys) {
                auto it = instance.txt.find(k);
                if (it != instance.txt.end()) return it->second;
            }
            return {};
        };
        dev.name = txt({"name", "dn", "devicename"});
        if (instance.type == ServiceType::Lyra) {
            auto encoded = txt({"appdata"});
            auto raw = base64_decode_simple(encoded);
            if (!encoded.empty() && raw.empty()) continue;
            auto decoded = parse_lyra_appdata_device_name(raw);
            if (!decoded.empty()) dev.name = std::move(decoded);
        }
        if (!is_generic_name(dev.name, dev.ip)) {
            dev.name_priority = instance.type == ServiceType::Lyra ? 1 : 2;
        } else {
            auto cleaned = clean_instance_label(instance.label);
            if (!is_generic_name(cleaned, dev.ip) && is_valid_utf8_printable_name(
                    reinterpret_cast<const uint8_t*>(cleaned.data()), cleaned.size())) {
                dev.name = std::move(cleaned);
                dev.name_priority = 1;
            } else {
                dev.name = "MiPlay-" + dev.ip;
            }
        }

        const auto text_port = txt({"port", "po"});
        int port = 0;
        const auto result = std::from_chars(text_port.data(), text_port.data() + text_port.size(), port);
        if (!text_port.empty() && (result.ec != std::errc{} || result.ptr != text_port.data() + text_port.size() ||
                                   port < 1 || port > 65535)) continue;
        const auto encoded_app = txt({"appsdata"});
        const auto app = base64_decode_simple(encoded_app);
        if (!encoded_app.empty() && app.empty()) continue;
        const auto app_port = app.size() >= 7 && app[0] == 0x81 ? read16(app.data() + 5) : 0;
        if (instance.type == ServiceType::Connect && instance.txt.contains("apps") &&
            !apps_list_contains_miplay(txt({"apps"})) && !app_port) continue;
        if (app_port) {
            port = app_port;
            dev.port_source = PortSource::AppsData;
        } else if (port) {
            dev.port_source = PortSource::Txt;
        } else if (instance.type == ServiceType::Lan && instance.srv_port) {
            port = instance.srv_port;
            dev.port_source = PortSource::Srv;
        }
        if (port) dev.cmd_port = static_cast<uint16_t>(port);
        dev.mac = txt({"mac", "m"});
        dev.id_hash = txt({"idhash"});
        dev.room_name = txt({"roomname"});
        dev.group_name = txt({"groupname"});
        const auto master = txt({"ismaster"});
        dev.is_master = master == "1" || master == "true";
        const auto device_type = txt({"devicetype", "type", "dev"});
        int32_t type = 0;
        const auto parsed = std::from_chars(device_type.data(), device_type.data() + device_type.size(), type);
        if (parsed.ec == std::errc{} && parsed.ptr == device_type.data() + device_type.size()) dev.device_type = type;
        dev.id = mac_key(dev.mac).empty() ? dev.ip : dev.mac;
        out.push_back({key, src_ip, instance.ttl, std::move(dev)});
    }
    return out;
}

bool MdnsDiscovery::parse_response_packet(const uint8_t* data, size_t len, const std::string& src_ip,
                                          DiscoveredDevice& out) {
    for (auto& record : parse_announcements(data, len, src_ip)) {
        if (record.ttl_seconds == 0) continue;
        out = std::move(record.device);
        out.last_seen_us = sync::monotonic_now_us();
        return true;
    }
    return false;
}

std::vector<DiscoveredDevice> DiscoveryCache::update(
    const std::vector<ServiceAnnouncement>& announcements, uint64_t now_us) {
    std::set<std::string> dirty;
    for (auto it = services_.begin(); it != services_.end();) {
        if (it->second.expiry_us <= now_us) {
            dirty.insert(it->second.device_key);
            it = services_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& entry : announcements) {
        const auto service_key = std::pair{entry.instance, entry.source_ip};
        auto prior = services_.find(service_key);
        if (!entry.ttl_seconds) {
            if (prior != services_.end()) {
                dirty.insert(prior->second.device_key);
                services_.erase(prior);
            }
            continue;
        }

        auto incoming = entry.device;
        incoming.last_seen_us = now_us;
        incoming.revision = ++revision_;
        std::string key;
        const std::string input_mac = mac_key(incoming.mac);
        const std::string prior_mac = prior == services_.end() ? "" : mac_key(prior->second.device.mac);
        if (prior != services_.end() && (input_mac.empty() || prior_mac.empty() || input_mac == prior_mac)) {
            const auto& previous = prior->second.device;
            if (incoming.mac.empty()) incoming.mac = previous.mac;
            if (incoming.id_hash.empty()) incoming.id_hash = previous.id_hash;
            if (!incoming.has_explicit_port()) {
                incoming.cmd_port = previous.cmd_port;
                incoming.port_source = previous.port_source;
            }
            if (previous.name_priority > incoming.name_priority) {
                incoming.name = previous.name;
                incoming.name_priority = previous.name_priority;
            }
            key = prior->second.device_key;
        }
        const std::string mac = mac_key(incoming.mac);
        const std::string mac_alias = mac.empty() ? "" : "mac:" + mac;
        const std::string hash_alias = hash_key(incoming.id_hash);
        if (auto it = identity_aliases_.find(mac_alias); it != identity_aliases_.end()) key = it->second;
        if (key.empty()) {
            if (auto it = identity_aliases_.find(hash_alias); it != identity_aliases_.end()) {
                const auto previous_mac = mac_key(devices_.at(it->second).mac);
                if (mac.empty() || previous_mac.empty() || previous_mac == mac) key = it->second;
            }
        }
        if (key.empty()) {
            for (const auto& [id, existing] : devices_) {
                const auto existing_mac = mac_key(existing.mac);
                const bool conflicting_mac = !existing_mac.empty() && !mac.empty() && existing_mac != mac;
                if ((!mac.empty() && existing_mac == mac) ||
                    (!conflicting_mac && existing.ip == incoming.ip &&
                     (!existing.has_explicit_port() || !incoming.has_explicit_port() || existing.cmd_port == incoming.cmd_port))) {
                    if (!key.empty()) { key.clear(); break; }
                    key = id;
                }
            }
        }
        if (key.empty()) {
            key = incoming.id;
            if (devices_.contains(key)) key += "_" + std::to_string(incoming.cmd_port);
        }
        if (!mac_alias.empty()) identity_aliases_[mac_alias] = key;
        if (!hash_alias.empty()) identity_aliases_[hash_alias] = key;
        incoming.id = key;
        if (prior != services_.end()) dirty.insert(prior->second.device_key);
        if (!mac_alias.empty() || !hash_alias.empty()) {
            for (auto it = services_.begin(); it != services_.end();) {
                if (it->first.first == entry.instance && it->first != service_key && it->second.device_key == key) {
                    it = services_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        services_[service_key] = {incoming, key, now_us + entry.ttl_seconds * 1'000'000ULL};
        devices_.try_emplace(key, incoming);
        dirty.insert(key);
    }

    std::vector<DiscoveredDevice> updates;
    for (const auto& key : dirty) {
        auto& device = devices_.at(key);
        const DiscoveredDevice* newest = nullptr;
        const DiscoveredDevice* named = nullptr;
        const DiscoveredDevice* port = nullptr;
        for (const auto& [_, service] : services_) {
            if (service.device_key != key) continue;
            const auto& current = service.device;
            if (!newest || current.revision > newest->revision) newest = &current;
            if (!named || std::pair{current.name_priority, current.revision} >
                          std::pair{named->name_priority, named->revision}) named = &current;
            if (current.has_explicit_port() && (!port || current.revision > port->revision)) port = &current;
        }
        if (newest) {
            auto previous = device;
            device = *newest;
            if (device.mac.empty()) device.mac = previous.mac;
            if (device.id_hash.empty()) device.id_hash = previous.id_hash;
            device.name_priority = named->name_priority;
            device.name = named->name_priority ? named->name : "MiPlay-" + device.ip;
            if (port) {
                device.cmd_port = port->cmd_port;
                device.port_source = port->port_source;
            }
        } else {
            device.online = false;
        }
        device.revision = ++revision_;
        updates.push_back(device);
    }
    return updates;
}

std::vector<DiscoveredDevice> DiscoveryCache::list_devices() const {
    std::vector<DiscoveredDevice> result;
    for (const auto& [_, entry] : devices_) {
        if (entry.online) result.push_back(entry);
    }
    return result;
}

namespace {
bool multicast_interface(SocketHandle fd, const std::string& ip) {
    in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1 &&
           setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&addr), sizeof(addr)) == 0;
}

bool multicast_membership(SocketHandle fd, const std::string& ip, int option) {
    ip_mreq req{};
    inet_pton(AF_INET, kMdnsGroup, &req.imr_multiaddr);
    if (inet_pton(AF_INET, ip.c_str(), &req.imr_interface) != 1) return false;
    return setsockopt(fd, IPPROTO_IP, option, reinterpret_cast<const char*>(&req), sizeof(req)) == 0;
}

SocketHandle open_mdns_socket(const std::string& ip, uint16_t port) {
    const auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) return fd;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &address.sin_addr);
    const int ttl = 255;
    if ((port && !net::set_reuse_addr(fd)) ||
        bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        !net::set_nonblocking(fd, true) ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl)) != 0 ||
        (!port && !multicast_interface(fd, ip))) {
        net::close_socket(fd);
        return kInvalidSocket;
    }
    return fd;
}

bool send_query(SocketHandle fd, const std::string& service, uint16_t port, bool qu) {
    auto packet = MdnsDiscovery::build_ptr_query_packet(service, qu);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, kMdnsGroup, &address.sin_addr);
    return sendto(fd, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&address), sizeof(address)) == static_cast<int>(packet.size());
}
} // namespace

MdnsDiscovery::~MdnsDiscovery() {
    stop();
}

bool MdnsDiscovery::start(const std::string& bind_ipv4, DeviceFoundCallback cb,
                          std::function<void()> on_interfaces_changed) {
    std::lock_guard lock(lifecycle_mu_);
    if (running_.load()) return true;
    net::init_network();
    preferred_bind_ipv4_ = bind_ipv4;
    cb_ = std::move(cb);
    on_interfaces_changed_ = std::move(on_interfaces_changed);
    {
        std::lock_guard wake_lock(wake_mu_);
        wake_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        wake_address_ = {};
        wake_address_.sin_family = AF_INET;
        wake_address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(wake_address_);
        if (wake_fd_ != kInvalidSocket &&
            (bind(wake_fd_, reinterpret_cast<sockaddr*>(&wake_address_), sizeof(wake_address_)) != 0 ||
             getsockname(wake_fd_, reinterpret_cast<sockaddr*>(&wake_address_), &size) != 0 ||
             !net::set_nonblocking(wake_fd_, true))) {
            net::close_socket(std::exchange(wake_fd_, kInvalidSocket));
        }
        refresh_auto_.store(false);
        refresh_manual_.store(false);
        bursting_.store(true);
        running_.store(true);
    }
    try {
        worker_ = std::thread(&MdnsDiscovery::run, this);
    } catch (const std::system_error&) {
        std::lock_guard wake_lock(wake_mu_);
        running_.store(false);
        bursting_.store(false);
        net::close_socket(std::exchange(wake_fd_, kInvalidSocket));
        cb_ = {};
        on_interfaces_changed_ = {};
        return false;
    }
    return true;
}

void MdnsDiscovery::stop() {
    std::lock_guard lock(lifecycle_mu_);
    {
        std::lock_guard wake_lock(wake_mu_);
        running_.store(false);
        wake_locked();
    }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard wake_lock(wake_mu_);
        net::close_socket(std::exchange(wake_fd_, kInvalidSocket));
    }
    cb_ = {};
    on_interfaces_changed_ = {};
}

void MdnsDiscovery::wake_locked() {
    if (wake_fd_ != kInvalidSocket) {
        const char signal = 1;
        sendto(wake_fd_, &signal, 1, 0, reinterpret_cast<sockaddr*>(&wake_address_), sizeof(wake_address_));
    }
}

void MdnsDiscovery::trigger_query(bool automatic) {
    std::lock_guard wake_lock(wake_mu_);
    if (!running_.load() || (automatic && bursting_.load())) return;
    (automatic ? refresh_auto_ : refresh_manual_).store(true);
    wake_locked();
}

bool MdnsDiscovery::refresh_interfaces() {
    const auto previous_addresses = std::move(local_ipv4s_);
    local_ipv4s_.clear();
    active_ips_.clear();
    for (const auto& iface : net::list_ipv4_interfaces()) {
        if (local_ipv4s_.insert(iface.ipv4).second && !iface.is_loopback && is_usable_device_ipv4(iface.ipv4)) {
            active_ips_.push_back(iface.ipv4);
        }
    }
    if (auto preferred = std::find(active_ips_.begin(), active_ips_.end(), preferred_bind_ipv4_);
        preferred != active_ips_.end()) {
        std::rotate(active_ips_.begin(), preferred, preferred + 1);
    }
    auto is_active = [&](const std::string& ip) {
        return std::find(active_ips_.begin(), active_ips_.end(), ip) != active_ips_.end();
    };

    for (auto it = query_socks_.begin(); it != query_socks_.end();) {
        if (!is_active(it->ipv4)) {
            net::close_socket(it->fd);
            it = query_socks_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& ip : active_ips_) {
        if (std::any_of(query_socks_.begin(), query_socks_.end(), [&](const auto& q) { return q.ipv4 == ip; })) continue;
        if (const auto fd = open_mdns_socket(ip, 0); fd != kInvalidSocket) query_socks_.push_back({ip, fd});
    }

    auto refresh_memberships = [&](SocketHandle& fd, std::set<std::string>& joined, uint16_t port) {
        if (fd == kInvalidSocket) fd = open_mdns_socket("0.0.0.0", port);
        if (fd == kInvalidSocket) return;
        for (auto it = joined.begin(); it != joined.end();) {
            if (is_active(*it)) {
                ++it;
            } else if (multicast_membership(fd, *it, IP_DROP_MEMBERSHIP)) {
                it = joined.erase(it);
            } else {
                // The interface may already be gone. Rebuild the listener with only current memberships.
                net::close_socket(fd);
                joined.clear();
                fd = open_mdns_socket("0.0.0.0", port);
                if (fd == kInvalidSocket) return;
                break;
            }
        }
        for (const auto& ip : active_ips_) {
            if (!joined.contains(ip) && multicast_membership(fd, ip, IP_ADD_MEMBERSHIP)) joined.insert(ip);
        }
    };
    refresh_memberships(sock_5355_, members_5355_, kMiPlayMdnsPort);
    refresh_memberships(sock_5353_, members_5353_, kStdMdnsPort);
    const bool changed = previous_addresses != local_ipv4s_;
    if (changed && running_.load() && on_interfaces_changed_) on_interfaces_changed_();
    return changed;
}

void MdnsDiscovery::send_probe_round(bool qm) {
    auto send_std = [](SocketHandle fd, bool qu) {
        bool ok = send_query(fd, "_mi-connect._udp.local", kStdMdnsPort, qu);
        ok &= send_query(fd, "_lyra-mdns._udp.local", kStdMdnsPort, qu);
        ok &= send_query(fd, "_miplay_lan._tcp.local", kStdMdnsPort, qu);
        return ok;
    };
    for (auto it = query_socks_.begin(); it != query_socks_.end();) {
        const bool lan = send_query(it->fd, "_miplay_lan._tcp.local", kMiPlayMdnsPort, true);
        const bool standard = send_std(it->fd, true);
        if (!lan && !standard) {
            net::close_socket(it->fd);
            it = query_socks_.erase(it);
        } else {
            ++it;
        }
    }
    if (!qm) return;
    for (const auto& ip : active_ips_) {
        if (sock_5355_ != kInvalidSocket && multicast_interface(sock_5355_, ip)) {
            send_query(sock_5355_, "_miplay_lan._tcp.local", kMiPlayMdnsPort, false);
        }
        if (sock_5353_ != kInvalidSocket && multicast_interface(sock_5353_, ip)) send_std(sock_5353_, false);
    }
}

void MdnsDiscovery::receive(std::chrono::milliseconds wait) {
#ifdef _WIN32
    std::vector<WSAPOLLFD> fds;
#else
    std::vector<pollfd> fds;
#endif
    auto watch = [&](SocketHandle fd) {
        if (fd != kInvalidSocket) fds.push_back({fd, POLLIN, 0});
    };
    watch(wake_fd_);
    watch(sock_5355_);
    watch(sock_5353_);
    for (const auto& query : query_socks_) watch(query.fd);
    if (fds.empty()) {
        std::this_thread::sleep_for(wait);
        return;
    }
#ifdef _WIN32
    const auto ready = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), static_cast<int>(wait.count()));
#else
    const auto ready = poll(fds.data(), fds.size(), static_cast<int>(wait.count()));
#endif
    if (ready <= 0 || !running_.load()) return;
    std::array<uint8_t, 4096> bytes{};
    for (const auto& ready_socket : fds) {
        if (!(ready_socket.revents & POLLIN)) continue;
        if (ready_socket.fd == wake_fd_) {
            char signals[32];
            for (int i = 0; i < 16 && recv(wake_fd_, signals, sizeof(signals), 0) > 0; ++i) {}
            continue;
        }
        // Keep a source that is continuously readable from starving queries or stop.
        for (int packet = 0; packet < 8 && running_.load(); ++packet) {
            sockaddr_in peer{};
            socklen_t size = sizeof(peer);
            const auto n = recvfrom(ready_socket.fd, reinterpret_cast<char*>(bytes.data()), bytes.size(), 0,
                                    reinterpret_cast<sockaddr*>(&peer), &size);
            if (n <= 0) break;
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            if (!local_ipv4s_.contains(ip)) publish(parse_announcements(bytes.data(), static_cast<size_t>(n), ip));
        }
    }
}

void MdnsDiscovery::publish(const std::vector<ServiceAnnouncement>& announcements) {
    std::vector<ServiceAnnouncement> remote;
    for (const auto& entry : announcements) {
        if (entry.ttl_seconds == 0 || !local_ipv4s_.contains(entry.device.ip)) remote.push_back(entry);
    }
    std::vector<DiscoveredDevice> updates;
    {
        std::lock_guard lock(mu_);
        updates = cache_.update(remote, sync::monotonic_now_us());
    }
    for (const auto& device : updates) {
        if (!running_.load()) break;
        if (cb_) cb_(device);
    }
}

void MdnsDiscovery::run() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    refresh_interfaces();
    ProbeBurst burst;
    burst.begin(Clock::now());
    auto next_pass = Clock::now();
    unsigned background_round = 0;
    while (running_.load()) {
        const bool automatic = refresh_auto_.exchange(false);
        const bool manual = refresh_manual_.exchange(false);
        if (!burst.active() && (automatic || manual)) {
            bursting_.store(true);
            refresh_interfaces();
            burst.begin(Clock::now());
        } else if (burst.active() && manual && Clock::now() < burst.deadline()) {
            refresh_interfaces();
            send_probe_round(true);
        }
        const auto now = Clock::now();
        auto next = next_pass;
        if (burst.active()) {
            if (const auto step = burst.take_due(now)) {
                send_probe_round(*step != 1);
                if (!burst.active()) {
                    bursting_.store(false);
                    background_round = 0;
                    next_pass = Clock::now() + 3s;
                }
            }
            next = burst.active() ? burst.deadline() : next_pass;
        } else if (now >= next_pass) {
            const bool changed = refresh_interfaces();
            send_probe_round((++background_round % 4) == 0 || changed);
            next_pass = Clock::now() + 3s;
            next = next_pass;
        }
        publish({});
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(next - Clock::now());
        receive(std::clamp(remaining, 0ms, wake_fd_ == kInvalidSocket ? 25ms : 250ms));
    }
    for (const auto& query : query_socks_) net::close_socket(query.fd);
    query_socks_.clear();
    net::close_socket(std::exchange(sock_5355_, kInvalidSocket));
    net::close_socket(std::exchange(sock_5353_, kInvalidSocket));
    members_5355_.clear();
    members_5353_.clear();
}

std::vector<DiscoveredDevice> MdnsDiscovery::list_devices() const {
    std::lock_guard lock(mu_);
    return cache_.list_devices();
}

} // namespace miplay::discovery
