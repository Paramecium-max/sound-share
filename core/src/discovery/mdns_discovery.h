#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <chrono>
#include <set>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay::discovery {

enum class PortSource { Default, Srv, Txt, AppsData, Manual };

struct DiscoveredDevice {
    std::string id;          // MAC 或 IP 唯一标识
    std::string name;
    std::string ip;
    uint16_t cmd_port = 8899;
    PortSource port_source = PortSource::Default;
    bool has_explicit_port() const { return port_source != PortSource::Default; }
    std::string mac;
    std::string id_hash;
    std::string room_name;
    std::string group_name;
    int32_t device_type = 4; // 2:TV/Box, 3:PC, 4:Speaker/Pad
    bool is_master = false;
    bool support_audio = true;
    bool is_manual = false;
    uint8_t name_priority = 0; // 0=兜底, 1=Lyra AppData/实例名, 2=_mi-connect/_miplay_lan 显式名
    uint64_t last_seen_us = 0;
    uint64_t revision = 0;
    bool online = true;
};

using DeviceFoundCallback = std::function<void(const DiscoveredDevice&)>;

struct ServiceAnnouncement {
    std::string instance;
    std::string source_ip;
    uint32_t ttl_seconds = 0;
    DiscoveredDevice device;
};

class DiscoveryCache {
public:
    std::vector<DiscoveredDevice> update(const std::vector<ServiceAnnouncement>& announcements, uint64_t now_us);
    std::vector<DiscoveredDevice> list_devices() const;
private:
    struct Service {
        DiscoveredDevice device;
        std::string device_key;
        uint64_t expiry_us;
    };
    std::map<std::pair<std::string, std::string>, Service> services_;
    std::map<std::string, DiscoveredDevice> devices_;
    std::map<std::string, std::string> identity_aliases_;
    uint64_t revision_ = 0;
};

// UDP event loop for QU replies on each active interface and QM on 5353/5355.
class MdnsDiscovery {
public:
    MdnsDiscovery() = default;
    ~MdnsDiscovery();

    bool start(const std::string& bind_ipv4, DeviceFoundCallback cb,
               std::function<void()> on_interfaces_changed = {});
    void stop();
    void trigger_query(bool automatic = false);

    std::vector<DiscoveredDevice> list_devices() const;

    // 公开静态辅助接口（便于协议单元测试验证 QU 报文、appsData 端口与多服务解析）
    static std::vector<uint8_t> build_ptr_query_packet(const std::string& domain, bool request_unicast);
    static bool parse_response_packet(const uint8_t* data, size_t len, const std::string& src_ip,
                                      DiscoveredDevice& out_dev);

    static std::vector<ServiceAnnouncement> parse_announcements(const uint8_t* data, size_t len,
                                                                 const std::string& src_ip);
private:
    struct InterfaceQuerySocket {
        std::string ipv4;
        SocketHandle fd;
    };

    void run();
    bool refresh_interfaces();
    void wake_locked();
    void send_probe_round(bool include_multicast_qm);
    void receive(std::chrono::milliseconds wait);
    void publish(const std::vector<ServiceAnnouncement>& announcements);

    std::mutex lifecycle_mu_;
    std::mutex wake_mu_;
    SocketHandle wake_fd_ = kInvalidSocket;
    sockaddr_in wake_address_{};
    std::atomic<bool> running_{false};
    std::string preferred_bind_ipv4_;
    SocketHandle sock_5355_ = kInvalidSocket;
    SocketHandle sock_5353_ = kInvalidSocket;
    std::vector<InterfaceQuerySocket> query_socks_;
    std::set<std::string> local_ipv4s_;
    std::vector<std::string> active_ips_;
    std::set<std::string> members_5355_;
    std::set<std::string> members_5353_;
    std::atomic<bool> refresh_auto_{false};
    std::atomic<bool> refresh_manual_{false};
    std::atomic<bool> bursting_{false};
    std::thread worker_;
    DeviceFoundCallback cb_;
    std::function<void()> on_interfaces_changed_;
    mutable std::mutex mu_;
    DiscoveryCache cache_;
};

} // namespace miplay::discovery
