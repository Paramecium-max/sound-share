#include "rtsp/wfd_rtsp_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

namespace miplay::rtsp {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool send_str(SocketHandle fd, const std::string& s) {
    return net::send_all(fd, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

} // namespace

WfdRtspServer::~WfdRtspServer() {
    stop();
}

std::string WfdRtspServer::build_m1_options(const std::string& sender_ip, uint16_t timer_port, int cseq) {
    const uint32_t ip_u32 = net::ipv4_to_u32_host_order(sender_ip);
    std::ostringstream oss;
    oss << "OPTIONS * RTSP/1.0\r\n"
        << "Date: Tue, 24 Nov 2026 04:00:00 +0000\r\n"
        << "Server: \r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Require: org.wfa.wfd1.0\r\n"
        << "wfd_timer_server_port:" << ip_u32 << ":" << timer_port << "\r\n"
        << "lib_version: miplaycast_os3_release1.5 3.1.6030516\r\n\r\n";
    return oss.str();
}

std::string WfdRtspServer::build_time_offset_req(uint64_t time_offset_us, int cseq) {
    std::ostringstream oss;
    oss << "TIME_OFFSET rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
        << "Date: Tue, 24 Nov 2026 04:00:00 +0000\r\n"
        << "Server: \r\n"
        << "CSeq: " << cseq << "\r\n"
        << "Content-Type: text/parameters\r\n"
        << "TimeOffset:" << time_offset_us << "\r\n"
        << "Content-Length: 0\r\n\r\n";
    return oss.str();
}

std::string WfdRtspServer::extract_header(const std::string& msg, const std::string& key) {
    const std::string target = to_lower(key) + ":";
    std::istringstream iss(msg);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (to_lower(line).rfind(target, 0) == 0) {
            std::string val = line.substr(target.size());
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            return val;
        }
    }
    return "";
}

bool WfdRtspServer::start(const std::string& device_id,
                          const std::string& sender_ip,
                          uint16_t timer_service_port,
                          uint64_t session_time_offset_us,
                          bool prefer_lpcm,
                          RtspServerCallbacks cbs) {
    stop();
    net::init_network();

    device_id_ = device_id;
    sender_ip_ = sender_ip;
    timer_port_ = timer_service_port;
    time_offset_us_ = session_time_offset_us;
    prefer_lpcm_ = prefer_lpcm;
    cbs_ = std::move(cbs);

    listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ == kInvalidSocket) return false;
    net::set_reuse_addr(listen_sock_);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock_, 8) != 0) {
        net::close_socket(listen_sock_);
        listen_sock_ = kInvalidSocket;
        return false;
    }

    struct sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (getsockname(listen_sock_, reinterpret_cast<struct sockaddr*>(&actual), &alen) == 0) {
        port_.store(ntohs(actual.sin_port));
    }

#ifdef _WIN32
    DWORD tv = 200;
    setsockopt(listen_sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{0, 200000};
    setsockopt(listen_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    running_.store(true);
    accept_th_ = std::thread(&WfdRtspServer::accept_loop, this);
    return true;
}

void WfdRtspServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_sock_ != kInvalidSocket) {
        net::close_socket(listen_sock_);
        listen_sock_ = kInvalidSocket;
    }
    if (accept_th_.joinable()) accept_th_.join();
    for (auto& t : client_ths_) {
        if (t.joinable()) t.join();
    }
    client_ths_.clear();
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        for (auto& [_, fd] : pending_conns_) net::close_socket(fd);
        pending_conns_.clear();
    }
    port_.store(0);
}

