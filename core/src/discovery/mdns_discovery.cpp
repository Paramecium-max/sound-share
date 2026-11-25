#include "discovery/mdns_discovery.h"
#include "sync/master_clock.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

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

bool is_lyra_service_name(const std::string& raw) {
    return to_lower_ascii(raw).find("_lyra-mdns") != std::string::npos;
}

bool is_dedicated_miplay_service(const std::string& raw) {
    const std::string s = to_lower_ascii(raw);
    return s.find("_miplay_lan") != std::string::npos ||
           s.find("_mi-connect") != std::string::npos;
}

bool is_miplay_service_name(const std::string& raw) {
    return is_dedicated_miplay_service(raw) || is_lyra_service_name(raw);
}

bool is_valid_utf8_printable_name(const uint8_t* p, size_t n) {
    if (!p || n == 0 || n > 128) return false;
    size_t i = 0;
    bool has_visible = false;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            if (c != ' ') has_visible = true;
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return false;
            has_visible = true;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false;
            has_visible = true;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= n || (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return false;
            has_visible = true;
            i += 4;
        } else {
            return false;
        }
    }
    return has_visible;
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
    return true;
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
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t v = kTable[c];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string decode_dns_name(const uint8_t* buf, size_t len, size_t& offset, int depth = 0) {
    if (depth > 8) return "";
    std::string out;
    size_t orig = offset;
    bool jumped = false;
    int steps = 0;
    while (offset < len && steps++ < 64) {
        uint8_t lb = buf[offset];
        if (lb == 0) {
            offset += 1;
            break;
        }
        if ((lb & 0xC0) == 0xC0) {
            if (offset + 1 >= len) break;
            size_t ptr = ((lb & 0x3F) << 8) | buf[offset + 1];
            if (ptr >= len) break;
            if (!jumped) {
                orig = offset + 2;
                jumped = true;
            }
            size_t tmp = ptr;
            std::string sub = decode_dns_name(buf, len, tmp, depth + 1);
            if (!out.empty() && !sub.empty()) out.push_back('.');
            out += sub;
            break;
        } else {
            offset += 1;
            if (offset + lb > len) break;
            if (!out.empty()) out.push_back('.');
            out.append(reinterpret_cast<const char*>(buf + offset), lb);
            offset += lb;
        }
    }
    if (jumped) offset = orig;
    return out;
}

