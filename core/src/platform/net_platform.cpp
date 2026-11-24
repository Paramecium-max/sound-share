#include "platform/net_platform.h"

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
        if (!(ifa->ifa_flags & IFF_UP)) continue;
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
    std::vector<uint8_t> buf(out_len);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, addrs, &out_len) == NO_ERROR) {
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

} // namespace miplay::net
