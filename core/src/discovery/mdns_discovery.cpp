#include "discovery/mdns_discovery.h"
#include "sync/master_clock.h"

#include <chrono>
#include <cstring>

namespace miplay::discovery {

namespace {

constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr uint16_t kMiPlayMdnsPort = 5355;
constexpr uint16_t kStdMdnsPort = 5353;

std::vector<uint8_t> build_dns_ptr_query(const std::string& domain) {
    std::vector<uint8_t> pkt(12, 0);
    pkt[5] = 1; // QDCOUNT = 1
    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0 && len < 64) {
            pkt.push_back(static_cast<uint8_t>(len));
            pkt.insert(pkt.end(), domain.begin() + start, domain.begin() + dot);
        }
        start = dot + 1;
    }
    pkt.push_back(0);    // root label
    pkt.push_back(0x00); // QTYPE = PTR (12)
    pkt.push_back(0x0C);
    pkt.push_back(0x00); // QCLASS = IN (1)
    pkt.push_back(0x01);
    return pkt;
}

std::string decode_dns_name(const uint8_t* buf, size_t len, size_t& offset, int depth = 0) {
    if (depth > 8) return "";
    std::string out;
    size_t orig = offset;
    bool jumped = false;
    while (offset < len) {
        uint8_t lb = buf[offset];
        if (lb == 0) {
            offset += 1;
            break;
        }
        if ((lb & 0xC0) == 0xC0) {
            if (offset + 1 >= len) break;
            size_t ptr = ((lb & 0x3F) << 8) | buf[offset + 1];
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

} // namespace

MdnsDiscovery::~MdnsDiscovery() {
    stop();
}

SocketHandle MdnsDiscovery::create_multicast_socket(const std::string& bind_ipv4, uint16_t port) {
    SocketHandle fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) return kInvalidSocket;

    net::set_reuse_addr(fd);

#ifdef _WIN32
    DWORD tv = 200;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        // 如果系统已占用 5353/5355 且不允许复用，则退化为绑定随机端口用于主动查询
        addr.sin_port = 0;
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            net::close_socket(fd);
            return kInvalidSocket;
        }
    }

    struct ip_mreq mreq{};
    inet_pton(AF_INET, kMdnsGroup, &mreq.imr_multiaddr);
    if (!bind_ipv4.empty() && bind_ipv4 != "0.0.0.0") {
        inet_pton(AF_INET, bind_ipv4.c_str(), &mreq.imr_interface);
        struct in_addr iface_addr{};
        inet_pton(AF_INET, bind_ipv4.c_str(), &iface_addr);
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&iface_addr), sizeof(iface_addr));
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
    }
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    return fd;
}