void join_multicast_on_ip(SocketHandle fd, const std::string& ipv4) {
    struct ip_mreq mreq{};
    inet_pton(AF_INET, kMdnsGroup, &mreq.imr_multiaddr);
    if (!ipv4.empty() && ipv4 != "0.0.0.0") {
        if (inet_pton(AF_INET, ipv4.c_str(), &mreq.imr_interface) != 1) return;
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
    }
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
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

bool MdnsDiscovery::parse_response_packet(const uint8_t* data, size_t len, const std::string& src_ip,
                                          DiscoveredDevice& out_dev) {
    if (!data || len <= 12) return false;
    uint16_t flags = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    if ((flags & 0x8000) == 0) return false; // 只处理 Response

    uint16_t qd = (static_cast<uint16_t>(data[4]) << 8) | data[5];
    uint16_t an = (static_cast<uint16_t>(data[6]) << 8) | data[7];
    uint16_t ns = (static_cast<uint16_t>(data[8]) << 8) | data[9];
    uint16_t ar = (static_cast<uint16_t>(data[10]) << 8) | data[11];

    size_t off = 12;
    for (uint16_t i = 0; i < qd && off < len; ++i) {
        decode_dns_name(data, len, off);
        if (off + 4 > len) return false;
        off += 4;
    }

    DiscoveredDevice dev;
    dev.ip = src_ip;
    dev.cmd_port = 8899;
    bool matched_miplay = false;
    bool matched_dedicated = false;
    bool has_live_rr = false;
    bool explicit_name_from_txt = false;
    bool explicit_port_from_txt = false;
    bool has_apps_field = false;
    bool apps_has_miplay = false;
    bool has_miplay_appsdata = false;
    std::string fallback_a_ip;

    const uint32_t total_rr = static_cast<uint32_t>(an) + ns + ar;
    for (uint32_t i = 0; i < total_rr && off + 10 <= len; ++i) {
        std::string rname = decode_dns_name(data, len, off);
        if (off + 10 > len) break;
        const uint16_t rtype = (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
        const uint32_t ttl =
            (static_cast<uint32_t>(data[off + 4]) << 24) |
            (static_cast<uint32_t>(data[off + 5]) << 16) |
            (static_cast<uint32_t>(data[off + 6]) << 8) |
            static_cast<uint32_t>(data[off + 7]);
        const uint16_t rdlen = (static_cast<uint16_t>(data[off + 8]) << 8) | data[off + 9];
        off += 10;
        if (off + rdlen > len) break;
        const size_t rstart = off;
        off += rdlen;

        // 跳过 TTL == 0 的 mDNS Goodbye 退网记录，避免将已下线设备误报为在线
        if (ttl == 0) continue;

        const bool rname_miplay = is_miplay_service_name(rname);
        if (rname_miplay) {
            matched_miplay = true;
            if (is_dedicated_miplay_service(rname)) matched_dedicated = true;
            has_live_rr = true;
            if (!explicit_name_from_txt && is_generic_name(dev.name, dev.ip)) {
                auto dot = rname.find('.');
                if (dot != std::string::npos && dot > 0 && rname[0] != '_') {
                    std::string cleaned = clean_instance_label(rname.substr(0, dot));
                    if (!is_hex_id_name(cleaned)) dev.name = std::move(cleaned);
                }
            }
        }

        if (rtype == 12) { // PTR
            size_t tmp = rstart;
            std::string target = decode_dns_name(data, len, tmp);
            if (is_miplay_service_name(target)) {
                matched_miplay = true;
                if (is_dedicated_miplay_service(target)) matched_dedicated = true;
                has_live_rr = true;
                auto dot = target.find('.');
                if (dot != std::string::npos && dot > 0 && target[0] != '_' &&
                    !explicit_name_from_txt && is_generic_name(dev.name, dev.ip)) {
                    std::string cleaned = clean_instance_label(target.substr(0, dot));
                    if (!is_hex_id_name(cleaned)) dev.name = std::move(cleaned);
                }
            }
        } else if (rtype == 33 && rdlen >= 6) { // SRV
            uint16_t srv_port = (static_cast<uint16_t>(data[rstart + 4]) << 8) | data[rstart + 5];
            // _miplay_lan 的 SRV 端口即为 8899 类控制端口；_mi-connect / _lyra-mdns 的真实控制端口由 appsData/TXT 提供
            if (srv_port > 0 && !explicit_port_from_txt &&
                to_lower_ascii(rname).find("_miplay_lan") != std::string::npos) {
                dev.cmd_port = srv_port;
            }
        } else if (rtype == 1 && rdlen == 4) { // A
            char a_ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, data + rstart, a_ip, sizeof(a_ip));
            if (is_usable_device_ipv4(a_ip)) {
                fallback_a_ip = a_ip;
            }
        } else if (rtype == 16 && rname_miplay) { // TXT (仅解析匹配服务的 TXT)
            size_t pos = rstart;
            while (pos < rstart + rdlen) {
                uint8_t kv_len = data[pos++];
                if (pos + kv_len > rstart + rdlen) break;
                std::string kv(reinterpret_cast<const char*>(data + pos), kv_len);
                pos += kv_len;
                auto eq = kv.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                std::string k = kv.substr(0, eq);
                std::string kl = to_lower_ascii(k);
                std::string v = kv.substr(eq + 1);
                if ((kl == "name" || kl == "dn" || kl == "devicename") && !v.empty()) {
                    dev.name = v;
                    explicit_name_from_txt = true;
                } else if (kl == "mac" || kl == "m") {
                    if (!v.empty()) dev.mac = v;
                } else if (kl == "idhash") {
                    dev.id_hash = v;
                } else if (kl == "roomname") {
                    dev.room_name = v;
                } else if (kl == "groupname") {
                    dev.group_name = v;
                } else if (kl == "ismaster") {
                    dev.is_master = (v == "1" || v == "true");
                } else if (kl == "devicetype" || kl == "type" || kl == "dev") {
                    try { dev.device_type = std::stoi(v); } catch (...) {}
                } else if (kl == "apps") {
                    has_apps_field = true;
                    if (apps_list_contains_miplay(v)) apps_has_miplay = true;
                } else if (kl == "port" || kl == "po") {
                    try {
                        int p = std::stoi(v);
                        if (p > 0 && p < 65536) {
                            dev.cmd_port = static_cast<uint16_t>(p);
                            explicit_port_from_txt = true;
                        }
                    } catch (...) {}
                } else if (kl == "appsdata" && !v.empty()) {
                    auto raw = base64_decode_simple(v);
                    if (raw.size() >= 7 && raw[0] == 0x81) {
                        uint16_t cp = (static_cast<uint16_t>(raw[5]) << 8) | raw[6];
                        if (cp > 0) {
                            dev.cmd_port = cp;
                            explicit_port_from_txt = true;
                            has_miplay_appsdata = true;
                        }
                    }
                } else if (kl == "appdata" && !v.empty()) {
                    // _lyra-mdns._udp.local. 的真实设备名位于 Base64 AppData 二进制 TLV (T=0x02) 内
                    auto raw = base64_decode_simple(v);
                    std::string lyra_name = parse_lyra_appdata_device_name(raw);
                    if (!lyra_name.empty()) {
                        dev.name = std::move(lyra_name);
                        explicit_name_from_txt = true;
                    }
                }
            }
        }
    }

    if (!matched_miplay || !has_live_rr) return false;
    // 若 _mi-connect 明确通告了 apps 列表但不包含 5 (MiPlay) 且无 MiPlay appsData，则为纯互联非音频设备，跳过
    if (has_apps_field && !apps_has_miplay && !has_miplay_appsdata) return false;
    if (!is_usable_device_ipv4(dev.ip) && !fallback_a_ip.empty()) {
        dev.ip = fallback_a_ip;
    } else if (!fallback_a_ip.empty() && src_ip == "127.0.0.1") {
        dev.ip = fallback_a_ip;
    }
    if (!is_usable_device_ipv4(dev.ip)) return false;
    if (dev.name.empty()) {
        dev.name = "MiPlay-" + dev.ip;
        dev.name_priority = 0;
    } else if (matched_dedicated && explicit_name_from_txt) {
        dev.name_priority = 2; // _mi-connect / _miplay_lan 显式 TXT name 最高优先级
    } else {
        dev.name_priority = 1; // _lyra-mdns AppData 解码名或已清洗实例名
    }
    dev.id = !dev.mac.empty() ? dev.mac : dev.ip;
    dev.last_seen_us = sync::monotonic_now_us();
    out_dev = std::move(dev);
    return true;
}

MdnsDiscovery::~MdnsDiscovery() {
    stop();
}

std::vector<std::string> MdnsDiscovery::collect_active_ipv4s() const {
    std::vector<std::string> ips;
    std::set<std::string> seen;
    auto push_unique = [&](const std::string& ip) {
        if (!is_usable_device_ipv4(ip)) return;
        if (seen.insert(ip).second) ips.push_back(ip);
    };

    if (!preferred_bind_ipv4_.empty() && preferred_bind_ipv4_ != "0.0.0.0") {
        push_unique(preferred_bind_ipv4_);
    }
    for (const auto& iface : net::list_ipv4_interfaces()) {
        if (iface.is_loopback) continue;
        push_unique(iface.ipv4);
    }
    if (ips.empty()) {
        ips.push_back("0.0.0.0");
    }
    return ips;
}

SocketHandle MdnsDiscovery::create_multicast_socket(const std::vector<std::string>& iface_ips, uint16_t port) {
    SocketHandle fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) return kInvalidSocket;

    net::set_reuse_addr(fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_port = 0;
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            net::close_socket(fd);
            return kInvalidSocket;
        }
    }

    int ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    int loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

    if (!iface_ips.empty() && iface_ips[0] != "0.0.0.0") {
        struct in_addr out_if{};
        if (inet_pton(AF_INET, iface_ips[0].c_str(), &out_if) == 1) {
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&out_if), sizeof(out_if));
        }
    }
    for (const auto& ip : iface_ips) {
        join_multicast_on_ip(fd, ip);
    }
    return fd;
}

