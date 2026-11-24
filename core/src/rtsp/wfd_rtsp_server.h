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

namespace miplay::rtsp {

struct RtspLatencyReport {
    std::string device_id;
    int32_t latency_ms = 0;
    int32_t bitrate = 0;
    uint64_t rtp_packet_num = 0;
};

struct RtspServerCallbacks {
    std::function<void(const std::string& device_id, const std::string& peer_ip, SocketHandle data_sock)> on_stream_ready;
    std::function<void(const RtspLatencyReport& report)> on_latency_report;
    std::function<void(const std::string& device_id)> on_session_closed;
    std::function<void(const std::string& device_id, const std::string& status)> on_status;
};

// 严格对齐红米 K90 实机捕获的反向 WFD RTSP 服务器（兼容官方设备与标准 WFD 接收端）
class WfdRtspServer {
public:
    WfdRtspServer() = default;
    ~WfdRtspServer();

    bool start(const std::string& device_id,
               const std::string& sender_ip,
               uint16_t timer_service_port,
               uint64_t session_time_offset_us,
               bool prefer_lpcm,
               RtspServerCallbacks cbs);

    void stop();
    uint16_t listen_port() const { return port_.load(); }
    bool is_running() const { return running_.load(); }

    static std::string build_m1_options(const std::string& sender_ip, uint16_t timer_port, int cseq = 1);
    static std::string build_time_offset_req(uint64_t time_offset_us, int cseq = 5);

private:
    void accept_loop();
    void handle_rtsp_control(SocketHandle ctrl_sock, const std::string& peer_ip);
    SocketHandle claim_pending_port_socket(uint16_t peer_src_port);
    static std::string extract_header(const std::string& msg, const std::string& key);

    std::string device_id_;
    std::string sender_ip_;
    uint16_t timer_port_ = 0;
    uint64_t time_offset_us_ = 0;
    bool prefer_lpcm_ = false;
    RtspServerCallbacks cbs_;

    SocketHandle listen_sock_ = kInvalidSocket;
    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    std::thread accept_th_;
    std::vector<std::thread> client_ths_;

    std::mutex pending_mu_;
    std::map<uint16_t, SocketHandle> pending_conns_;
};

} // namespace miplay::rtsp
