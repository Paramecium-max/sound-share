#pragma once

#include "platform/net_platform.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace miplay::sidecar {

using WsMessageHandler = std::function<std::string(const std::string& request_json)>;

// 轻量本地 RFC 6455 WebSocket 服务器 (仅绑定 127.0.0.1 + Token 鉴权，零外部依赖)
class LocalWsServer {
public:
    LocalWsServer() = default;
    ~LocalWsServer();

    bool start(uint16_t port, const std::string& auth_token, WsMessageHandler handler);
    void stop();
    uint16_t port() const { return port_.load(); }
    void broadcast_text(const std::string& text);

private:
    void accept_loop();
    void handle_client(SocketHandle fd);
    static bool send_ws_text(SocketHandle fd, const std::string& text);
    static std::string compute_accept_key(const std::string& client_key);

    SocketHandle listen_fd_ = kInvalidSocket;
    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    std::string auth_token_;
    WsMessageHandler handler_;
    std::thread accept_th_;
    std::vector<std::thread> client_ths_;
    std::mutex clients_mu_;
    std::map<SocketHandle, bool> clients_; // socket -> handshake complete
};

} // namespace miplay::sidecar