SocketHandle MdnsDiscovery::create_unicast_query_socket(const std::string& iface_ip) {
    SocketHandle fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) return kInvalidSocket;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // 独占临时端口：强制接收端单播直返响应，100% 绕开 Windows Dnscache 5353 冲突与 Wi-Fi 组播丢包
    if (!iface_ip.empty() && iface_ip != "0.0.0.0") {
        if (inet_pton(AF_INET, iface_ip.c_str(), &addr.sin_addr) != 1) {
            addr.sin_addr.s_addr = INADDR_ANY;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            net::close_socket(fd);
            return kInvalidSocket;
        }
    }

    int ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    if (!iface_ip.empty() && iface_ip != "0.0.0.0") {
        struct in_addr out_if{};
        if (inet_pton(AF_INET, iface_ip.c_str(), &out_if) == 1) {
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&out_if), sizeof(out_if));
        }
    }
    return fd;
}

void MdnsDiscovery::close_query_sockets() {
    std::lock_guard<std::mutex> lk(qsock_mu_);
    for (auto& qs : query_socks_) {
        if (qs.fd != kInvalidSocket) {
            net::close_socket(qs.fd);
            qs.fd = kInvalidSocket;
        }
    }
    query_socks_.clear();
}

void MdnsDiscovery::refresh_interface_sockets_if_needed() {
    auto active_ips = collect_active_ipv4s();
    std::set<std::string> desired(active_ips.begin(), active_ips.end());

    std::lock_guard<std::mutex> lk(qsock_mu_);
    std::set<std::string> current;
    for (const auto& qs : query_socks_) current.insert(qs.ipv4);
    if (current == desired && !query_socks_.empty()) return;

    // 移除已下线网卡对应的查询 Socket
    for (auto it = query_socks_.begin(); it != query_socks_.end();) {
        if (desired.find(it->ipv4) == desired.end()) {
            if (it->fd != kInvalidSocket) net::close_socket(it->fd);
            it = query_socks_.erase(it);
        } else {
            ++it;
        }
    }

    // 为新增或刚切换上来的网卡创建独占 QU 单播查询 Socket，并补充加入 5353/5355 组播监听
    for (const auto& ip : active_ips) {
        if (current.find(ip) != current.end()) continue;
        SocketHandle fd = create_unicast_query_socket(ip);
        if (fd != kInvalidSocket) {
            query_socks_.push_back({ip, fd});
        }
        if (sock_5355_ != kInvalidSocket) join_multicast_on_ip(sock_5355_, ip);
        if (sock_5353_ != kInvalidSocket) join_multicast_on_ip(sock_5353_, ip);
    }
}

