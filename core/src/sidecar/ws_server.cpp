#include "sidecar/ws_server.h"
#include "crypto/mini_crypto.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace miplay::sidecar {

LocalWsServer::~LocalWsServer() {
    stop();
}

std::string LocalWsServer::compute_accept_key(const std::string& client_key) {
    const std::string magic = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto digest = crypto::sha1(reinterpret_cast<const uint8_t*>(magic.data()), magic.size());
    return crypto::base64_encode(digest.data(), digest.size());
}

bool LocalWsServer::send_ws_text(SocketHandle fd, const std::string& text) {
    std::vector<uint8_t> frame;
    frame.reserve(10 + text.size());
    frame.push_back(0x81); // FIN=1, opcode=TEXT
    const size_t n = text.size();
    if (n < 126) {
        frame.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(n & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF));
        }
    }
    frame.insert(frame.end(), text.begin(), text.end());
    return net::send_all(fd, frame.data(), frame.size());
}

bool LocalWsServer::start(uint16_t port, const std::string& auth_token, WsMessageHandler handler) {
    if (running_.load()) return true;
    net::init_network();
    auth_token_ = auth_token;
    handler_ = std::move(handler);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == kInvalidSocket) return false;
    net::set_reuse_addr(listen_fd_);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd_, 16) != 0 || !net::set_nonblocking(listen_fd_, true)) {
        net::close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return false;
    }

    struct sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&actual), &alen) == 0) {
        port_.store(ntohs(actual.sin_port));
    }

    running_.store(true);
    accept_th_ = std::thread(&LocalWsServer::accept_loop, this);
    return true;
}

void LocalWsServer::stop() {
    if (!running_.exchange(false)) return;
    if (accept_th_.joinable()) accept_th_.join();
    if (listen_fd_ != kInvalidSocket) {
        net::close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (const auto& [fd, authenticated] : clients_) net::shutdown_socket(fd);
    }
    for (auto& t : client_ths_) {
        if (t.joinable()) t.join();
    }
    client_ths_.clear();
    port_.store(0);
}

void LocalWsServer::broadcast_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(clients_mu_);
    if (!running_.load()) return;
    for (auto& [fd, authenticated] : clients_) {
        if (authenticated && !send_ws_text(fd, text)) {
            authenticated = false;
            net::shutdown_socket(fd);
        }
    }
}

void LocalWsServer::accept_loop() {
    while (running_.load()) {
        if (!net::wait_readable(listen_fd_, 50) || !running_.load()) continue;
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        SocketHandle fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (fd == kInvalidSocket) continue;
        if (!net::set_nonblocking(fd, false) || !net::set_socket_timeout(fd, SO_SNDTIMEO, 100)) {
            net::close_socket(fd);
            continue;
        }
        net::set_tcp_nodelay(fd, true);
        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            clients_.emplace(fd, false);
        }
        client_ths_.emplace_back([this, fd]() {
            handle_client(fd);
            std::lock_guard<std::mutex> lock(clients_mu_);
            clients_.erase(fd);
            net::close_socket(fd);
        });
    }
}

void LocalWsServer::handle_client(SocketHandle fd) {
    char hbuf[4096];
    while (running_.load() && !net::wait_readable(fd, 50)) {}
    if (!running_.load()) return;
    auto n = recv(fd, hbuf, sizeof(hbuf) - 1, 0);
    if (n <= 0) {
        return;
    }
    hbuf[n] = '\0';
    std::string req(hbuf, static_cast<size_t>(n));

    // Token 校验（URL Query ?token=xxx）
    if (!auth_token_.empty() && req.find("token=" + auth_token_) == std::string::npos) {
        const char* deny = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        net::send_all(fd, reinterpret_cast<const uint8_t*>(deny), std::strlen(deny));
        return;
    }

    std::string ws_key;
    std::istringstream iss(req);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Sec-WebSocket-Key:", 0) == 0 || line.rfind("sec-websocket-key:", 0) == 0) {
            ws_key = line.substr(18);
            while (!ws_key.empty() && ws_key.front() == ' ') ws_key.erase(ws_key.begin());
        }
    }
    if (ws_key.empty()) {
        return;
    }

    std::string accept_val = compute_accept_key(ws_key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_val + "\r\n\r\n";
    if (!net::send_all(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size())) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        clients_.at(fd) = true;
    }

    std::vector<uint8_t> acc;
    uint8_t buf[4096];
    while (running_.load()) {
        auto r = recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (r == 0) break;
        if (r < 0) break;
        acc.insert(acc.end(), buf, buf + r);

        while (acc.size() >= 2) {
            uint8_t b0 = acc[0];
            uint8_t b1 = acc[1];
            uint8_t opcode = b0 & 0x0F;
            bool masked = (b1 & 0x80) != 0;
            uint64_t plen = b1 & 0x7F;
            size_t off = 2;
            if (plen == 126) {
                if (acc.size() < 4) break;
                plen = (static_cast<uint64_t>(acc[2]) << 8) | acc[3];
                off = 4;
            } else if (plen == 127) {
                if (acc.size() < 10) break;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen << 8) | acc[2 + i];
                off = 10;
            }
            if (acc.size() < off + (masked ? 4 : 0) + plen) break;

            uint8_t mask[4]{};
            if (masked) {
                std::memcpy(mask, acc.data() + off, 4);
                off += 4;
            }
            std::string payload(static_cast<size_t>(plen), '\0');
            for (size_t i = 0; i < static_cast<size_t>(plen); ++i) {
                payload[i] = static_cast<char>(masked ? (acc[off + i] ^ mask[i % 4]) : acc[off + i]);
            }
            acc.erase(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(off + plen));

            if (opcode == 0x08) { // Close
                return;
            } else if (opcode == 0x09) { // Ping -> Pong
                std::vector<uint8_t> pong = {0x8A, 0x00};
                std::lock_guard<std::mutex> lock(clients_mu_);
                if (!net::send_all(fd, pong.data(), pong.size())) return;
            } else if (opcode == 0x01 && handler_) { // Text frame
                std::string reply = handler_(payload);
                if (!reply.empty()) {
                    std::lock_guard<std::mutex> lock(clients_mu_);
                    if (!send_ws_text(fd, reply)) return;
                }
            }
        }
    }
}

} // namespace miplay::sidecar
