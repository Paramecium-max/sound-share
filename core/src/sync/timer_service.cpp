#include "sync/timer_service.h"
#include "sync/master_clock.h"

#include <cstring>
#include <limits>

namespace miplay::sync {

namespace {

uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    }
    return v;
}

void write_u64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

} // namespace

TimerService::~TimerService() {
    stop();
}

bool TimerService::start(const std::string& bind_ip, uint16_t port) {
    if (running_.load()) return true;
    net::init_network();

    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kInvalidSocket) return false;

    net::set_reuse_addr(sock_);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_ip.empty() || bind_ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
    }

    if (bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_socket(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    struct sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (getsockname(sock_, reinterpret_cast<struct sockaddr*>(&actual), &alen) == 0) {
        port_.store(ntohs(actual.sin_port));
    }

    running_.store(true);
    worker_ = std::thread(&TimerService::run_loop, this);
    return true;
}

void TimerService::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sock_ != kInvalidSocket) {
        net::close_socket(sock_);
        sock_ = kInvalidSocket;
    }
    port_.store(0);
}

void TimerService::run_loop() {
    uint8_t buf[128];
    while (running_.load()) {
        if (!net::wait_readable(sock_, 50) || !running_.load()) continue;
        struct sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        auto n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                          reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (n != 40) {
            continue;
        }
        const uint64_t t_recv = monotonic_now_us();

        const uint64_t q0 = read_u64_le(buf + 0);
        const uint64_t q4 = read_u64_le(buf + 32);

        // 严格按实机抓取规范：原样保留 Q[0], Q[1], Q[4]，将 Q[2], Q[3] 覆写为服务端主时钟
        const uint64_t t_send = monotonic_now_us();
        write_u64_le(buf + 16, t_recv);
        write_u64_le(buf + 24, t_send);

        sendto(sock_, reinterpret_cast<const char*>(buf), 40, 0,
               reinterpret_cast<struct sockaddr*>(&peer), peer_len);

        char ip_str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip_str, sizeof(ip_str));
        const std::string ip(ip_str);

        std::lock_guard<std::mutex> lock(telemetry_mu_);
        auto& item = telemetry_[ip];
        item.client_ip = ip;
        item.client_port = ntohs(peer.sin_port);
        item.last_seq = q4;
        item.last_seen_us = t_recv;
        constexpr auto max_signed = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        item.residual_error_us = q0 != 0 && q0 <= max_signed && t_recv <= max_signed
            ? static_cast<int64_t>(t_recv) - static_cast<int64_t>(q0) : 0;
        item.packet_count++;
    }
}

std::map<std::string, ClientSyncTelemetry> TimerService::snapshot_telemetry() const {
    std::lock_guard<std::mutex> lock(telemetry_mu_);
    return telemetry_;
}

} // namespace miplay::sync