bool MdnsDiscovery::start(const std::string& bind_ipv4, DeviceFoundCallback cb) {
    if (running_.load()) return true;
    net::init_network();
    preferred_bind_ipv4_ = bind_ipv4;
    cb_ = std::move(cb);

    const auto active_ips = collect_active_ipv4s();
    sock_5355_ = create_multicast_socket(active_ips, kMiPlayMdnsPort);
    sock_5353_ = create_multicast_socket(active_ips, kStdMdnsPort);
    refresh_interface_sockets_if_needed();

    bool has_any_sock = (sock_5355_ != kInvalidSocket || sock_5353_ != kInvalidSocket);
    {
        std::lock_guard<std::mutex> lk(qsock_mu_);
        has_any_sock = has_any_sock || !query_socks_.empty();
    }
    if (!has_any_sock) {
        return false;
    }

    running_.store(true);
    if (sock_5355_ != kInvalidSocket) {
        th_5355_ = std::thread(&MdnsDiscovery::recv_loop, this, sock_5355_, kMiPlayMdnsPort);
    }
    if (sock_5353_ != kInvalidSocket) {
        th_5353_ = std::thread(&MdnsDiscovery::recv_loop, this, sock_5353_, kStdMdnsPort);
    }
    th_unicast_recv_ = std::thread(&MdnsDiscovery::unicast_recv_loop, this);

    th_query_ = std::thread([this]() {
        uint64_t handled_gen = 0;
        // 启动首拍立即执行 3 连发极速探测（0ms / 220ms / 650ms），开屏秒现设备
        auto run_burst = [&]() {
            refresh_interface_sockets_if_needed();
            static constexpr int kDelaysMs[3] = {0, 220, 430};
            for (int step = 0; step < 3 && running_.load(); ++step) {
                if (kDelaysMs[step] > 0) {
                    std::unique_lock<std::mutex> lk(burst_mu_);
                    burst_cv_.wait_for(lk, std::chrono::milliseconds(kDelaysMs[step]),
                                       [this]() { return !running_.load(); });
                    if (!running_.load()) return;
                }
                send_probe_round(step == 0 || step == 2);
            }
        };

        run_burst();

        int bg_tick = 0;
        while (running_.load()) {
            bool manual_triggered = false;
            {
                std::unique_lock<std::mutex> lk(burst_mu_);
                burst_cv_.wait_for(lk, std::chrono::seconds(3), [this, handled_gen]() {
                    return !running_.load() || burst_gen_ != handled_gen;
                });
                if (!running_.load()) break;
                if (burst_gen_ != handled_gen) {
                    handled_gen = burst_gen_;
                    manual_triggered = true;
                }
            }
            if (manual_triggered) {
                run_burst();
                bg_tick = 0;
            } else {
                // 后台无感自动发现：每 3s 发轻量 QU 单播探测；每 12s 同步检查网卡变更并补一轮 QM 组播兜底
                ++bg_tick;
                if (bg_tick % 4 == 0) {
                    refresh_interface_sockets_if_needed();
                }
                send_probe_round((bg_tick % 4) == 0);
            }
        }
    });
    return true;
}

