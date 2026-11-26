#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    std::function<void(const std::string&, const std::string&,
                       std::shared_ptr<net::ConnectionWriter>, std::function<void()>)> on_stream_connection;
    std::function<void(const std::string&)> on_stop_media;
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
               RtspServerCallbacks cbs,
               uint32_t sample_rate = 48000,
               uint16_t bind_port = 0);

    void stop();
    uint16_t listen_port() const { return port_.load(); }
    bool is_running() const { return running_.load(); }

    static std::string build_m1_options(const std::string& sender_ip, uint16_t timer_port, int cseq = 1);
    static std::string build_m3_get_parameter_body();
    static std::string build_time_offset_req(uint64_t time_offset_us, int cseq = 5);
    static std::string build_m4_audio_body(bool prefer_lpcm, uint32_t sample_rate,
                                           const std::string& sender_ip, bool has_v2, bool has_v1);
    static int parse_multi_port_media_port(const std::string& multi_port);

private:
    static constexpr size_t kMaxPendingSockets = 4;

    void accept_loop();
    void handle_rtsp_control(SocketHandle ctrl_sock, const std::string& peer_ip, uint16_t peer_port);
    SocketHandle claim_pending_port_socket(uint16_t peer_src_port);
    static std::string extract_header(const std::string& msg, const std::string& key);

    std::string device_id_;
    std::string sender_ip_;
    uint16_t timer_port_ = 0;
    uint64_t time_offset_us_ = 0;
    bool prefer_lpcm_ = false;
    uint32_t sample_rate_ = 48000;
    RtspServerCallbacks cbs_;

    SocketHandle listen_sock_ = kInvalidSocket;
    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread accept_th_;
    std::vector<std::thread> client_ths_;

    std::mutex pending_mu_;
    std::condition_variable pending_cv_;
    bool has_ctrl_ = false;
    std::string ctrl_peer_ip_;
    std::map<uint16_t, SocketHandle> pending_conns_;
};

} // namespace miplay::rtsp
