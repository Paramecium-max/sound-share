#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay::discovery {

struct DiscoveredDevice {
    std::string id;          // MAC 或 IP 唯一标识
    std::string name;
    std::string ip;
    uint16_t cmd_port = 8899;
    std::string mac;
    std::string id_hash;
    std::string room_name;
    std::string group_name;
    int32_t device_type = 4; // 2:TV/Box, 3:PC, 4:Speaker/Pad
    bool is_master = false;
    bool support_audio = true;
    bool is_manual = false;
    uint64_t last_seen_us = 0;
};

using DeviceFoundCallback = std::function<void(const DiscoveredDevice&)>;

// 局域网 mDNS 发现器：同时查询 UDP 5355 (_miplay_lan._tcp.local.) 与 UDP 5353
class MdnsDiscovery {
public:
    MdnsDiscovery() = default;
    ~MdnsDiscovery();

    bool start(const std::string& bind_ipv4, DeviceFoundCallback cb);
    void stop();
    void trigger_query();

    std::vector<DiscoveredDevice> list_devices() const;

private:
    SocketHandle create_multicast_socket(const std::string& bind_ipv4, uint16_t port);
    void send_ptr_query(SocketHandle fd, const std::string& qname, uint16_t dst_port);
    void recv_loop(SocketHandle fd, uint16_t listen_port);
    void parse_mdns_packet(const uint8_t* data, size_t len, const std::string& src_ip);

    std::atomic<bool> running_{false};
    SocketHandle sock_5355_ = kInvalidSocket;
    SocketHandle sock_5353_ = kInvalidSocket;
    std::thread th_5355_;
    std::thread th_5353_;
    std::thread th_query_;
    DeviceFoundCallback cb_;
    mutable std::mutex mu_;
    std::map<std::string, DiscoveredDevice> devices_;
};

} // namespace miplay::discovery