void MdnsDiscovery::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(burst_mu_);
        ++burst_gen_;
    }
    burst_cv_.notify_all();
    if (th_query_.joinable()) th_query_.join();
    if (th_unicast_recv_.joinable()) th_unicast_recv_.join();
    if (th_5355_.joinable()) th_5355_.join();
    if (th_5353_.joinable()) th_5353_.join();
    close_query_sockets();
    if (sock_5355_ != kInvalidSocket) {
        net::close_socket(sock_5355_);
        sock_5355_ = kInvalidSocket;
    }
    if (sock_5353_ != kInvalidSocket) {
        net::close_socket(sock_5353_);
        sock_5353_ = kInvalidSocket;
    }
}

void MdnsDiscovery::send_ptr_query(SocketHandle fd, const std::string& qname, uint16_t dst_port, bool request_unicast) {
    if (fd == kInvalidSocket) return;
    auto pkt = build_ptr_query_packet(qname, request_unicast);
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port);
    inet_pton(AF_INET, kMdnsGroup, &dst.sin_addr);
    sendto(fd, reinterpret_cast<const char*>(pkt.data()),
           static_cast<int>(pkt.size()), 0,
           reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
}

void MdnsDiscovery::send_probe_round(bool include_multicast_qm) {
    // 1. 主力通道：通过每张活跃网卡的独占临时端口发送 QU (0x8001) 单播响应查询
    std::vector<SocketHandle> qfds;
    {
        std::lock_guard<std::mutex> lk(qsock_mu_);
        qfds.reserve(query_socks_.size());
        for (const auto& qs : query_socks_) {
            if (qs.fd != kInvalidSocket) qfds.push_back(qs.fd);
        }
    }
    for (SocketHandle fd : qfds) {
        send_ptr_query(fd, "_miplay_lan._tcp.local", kMiPlayMdnsPort, true);
        send_ptr_query(fd, "_mi-connect._udp.local", kStdMdnsPort, true);
        send_ptr_query(fd, "_lyra-mdns._udp.local", kStdMdnsPort, true);
        send_ptr_query(fd, "_miplay_lan._tcp.local", kStdMdnsPort, true);
    }

    // 2. 兜底通道：从 5355/5353 组播端口发送传统 QM 查询，兼容只响应标准端口源的老设备
    if (include_multicast_qm) {
        send_ptr_query(sock_5355_, "_miplay_lan._tcp.local", kMiPlayMdnsPort, false);
        send_ptr_query(sock_5353_, "_mi-connect._udp.local", kStdMdnsPort, false);
        send_ptr_query(sock_5353_, "_lyra-mdns._udp.local", kStdMdnsPort, false);
        send_ptr_query(sock_5353_, "_miplay_lan._tcp.local", kStdMdnsPort, false);
    }
}

void MdnsDiscovery::trigger_query() {
    if (running_.load()) {
        {
            std::lock_guard<std::mutex> lk(burst_mu_);
            ++burst_gen_;
        }
        burst_cv_.notify_one();
    } else {
        send_probe_round(true);
    }
}

