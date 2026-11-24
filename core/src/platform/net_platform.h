#pragma once

#include <cstdint>
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
bool set_tcp_nodelay(SocketHandle fd, bool enable);
bool set_reuse_addr(SocketHandle fd);
uint32_t ipv4_to_u32_host_order(const std::string& ipv4);
std::string u32_host_order_to_ipv4(uint32_t ip);
std::vector<NetworkInterface> list_ipv4_interfaces();
std::string pick_default_lan_ipv4();
bool send_all(SocketHandle fd, const uint8_t* data, size_t len);
bool last_socket_error_is_timeout();
int last_socket_error_code();

} // namespace miplay::net
