#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace miplay::sync {

// 实机实证 40 字节 UDP 校时报文 (5 × Little-Endian uint64)
#pragma pack(push, 1)
struct MiPlayTimerPacket {
    uint64_t client_synced_us;
    uint64_t client_local_us;
    uint64_t server_recv_us;
    uint64_t server_send_us;
    uint64_t seq;
};
#pragma pack(pop)
static_assert(sizeof(MiPlayTimerPacket) == 40, "MiPlayTimerPacket must be 40 bytes");

struct ClientSyncTelemetry {
    std::string client_ip;
    uint16_t client_port = 0;
    uint64_t last_seq = 0;
    uint64_t last_seen_us = 0;
    int64_t residual_error_us = 0; // server_recv_us - client_synced_us
    uint64_t packet_count = 0;
};

// 全屋播放主时钟校时服务器：监听单 UDP 端口，同时服务所有音箱的 40B 校时请求
class TimerService {
public:
    TimerService() = default;
    ~TimerService();

    bool start(const std::string& bind_ip = "0.0.0.0", uint16_t port = 0);
    void stop();

    bool is_running() const { return running_.load(); }
    uint16_t port() const { return port_.load(); }

    std::map<std::string, ClientSyncTelemetry> snapshot_telemetry() const;

private:
    void run_loop();

    SocketHandle sock_ = kInvalidSocket;
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{0};
    std::thread worker_;
    mutable std::mutex telemetry_mu_;
    std::map<std::string, ClientSyncTelemetry> telemetry_;
};

} // namespace miplay::sync