void MdnsDiscovery::recv_loop(SocketHandle fd, uint16_t /*listen_port*/) {
    uint8_t buf[4096];
    while (running_.load()) {
        if (!net::wait_readable(fd, 200) || !running_.load()) continue;
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        auto n = recvfrom(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                          reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (n <= 12) continue;
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        handle_incoming_packet(buf, static_cast<size_t>(n), ip);
    }
}

void MdnsDiscovery::unicast_recv_loop() {
    uint8_t buf[4096];
    while (running_.load()) {
        std::vector<SocketHandle> fds;
        {
            std::lock_guard<std::mutex> lk(qsock_mu_);
            fds.reserve(query_socks_.size());
            for (const auto& qs : query_socks_) {
                if (qs.fd != kInvalidSocket) fds.push_back(qs.fd);
            }
        }
        if (fds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }
#ifdef _WIN32
        fd_set read_set;
        FD_ZERO(&read_set);
        const size_t limit = std::min<size_t>(fds.size(), FD_SETSIZE);
        for (size_t i = 0; i < limit; ++i) FD_SET(fds[i], &read_set);
        timeval tv{0, 150000}; // 150ms
        int ready = select(0, &read_set, nullptr, nullptr, &tv);
        if (ready <= 0 || !running_.load()) continue;
        for (size_t i = 0; i < limit; ++i) {
            if (!FD_ISSET(fds[i], &read_set)) continue;
            struct sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            auto n = recvfrom(fds[i], reinterpret_cast<char*>(buf), sizeof(buf), 0,
                              reinterpret_cast<struct sockaddr*>(&peer), &plen);
            if (n <= 12) continue;
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            handle_incoming_packet(buf, static_cast<size_t>(n), ip);
        }
#else
        std::vector<pollfd> pfds(fds.size());
        for (size_t i = 0; i < fds.size(); ++i) {
            pfds[i].fd = fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        int ready = poll(pfds.data(), pfds.size(), 150);
        if (ready <= 0 || !running_.load()) continue;
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            auto n = recvfrom(pfds[i].fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                              reinterpret_cast<struct sockaddr*>(&peer), &plen);
            if (n <= 12) continue;
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            handle_incoming_packet(buf, static_cast<size_t>(n), ip);
        }
#endif
    }
}

void MdnsDiscovery::handle_incoming_packet(const uint8_t* data, size_t len, const std::string& src_ip) {
    // 忽略来自本机任意活跃网卡的回环自发包（避免将本机电脑管家/小米妙享服务误识别为远端音箱）
    {
        std::lock_guard<std::mutex> lk(qsock_mu_);
        for (const auto& qs : query_socks_) {
            if (!qs.ipv4.empty() && qs.ipv4 != "0.0.0.0" && qs.ipv4 == src_ip) return;
        }
    }
    if (!preferred_bind_ipv4_.empty() && preferred_bind_ipv4_ != "0.0.0.0" && preferred_bind_ipv4_ == src_ip) {
        return;
    }

    DiscoveredDevice incoming;
    if (!parse_response_packet(data, len, src_ip, incoming)) return;
    // 二次校验解析后的设备 IP 也不得指向本机自身
    {
        std::lock_guard<std::mutex> lk(qsock_mu_);
        for (const auto& qs : query_socks_) {
            if (!qs.ipv4.empty() && qs.ipv4 != "0.0.0.0" && qs.ipv4 == incoming.ip) return;
        }
    }

    DiscoveredDevice merged = incoming;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // 按 id 或相同 IP 智能合并：防止同一设备同时响应 _mi-connect 与 _lyra-mdns 时产生重复条目
        auto it = devices_.find(incoming.id);
        if (it == devices_.end()) {
            for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
                if (iter->second.ip == incoming.ip) {
                    it = iter;
                    break;
                }
            }
        }
        if (it != devices_.end()) {
            DiscoveredDevice cur = it->second;
            cur.ip = incoming.ip;
            cur.last_seen_us = incoming.last_seen_us;
            // 仅当新名称优先级 >= 已有名称（或已有名称为兜底/随机ID）时才更新，防止 _lyra-mdns 覆盖 _mi-connect 真名
            const bool cur_generic = is_generic_name(cur.name, cur.ip);
            const bool inc_generic = is_generic_name(incoming.name, incoming.ip);
            if (!inc_generic && (cur_generic || incoming.name_priority >= cur.name_priority)) {
                cur.name = incoming.name;
                cur.name_priority = incoming.name_priority;
            }
            if (!incoming.mac.empty()) cur.mac = incoming.mac;
            if (!incoming.id_hash.empty()) cur.id_hash = incoming.id_hash;
            if (!incoming.room_name.empty()) cur.room_name = incoming.room_name;
            if (!incoming.group_name.empty()) cur.group_name = incoming.group_name;
            if (incoming.cmd_port != 8899 || cur.cmd_port == 0) cur.cmd_port = incoming.cmd_port;
            if (incoming.device_type != 4) cur.device_type = incoming.device_type;
            cur.is_master = cur.is_master || incoming.is_master;
            // 保持既有稳定 ID，避免 UI 卡片 key 抖动或丢失已勾选状态
            merged = cur;
            it->second = cur;
        } else {
            devices_[merged.id] = merged;
        }
    }
    if (cb_) cb_(merged);
}

std::vector<DiscoveredDevice> MdnsDiscovery::list_devices() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<DiscoveredDevice> res;
    res.reserve(devices_.size());
    for (const auto& [_, d] : devices_) res.push_back(d);
    return res;
}

} // namespace miplay::discovery