bool MdnsDiscovery::start(const std::string& bind_ipv4, DeviceFoundCallback cb) {
    if (running_.load()) return true;
    net::init_network();
    cb_ = std::move(cb);

    sock_5355_ = create_multicast_socket(bind_ipv4, kMiPlayMdnsPort);
    sock_5353_ = create_multicast_socket(bind_ipv4, kStdMdnsPort);
    if (sock_5355_ == kInvalidSocket && sock_5353_ == kInvalidSocket) {
        return false;
    }

    running_.store(true);
    if (sock_5355_ != kInvalidSocket) {
        th_5355_ = std::thread(&MdnsDiscovery::recv_loop, this, sock_5355_, kMiPlayMdnsPort);
    }
    if (sock_5353_ != kInvalidSocket) {
        th_5353_ = std::thread(&MdnsDiscovery::recv_loop, this, sock_5353_, kStdMdnsPort);
    }

    th_query_ = std::thread([this]() {
        for (int i = 0; running_.load(); ++i) {
            if (i % 5 == 0) {
                trigger_query();
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    return true;
}

void MdnsDiscovery::stop() {
    if (!running_.exchange(false)) return;
    if (th_query_.joinable()) th_query_.join();
    if (th_5355_.joinable()) th_5355_.join();
    if (th_5353_.joinable()) th_5353_.join();
    if (sock_5355_ != kInvalidSocket) {
        net::close_socket(sock_5355_);
        sock_5355_ = kInvalidSocket;
    }
    if (sock_5353_ != kInvalidSocket) {
        net::close_socket(sock_5353_);
        sock_5353_ = kInvalidSocket;
    }
}

void MdnsDiscovery::send_ptr_query(SocketHandle fd, const std::string& qname, uint16_t dst_port) {
    if (fd == kInvalidSocket) return;
    auto pkt = build_dns_ptr_query(qname);
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port);
    inet_pton(AF_INET, kMdnsGroup, &dst.sin_addr);
    sendto(fd, reinterpret_cast<const char*>(pkt.data()),
           static_cast<int>(pkt.size()), 0,
           reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
}

void MdnsDiscovery::trigger_query() {
    // 实机抓包实证：UDP 5355 查询 _miplay_lan._tcp.local.
    send_ptr_query(sock_5355_, "_miplay_lan._tcp.local", kMiPlayMdnsPort);
    // 兼容部分设备在标准 5353 上广播 _miplay_lan._tcp.local. / _mi-connect._udp.local.
    send_ptr_query(sock_5353_, "_miplay_lan._tcp.local", kStdMdnsPort);
    send_ptr_query(sock_5353_, "_mi-connect._udp.local", kStdMdnsPort);
}

void MdnsDiscovery::recv_loop(SocketHandle fd, uint16_t /*listen_port*/) {
    uint8_t buf[4096];
    while (running_.load()) {
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        auto n = recvfrom(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                          reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (n <= 12) continue;
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        parse_mdns_packet(buf, static_cast<size_t>(n), ip);
    }
}

void MdnsDiscovery::parse_mdns_packet(const uint8_t* data, size_t len, const std::string& src_ip) {
    uint16_t flags = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    if ((flags & 0x8000) == 0) return; // 只处理 Response

    uint16_t qd = (static_cast<uint16_t>(data[4]) << 8) | data[5];
    uint16_t an = (static_cast<uint16_t>(data[6]) << 8) | data[7];
    uint16_t ns = (static_cast<uint16_t>(data[8]) << 8) | data[9];
    uint16_t ar = (static_cast<uint16_t>(data[10]) << 8) | data[11];

    size_t off = 12;
    for (uint16_t i = 0; i < qd && off < len; ++i) {
        decode_dns_name(data, len, off);
        off += 4;
    }

    DiscoveredDevice dev;
    dev.ip = src_ip;
    dev.cmd_port = 8899;
    bool matched_miplay = false;

    const uint32_t total_rr = an + ns + ar;
    for (uint32_t i = 0; i < total_rr && off + 10 <= len; ++i) {
        std::string rname = decode_dns_name(data, len, off);
        if (off + 10 > len) break;
        uint16_t rtype = (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
        uint16_t rdlen = (static_cast<uint16_t>(data[off + 8]) << 8) | data[off + 9];
        off += 10;
        if (off + rdlen > len) break;
        size_t rstart = off;

        if (rname.find("_miplay_lan") != std::string::npos ||
            rname.find("_mi-connect") != std::string::npos) {
            matched_miplay = true;
            if (dev.name.empty()) {
                auto dot = rname.find('.');
                if (dot != std::string::npos && dot > 0 && rname[0] != '_') {
                    dev.name = rname.substr(0, dot);
                }
            }
        }

        if (rtype == 12) { // PTR
            size_t tmp = rstart;
            std::string target = decode_dns_name(data, len, tmp);
            if (target.find("_miplay_lan") != std::string::npos ||
                target.find("_mi-connect") != std::string::npos) {
                matched_miplay = true;
                auto dot = target.find('.');
                if (dot != std::string::npos && dot > 0) {
                    dev.name = target.substr(0, dot);
                }
            }
        } else if (rtype == 33 && rdlen >= 6) { // SRV
            uint16_t srv_port = (static_cast<uint16_t>(data[rstart + 4]) << 8) | data[rstart + 5];
            if (srv_port > 0 && rname.find("_miplay_lan") != std::string::npos) {
                dev.cmd_port = srv_port;
            }
        } else if (rtype == 1 && rdlen == 4) { // A
            char a_ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, data + rstart, a_ip, sizeof(a_ip));
            dev.ip = a_ip;
        } else if (rtype == 16) { // TXT
            size_t pos = rstart;
            while (pos < rstart + rdlen) {
                uint8_t kv_len = data[pos++];
                if (pos + kv_len > rstart + rdlen) break;
                std::string kv(reinterpret_cast<const char*>(data + pos), kv_len);
                pos += kv_len;
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string k = kv.substr(0, eq);
                std::string v = kv.substr(eq + 1);
                if (k == "name" && !v.empty()) dev.name = v;
                else if (k == "mac" || k == "m") dev.mac = v;
                else if (k == "idhash") dev.id_hash = v;
                else if (k == "roomName") dev.room_name = v;
                else if (k == "groupName") dev.group_name = v;
                else if (k == "isMaster") dev.is_master = (v == "1" || v == "true");
                else if (k == "deviceType" || k == "type") {
                    try { dev.device_type = std::stoi(v); } catch (...) {}
                } else if (k == "port" || k == "po") {
                    try { dev.cmd_port = static_cast<uint16_t>(std::stoi(v)); } catch (...) {}
                }
            }
        }
        off += rdlen;
    }

    if (!matched_miplay) return;
    if (dev.name.empty()) dev.name = "MiPlay-" + dev.ip;
    dev.id = !dev.mac.empty() ? dev.mac : dev.ip;
    dev.last_seen_us = sync::monotonic_now_us();

    {
        std::lock_guard<std::mutex> lock(mu_);
        devices_[dev.id] = dev;
    }
    if (cb_) cb_(dev);
}

std::vector<DiscoveredDevice> MdnsDiscovery::list_devices() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<DiscoveredDevice> res;
    res.reserve(devices_.size());
    for (const auto& [_, d] : devices_) res.push_back(d);
    return res;
}

} // namespace miplay::discovery
