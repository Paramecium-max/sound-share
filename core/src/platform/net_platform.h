#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace miplay::net {

struct NetworkInterface {
    std::string name;
    std::string ipv4;
    bool is_loopback = false;
};

void init_network();
void close_socket(SocketHandle fd);
bool set_nonblocking(SocketHandle fd, bool enable);
bool wait_readable(SocketHandle fd, int timeout_ms);
bool wait_connect_result(SocketHandle fd, int timeout_ms);
bool set_socket_timeout(SocketHandle fd, int option, int timeout_ms);
void shutdown_socket(SocketHandle fd);
bool set_tcp_nodelay(SocketHandle fd, bool enable);
bool set_reuse_addr(SocketHandle fd);
uint32_t ipv4_to_u32_host_order(const std::string& ipv4);
std::string u32_host_order_to_ipv4(uint32_t ip);
std::vector<NetworkInterface> list_ipv4_interfaces();
std::string pick_default_lan_ipv4();
std::string local_ipv4_for_peer(const std::string& peer_ip, uint16_t port);
bool is_local_ipv4(const std::string& ip);
enum class SendAllStatus {
    Ok = 0,
    TimeoutClean = 1, // 首字节写出前超时，TCP 字节流帧边界完好
    Failed,
    Partial,
    Cancelled
};

bool send_all(SocketHandle fd, const uint8_t* data, size_t len);
struct WriteAttempt {
    enum class State { Progress, Interrupted, WouldBlock, Failed };
    State state;
    size_t bytes = 0;
};

class ConnectionWriter {
public:
    using SendOperation = std::function<WriteAttempt(const uint8_t*, size_t)>;
    explicit ConnectionWriter(SocketHandle fd, bool owns_socket = false, SendOperation send_override = {});
    ~ConnectionWriter();
    ConnectionWriter(const ConnectionWriter&) = delete;
    ConnectionWriter& operator=(const ConnectionWriter&) = delete;

    SendAllStatus write(const uint8_t* data, size_t len, int timeout_ms,
                        const std::atomic<bool>* cancel = nullptr);
    void abort();
    SocketHandle socket() const { return fd_; }

private:
    void abort_locked();
    const SocketHandle fd_;
    const bool owns_socket_;
    std::timed_mutex mutex_;
    std::atomic<bool> usable_;
    SendOperation send_override_;
};
bool last_socket_error_is_timeout();
int last_socket_error_code();

} // namespace miplay::net
