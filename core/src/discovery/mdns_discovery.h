#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <condition_variable>
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
    uint8_t name_priority = 0; // 0=兜底, 1=Lyra AppData/实例名, 2=_mi-connect/_miplay_lan 显式名
    uint64_t last_seen_us = 0;
};

using DeviceFoundCallback = std::function<void(const DiscoveredDevice&)>;

// 局域网高可靠 mDNS 发现器：
// - 主力路径：每张活跃物理/局域网 IPv4 网卡使用独立独占临时端口发送 RFC 6762 QU (0x8001) 单播响应查询，
//   100% 绕开 Windows Dnscache/5353 端口争抢与 Wi-Fi 组播回包丢包，毫秒级单播必达；
// - 兜底路径：UDP 5355 + 5353 全网卡组播监听，零延迟接收设备上线广播并兼容老款仅回组播的设备；
// - 全服务最强兼容：同时探测 _miplay_lan._tcp.local. / _mi-connect._udp.local. / _lyra-mdns._udp.local.，
//   完整解析 _mi-connect appsData 控制端口与 _lyra-mdns AppData 二进制 TLV (T=0x02) UTF-8 真名，
//   过滤本机自发包、198.18.0.0/15 代理段与 TTL=0 退网包，并按优先级智能合并去重；
// - 无感自愈：手动刷新触发 3 连发 Burst 并热重载网卡列表，后台每 3s 轻量无感巡检。
class MdnsDiscovery {
public:
    MdnsDiscovery() = default;
    ~MdnsDiscovery();

    bool start(const std::string& bind_ipv4, DeviceFoundCallback cb);
    void stop();
    void trigger_query();

    std::vector<DiscoveredDevice> list_devices() const;

    // 公开静态辅助接口（便于协议单元测试验证 QU 报文、appsData 端口与多服务解析）
    static std::vector<uint8_t> build_ptr_query_packet(const std::string& domain, bool request_unicast);
    static bool parse_response_packet(const uint8_t* data, size_t len, const std::string& src_ip,
                                      DiscoveredDevice& out_dev);

private:
    struct InterfaceQuerySocket {
        std::string ipv4;
        SocketHandle fd = kInvalidSocket;
    };

    SocketHandle create_multicast_socket(const std::vector<std::string>& iface_ips, uint16_t port);
    SocketHandle create_unicast_query_socket(const std::string& iface_ip);
    std::vector<std::string> collect_active_ipv4s() const;
    void refresh_interface_sockets_if_needed();
    void send_ptr_query(SocketHandle fd, const std::string& qname, uint16_t dst_port, bool request_unicast);
    void send_probe_round(bool include_multicast_qm);
    void recv_loop(SocketHandle fd, uint16_t listen_port);
    void unicast_recv_loop();
    void handle_incoming_packet(const uint8_t* data, size_t len, const std::string& src_ip);
    void close_query_sockets();

    std::atomic<bool> running_{false};
    std::string preferred_bind_ipv4_;
    SocketHandle sock_5355_ = kInvalidSocket;
    SocketHandle sock_5353_ = kInvalidSocket;

    mutable std::mutex qsock_mu_;
    std::vector<InterfaceQuerySocket> query_socks_;

    std::mutex burst_mu_;
    std::condition_variable burst_cv_;
    uint64_t burst_gen_{0};

    std::thread th_5355_;
    std::thread th_5353_;
    std::thread th_unicast_recv_;
    std::thread th_query_;
    DeviceFoundCallback cb_;
    mutable std::mutex mu_;
    std::map<std::string, DiscoveredDevice> devices_;
};

} // namespace miplay::discovery