SocketHandle WfdRtspServer::claim_pending_port_socket(uint16_t peer_src_port) {
    for (int i = 0; i < 30 && running_.load(); ++i) {
        {
            std::lock_guard<std::mutex> lock(pending_mu_);
            auto it = pending_conns_.find(peer_src_port);
            if (it != pending_conns_.end()) {
                SocketHandle fd = it->second;
                pending_conns_.erase(it);
                return fd;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return kInvalidSocket;
}

void WfdRtspServer::accept_loop() {
    bool has_ctrl = false;
    while (running_.load()) {
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        SocketHandle client = accept(listen_sock_, reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (client == kInvalidSocket) continue;

        net::set_tcp_nodelay(client, true);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        uint16_t peer_port = ntohs(peer.sin_port);

        if (!has_ctrl) {
            has_ctrl = true;
            client_ths_.emplace_back(&WfdRtspServer::handle_rtsp_control, this, client, std::string(ip));
        } else {
            std::lock_guard<std::mutex> lock(pending_mu_);
            pending_conns_[peer_port] = client;
        }
    }
}

void WfdRtspServer::handle_rtsp_control(SocketHandle ctrl_sock, const std::string& peer_ip) {
#ifdef _WIN32
    DWORD tv = 500;
    setsockopt(ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{0, 500000};
    setsockopt(ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    auto report = [this](const std::string& s) {
        if (cbs_.on_status) cbs_.on_status(device_id_, s);
    };

    report("RTSP 已反连 (" + peer_ip + ")，正在 M1 OPTIONS 握手...");

    // M1 OPTIONS 使用 CSeq: 1，兼容所有官方设备及第三方接收端
    if (!send_str(ctrl_sock, build_m1_options(sender_ip_, timer_port_, 1))) {
        net::close_socket(ctrl_sock);
        return;
    }

    std::string acc;
    char buf[4096];
    bool sent_m3 = false;
    bool sent_m4 = false;
    bool sent_m5 = false;
    bool sent_time_offset = false;
    bool has_multi_port = false;
    SocketHandle claimed_data_sock = kInvalidSocket;
    const std::string session_id = "1189641421";

    while (running_.load()) {
        auto n = recv(ctrl_sock, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (net::last_socket_error_is_timeout()) continue;
            break;
        }
        acc.append(buf, static_cast<size_t>(n));

        while (true) {
            auto hdr_end = acc.find("\r\n\r\n");
            if (hdr_end == std::string::npos) break;
            std::string hdr = acc.substr(0, hdr_end + 4);
            size_t content_len = 0;
            std::string cl = extract_header(hdr, "Content-Length");
            if (!cl.empty()) {
                try { content_len = static_cast<size_t>(std::stoul(cl)); } catch (...) {}
            }
            if (acc.size() < hdr_end + 4 + content_len) break;
            std::string msg = acc.substr(0, hdr_end + 4 + content_len);
            acc.erase(0, hdr_end + 4 + content_len);

            std::string cseq = extract_header(msg, "CSeq");

            if (msg.rfind("OPTIONS ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nPublic: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n\r\n";
                send_str(ctrl_sock, resp.str());

                if (!sent_m3) {
                    sent_m3 = true;
                    report("RTSP M3 查询接收端音频能力...");
                    const std::string body =
                        "wfd_content_SP_protection\r\n"
                        "wfd_video_formats\r\n"
                        "wfd_audio_codecs\r\n"
                        "wfd_audio_codecs_v2\r\n"
                        "wfd_client_rtp_ports\r\n"
                        "wfd_tcp_enable\r\n"
                        "wfd_tcp_multi_session_enable\r\n";
                    std::ostringstream m3;
                    m3 << "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 2\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << body.size() << "\r\n\r\n" << body;
                    send_str(ctrl_sock, m3.str());
                }
            } else if (msg.rfind("RTSP/1.0 200 OK", 0) == 0) {
                if (cseq == "2" && !sent_m4) {
                    sent_m4 = true;
                    bool has_v2 = (msg.find("wfd_audio_codecs_v2") != std::string::npos);
                    bool has_v1 = (msg.find("wfd_audio_codecs:") != std::string::npos);
                    report("RTSP M4 下发" + std::string(prefer_lpcm_ ? "LPCM" : "ADTS-AAC") + "参数...");

                    std::ostringstream body;
                    if (has_v2 || !has_v1) {
                        body << "wfd_type_encryp: 0 1 1 0 0\r\n"
                             << "wfd_audio_codecs_v2: " << (prefer_lpcm_ ? "2 1" : "1 1") << "\r\n";
                    }
                    if (has_v1) {
                        body << "wfd_audio_codecs: "
                             << (prefer_lpcm_ ? "LPCM 00000002 00" : "AAC 00000001 00") << "\r\n";
                    }
                    body << "wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n"
                         << "wfd_presentation_URL: rtsp://" << sender_ip_ << "/wfd1.0/streamid=0 none\r\n";
                    std::string b = body.str();
                    std::ostringstream m4;
                    m4 << "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 3\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << b.size() << "\r\n\r\n" << b;
                    send_str(ctrl_sock, m4.str());
                } else if (cseq == "3" && !sent_m5) {
                    sent_m5 = true;
                    report("RTSP M5 触发 SETUP...");
                    const std::string b = "wfd_trigger_method: SETUP\r\n";
                    std::ostringstream m5;
                    m5 << "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 4\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << b.size() << "\r\n\r\n" << b;
                    send_str(ctrl_sock, m5.str());
                }
            } else if (msg.rfind("SETUP ", 0) == 0) {
                report("RTSP M6 SETUP 建立数据通道...");
                std::string mp = extract_header(msg, "MultiPort");
                uint16_t multi_port = 0;
                auto pos = mp.find("multi_port=");
                if (pos != std::string::npos) {
                    try { multi_port = static_cast<uint16_t>(std::stoi(mp.substr(pos + 11))); } catch (...) {}
                }
                if (multi_port > 0) {
                    has_multi_port = true;
                    claimed_data_sock = claim_pending_port_socket(multi_port);
                    // 官方小米接收端（含 MultiPort）在 SETUP 响应前期望 TIME_OFFSET
                    send_str(ctrl_sock, build_time_offset_req(time_offset_us_, 5));
                    sent_time_offset = true;
                }

                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=60"
                     << "\r\nTransport: RTP/AVP/TCP;interleaved=0-1;\r\n\r\n";
                send_str(ctrl_sock, resp.str());
            } else if (msg.rfind("PLAY ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=60"
                     << "\r\nRange: npt=now-\r\n\r\n";
                send_str(ctrl_sock, resp.str());

                if (!sent_time_offset) {
                    send_str(ctrl_sock, build_time_offset_req(time_offset_us_, 5));
                    sent_time_offset = true;
                }

                report(has_multi_port ? "RTSP M7 同步推流中 (MultiPort)" : "RTSP M7 同步推流中 (TCP Interleaved)");
                SocketHandle out_sock = (claimed_data_sock != kInvalidSocket) ? claimed_data_sock : ctrl_sock;
                if (cbs_.on_stream_ready) {
                    cbs_.on_stream_ready(device_id_, peer_ip, out_sock);
                }
            } else if (msg.rfind("GET_PARAMETER ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=20\r\n\r\n";
                send_str(ctrl_sock, resp.str());
            } else if (msg.rfind("VIDEO_LATENCY ", 0) == 0) {
                RtspLatencyReport rep;
                rep.device_id = device_id_;
                try {
                    auto l = extract_header(msg, "latency");
                    auto b = extract_header(msg, "bitrate");
                    auto p = extract_header(msg, "rtpPacketNum");
                    if (!l.empty()) rep.latency_ms = std::stoi(l);
                    if (!b.empty()) rep.bitrate = std::stoi(b);
                    if (!p.empty()) rep.rtp_packet_num = std::stoull(p);
                } catch (...) {}
                if (cbs_.on_latency_report) cbs_.on_latency_report(rep);
            } else if (msg.rfind("TEARDOWN ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n\r\n";
                send_str(ctrl_sock, resp.str());
                break;
            }
        }
    }

    net::close_socket(ctrl_sock);
    report("RTSP 会话已关闭");
    if (cbs_.on_session_closed) cbs_.on_session_closed(device_id_);
}

} // namespace miplay::rtsp
