#include "platform/net_platform.h"
#include "platform/deadline_write.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>

#ifdef _WIN32
  #include <iphlpapi.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <poll.h>
#endif

namespace miplay::net {

void init_network() {
#ifdef _WIN32
    static std::once_flag flag;
    std::call_once(flag, []() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

void close_socket(SocketHandle fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

int last_socket_error_code() {
#ifdef _WIN32
    return static_cast<int>(WSAGetLastError());
#else
    return errno;
#endif
}

bool last_socket_error_is_timeout() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

bool set_nonblocking(SocketHandle fd, bool enable) {
    if (fd == kInvalidSocket) return false;
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool wait_readable(SocketHandle fd, int timeout_ms) {
#ifdef _WIN32
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
    pollfd entry{fd, POLLIN, 0};
    return poll(&entry, 1, timeout_ms) > 0;
#endif
}

bool wait_connect_result(SocketHandle fd, int timeout_ms) {
#ifdef _WIN32
    fd_set write_set, error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(fd, &write_set);
    FD_SET(fd, &error_set);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, nullptr, &write_set, &error_set, &timeout) > 0;
#else
    pollfd entry{fd, POLLOUT, 0};
    return poll(&entry, 1, timeout_ms) > 0;
#endif
}

bool set_socket_timeout(SocketHandle fd, int option, int timeout_ms) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeout_ms);
#else
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
    return setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}

void shutdown_socket(SocketHandle fd) {
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

bool set_tcp_nodelay(SocketHandle fd, bool enable) {
    int opt = enable ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool set_reuse_addr(SocketHandle fd) {
    int opt = 1;
    bool ok = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif
    return ok;
}

uint32_t ipv4_to_u32_host_order(const std::string& ipv4) {
    struct in_addr addr{};
    if (inet_pton(AF_INET, ipv4.c_str(), &addr) != 1) {
        return 0;
    }
    return ntohl(addr.s_addr);
}

std::string u32_host_order_to_ipv4(uint32_t ip) {
    struct in_addr addr{};
    addr.s_addr = htonl(ip);
    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
}

std::vector<NetworkInterface> list_ipv4_interfaces() {
    std::vector<NetworkInterface> result;
#ifndef _WIN32
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || !ifaddr) return result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) ||
            !(ifa->ifa_flags & (IFF_RUNNING | IFF_LOOPBACK))) continue;
        char ip[INET_ADDRSTRLEN]{};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        NetworkInterface iface;
        iface.name = ifa->ifa_name ? ifa->ifa_name : "";
        iface.ipv4 = ip;
        iface.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        result.push_back(std::move(iface));
    }
    freeifaddrs(ifaddr);
#else
    ULONG out_len = 16384;
    std::vector<uint8_t> buf;
    IP_ADAPTER_ADDRESSES* addrs = nullptr;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempts = 0; attempts < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempts) {
        buf.resize(out_len);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                      nullptr, addrs, &out_len);
    }
    if (status == NO_ERROR) {
        for (auto* a = addrs; a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* sin = reinterpret_cast<struct sockaddr_in*>(u->Address.lpSockaddr);
                char ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                NetworkInterface iface;
                iface.name = a->AdapterName ? a->AdapterName : "";
                iface.ipv4 = ip;
                iface.is_loopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
                result.push_back(std::move(iface));
            }
        }
    }
#endif
    return result;
}

std::string pick_default_lan_ipv4() {
    auto ifaces = list_ipv4_interfaces();
    for (const auto& i : ifaces) {
        if (i.is_loopback) continue;
        if (i.ipv4.rfind("192.168.", 0) == 0 || i.ipv4.rfind("10.", 0) == 0) {
            if (i.name.find("docker") == std::string::npos &&
                i.name.find("br-") == std::string::npos) {
                return i.ipv4;
            }
        }
    }
    for (const auto& i : ifaces) {
        if (!i.is_loopback) return i.ipv4;
    }
    return "127.0.0.1";
}

bool is_local_ipv4(const std::string& ip) {
    const auto interfaces = list_ipv4_interfaces();
    return std::any_of(interfaces.begin(), interfaces.end(), [&](const auto& iface) { return iface.ipv4 == ip; });
}

std::string local_ipv4_for_peer(const std::string& peer_ip, uint16_t port) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &peer.sin_addr) != 1) return {};
    const auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) return {};
    sockaddr_in local{};
    socklen_t size = sizeof(local);
    const bool ok = connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0 &&
                    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0;
    close_socket(fd);
    return ok ? u32_host_order_to_ipv4(ntohl(local.sin_addr.s_addr)) : "";
}

bool send_all(SocketHandle fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(fd, reinterpret_cast<const char*>(data + sent),
                     static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
#else
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
#endif
        sent += static_cast<size_t>(n);
    }
    return true;
}

ConnectionWriter::ConnectionWriter(SocketHandle fd, bool owns_socket, SendOperation send_override)
    : fd_(fd), owns_socket_(owns_socket), usable_(set_nonblocking(fd, true)),
      send_override_(std::move(send_override)) {}

ConnectionWriter::~ConnectionWriter() {
    if (owns_socket_) close_socket(fd_);
}

void ConnectionWriter::abort_locked() {
    usable_ = false;
    if (fd_ != kInvalidSocket) shutdown_socket(fd_);
}

void ConnectionWriter::abort() {
    std::lock_guard lock(mutex_);
    abort_locked();
}

SendAllStatus ConnectionWriter::write(const uint8_t* data, size_t len, int timeout_ms,
                                      const std::atomic<bool>* cancel) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    if (cancel && cancel->load()) return SendAllStatus::Cancelled;
    std::unique_lock lock(mutex_, std::defer_lock);
    if (!lock.try_lock_until(deadline)) {
        usable_.store(false);
        if (fd_ != kInvalidSocket) shutdown_socket(fd_);
        return SendAllStatus::Failed;
    }
    if (!usable_.load()) return SendAllStatus::Failed;
    auto send_bytes = [&](const uint8_t* buffer, size_t remaining) -> WriteAttempt {
        using State = WriteAttempt::State;
        if (!usable_.load()) return {State::Failed};
        if (send_override_) return send_override_(buffer, remaining);
        const int chunk = static_cast<int>(std::min<size_t>(remaining, INT_MAX));
#ifdef _WIN32
        const auto n = send(fd_, reinterpret_cast<const char*>(buffer), chunk, 0);
#else
        const auto n = ::send(fd_, buffer, chunk, MSG_NOSIGNAL);
#endif
        if (n > 0) return {State::Progress, static_cast<size_t>(n)};
        if (n == 0) return {State::Failed};
        const int err = last_socket_error_code();
#ifdef _WIN32
        if (err == WSAEINTR) return {State::Interrupted};
        if (err == WSAEWOULDBLOCK) return {State::WouldBlock};
        // WSAETIMEDOUT and other terminal errors invalidate the connection.
#else
        if (err == EINTR) return {State::Interrupted};
        if (err == EAGAIN || err == EWOULDBLOCK) return {State::WouldBlock};
#endif
        return {State::Failed};
    };
    auto wait = [&](Clock::time_point until) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - Clock::now());
        if (remaining > 0ms) wait_connect_result(fd_, static_cast<int>(std::min(remaining, 10ms).count()));
    };
    const auto status = write_until(data, len, deadline, cancel, Clock::now, send_bytes, wait);
    if (status == SendAllStatus::Partial || status == SendAllStatus::Failed) abort_locked();
    return status;
}

} // namespace miplay::net
