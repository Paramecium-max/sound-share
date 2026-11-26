#include "rtsp/wfd_rtsp_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <sstream>

namespace miplay::rtsp {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim_ws(std::string s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}



std::optional<std::string> take_rtsp_message(std::string& buffer, size_t header_end, size_t content_len) {
    const size_t header_len = header_end + 4;
    if (content_len > buffer.size() - header_len) return std::nullopt;
    const size_t size = header_len + content_len;
    std::string msg = buffer.substr(0, size);
    buffer.erase(0, size);
    return msg;
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

std::string WfdRtspServer::build_m3_get_parameter_body() {
    // 对齐小米官方 libaudiomirror-jni.so SendM3 能力查询（含 wfd_tcp_multi_session_enable 与 wfd_image_enable_v2），
    // 确保小爱音箱等官方接收端在 M3/M6 协商独立 MultiPort 音频 TCP 连接，而非回退单连接 TCP Interleaved。
    return
        "wfd_content_SP_protection\r\n"
        "wfd_video_formats\r\n"
        "wfd_video_enctype\r\n"
        "wfd_video_gamuttype\r\n"
        "wfd_video_bitrate\r\n"
        "wfd_dynamic_video_enable\r\n"
        "wfd_current_video_info\r\n"
        "wfd_audio_codecs\r\n"
        "wfd_audio_codecs_v2\r\n"
        "wfd_client_rtp_ports\r\n"
        "wfd_tcp_enable\r\n"
        "wfd_tcp_multi_session_enable\r\n"
        "wfd_support_secure_win\r\n"
        "wfd_mirror_control_enable\r\n"
        "wfd_standby_resume_capability\r\n"
        "wfd_image_enable_v2\r\n";
}

int WfdRtspServer::parse_multi_port_media_port(const std::string& multi_port) {
    size_t start = 0;
    while (start <= multi_port.size()) {
        const size_t sep = multi_port.find(';', start);
        const std::string token = (sep == std::string::npos)
                                      ? multi_port.substr(start)
                                      : multi_port.substr(start, sep - start);
        const size_t eq = token.find('=');
        if (eq != std::string::npos) {
            const std::string key = to_lower(trim_ws(token.substr(0, eq)));
            if (key == "multi_port") {
                const std::string val = trim_ws(token.substr(eq + 1));
                if (!val.empty() &&
                    std::all_of(val.begin(), val.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    try {
                        const long port = std::stol(val);
                        if (port > 0 && port <= 65535) {
                            return static_cast<int>(port);
                        }
                    } catch (...) {}
                }
                return -1;
            }
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return -1;
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

std::string WfdRtspServer::build_m4_audio_body(bool prefer_lpcm, uint32_t sample_rate,
                                               const std::string& sender_ip, bool has_v2, bool has_v1) {
    // 严格按实际音频配置派生 WFD 1.0 音频能力掩码，确保 RTSP M4 与 TS 负载、采样率完全一致
    // LPCM: bit 0 = 44.1kHz (00000001), bit 1 = 48kHz (00000002)
    // AAC : bit 0 = 48kHz 2ch (00000001)
    const char* v2_rate = (sample_rate == 44100) ? "0" : "1";
    const char* lpcm_v1_mask = (sample_rate == 44100) ? "00000001" : "00000002";
    std::ostringstream body;
    if (has_v2 || !has_v1) {
        body << "wfd_type_encryp: 0 1 1 0 0\r\n"
             << "wfd_audio_codecs_v2: " << (prefer_lpcm ? "2 " : "1 ") << v2_rate << "\r\n";
    }
    if (has_v1) {
        body << "wfd_audio_codecs: "
             << (prefer_lpcm ? (std::string("LPCM ") + lpcm_v1_mask + " 00") : "AAC 00000001 00") << "\r\n";
    }
    body << "wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n"
         << "wfd_presentation_URL: rtsp://" << sender_ip << "/wfd1.0/streamid=0 none\r\n";
    return body.str();
}

bool WfdRtspServer::start(const std::string& device_id,
                          const std::string& sender_ip,
                          uint16_t timer_service_port,
                          uint64_t session_time_offset_us,
                          bool prefer_lpcm,
                          RtspServerCallbacks cbs,
                          uint32_t sample_rate,
                          uint16_t bind_port) {
    stop();
    net::init_network();

    device_id_ = device_id;
    sender_ip_ = sender_ip;
    timer_port_ = timer_service_port;
    time_offset_us_ = session_time_offset_us;
    prefer_lpcm_ = prefer_lpcm;
    sample_rate_ = (sample_rate == 44100 || sample_rate == 48000) ? sample_rate : 48000;
    cbs_ = std::move(cbs);

    listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ == kInvalidSocket) return false;
    if (bind_port == 0) net::set_reuse_addr(listen_sock_);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(bind_port);

    if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock_, 8) != 0 || !net::set_nonblocking(listen_sock_, true)) {
        net::close_socket(listen_sock_);
        listen_sock_ = kInvalidSocket;
        port_.store(0);
        return false;
    }

    struct sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (getsockname(listen_sock_, reinterpret_cast<struct sockaddr*>(&actual), &alen) == 0) {
        port_.store(ntohs(actual.sin_port));
    }

    stopping_.store(false);
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        has_ctrl_ = false;
        ctrl_peer_ip_.clear();
        for (auto& [_, fd] : pending_conns_) net::close_socket(fd);
        pending_conns_.clear();
    }
    accept_th_ = std::thread(&WfdRtspServer::accept_loop, this);
    return true;
}

void WfdRtspServer::stop() {
    stopping_.store(true);
    running_.store(false);
    pending_cv_.notify_all();
    // Let the accept thread leave its bounded wait before closing its listener.
    if (accept_th_.joinable()) accept_th_.join();
    if (listen_sock_ != kInvalidSocket) {
        net::close_socket(listen_sock_);
        listen_sock_ = kInvalidSocket;
    }
    for (auto& t : client_ths_) {
        if (t.joinable()) t.join();
    }
    client_ths_.clear();
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        has_ctrl_ = false;
        ctrl_peer_ip_.clear();
        for (auto& [_, fd] : pending_conns_) net::close_socket(fd);
        pending_conns_.clear();
    }
    port_.store(0);
}

SocketHandle WfdRtspServer::claim_pending_port_socket(uint16_t peer_src_port) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    std::unique_lock<std::mutex> lock(pending_mu_);
    while (running_.load() && !stopping_.load()) {
        auto it = pending_conns_.find(peer_src_port);
        if (it != pending_conns_.end()) {
            SocketHandle fd = it->second;
            pending_conns_.erase(it);
            return fd;
        }
        if (pending_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            auto after = pending_conns_.find(peer_src_port);
            if (after != pending_conns_.end()) {
                SocketHandle fd = after->second;
                pending_conns_.erase(after);
                return fd;
            }
            break;
        }
    }
    return kInvalidSocket;
}

void WfdRtspServer::accept_loop() {
    while (running_.load()) {
        if (!net::wait_readable(listen_sock_, 50) || !running_.load()) continue;
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        SocketHandle client = accept(listen_sock_, reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (client == kInvalidSocket) continue;

        if (!net::set_nonblocking(client, true)) {
            net::close_socket(client);
            continue;
        }
        net::set_tcp_nodelay(client, true);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        uint16_t peer_port = ntohs(peer.sin_port);
        std::string peer_ip(ip);

        bool spawn_ctrl = false;
        SocketHandle to_close = kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(pending_mu_);
            if (!has_ctrl_) {
                has_ctrl_ = true;
                ctrl_peer_ip_ = peer_ip;
                spawn_ctrl = true;
            } else if (ctrl_peer_ip_ == peer_ip && pending_conns_.size() < kMaxPendingSockets) {
                auto [it, inserted] = pending_conns_.try_emplace(peer_port, client);
                if (!inserted) {
                    to_close = it->second;
                    it->second = client;
                }
                pending_cv_.notify_all();
            } else {
                to_close = client;
            }
        }
        if (to_close != kInvalidSocket) {
            net::close_socket(to_close);
        }
        if (spawn_ctrl) {
            client_ths_.emplace_back(&WfdRtspServer::handle_rtsp_control, this, client, std::move(peer_ip), peer_port);
        }
    }
    net::close_socket(listen_sock_);
    listen_sock_ = kInvalidSocket;
    port_.store(0);
}

void WfdRtspServer::handle_rtsp_control(SocketHandle ctrl_sock, const std::string& peer_ip, uint16_t peer_port) {
    auto ctrl_writer = std::make_shared<net::ConnectionWriter>(ctrl_sock, true);
    std::shared_ptr<net::ConnectionWriter> data_writer;
    auto send_str = [&](const std::string& message) {
        if (ctrl_writer->write(reinterpret_cast<const uint8_t*>(message.data()), message.size(), 120)
                == net::SendAllStatus::Ok) return true;
        ctrl_writer->abort();
        return false;
    };

    auto report = [this](const std::string& s) {
        if (cbs_.on_status) cbs_.on_status(device_id_, s);
    };

    report("RTSP 已反连 (" + peer_ip + ")，正在 M1 OPTIONS 握手...");

    // M1 OPTIONS 使用 CSeq: 1，兼容所有官方设备及第三方接收端
    bool peer_closed = !send_str(build_m1_options(sender_ip_, timer_port_, 1));

    std::string acc;
    char buf[4096];
    bool sent_m3 = false;
    bool sent_m4 = false;
    bool sent_m5 = false;
    bool sent_time_offset = false;
    bool setup_complete = false;
    bool streaming = false;
    uint16_t media_port = 0;
    SocketHandle claimed_data_sock = kInvalidSocket;
    const std::string session_id = "1189641421";

    while (running_.load() && !peer_closed) {
        if (!net::wait_readable(ctrl_sock, 50) || !running_.load()) continue;
        auto n = recv(ctrl_sock, buf, sizeof(buf), 0);
        if (n == 0) { peer_closed = true; break; }
        if (n < 0) {
            if (net::last_socket_error_is_timeout()) continue;
            peer_closed = true;
            break;
        }
        acc.append(buf, static_cast<size_t>(n));

        while (running_.load() && !peer_closed) {
            auto hdr_end = acc.find("\r\n\r\n");
            if (hdr_end == std::string::npos) break;
            std::string hdr = acc.substr(0, hdr_end + 4);
            size_t content_len = 0;
            std::string cl = extract_header(hdr, "Content-Length");
            if (!cl.empty()) {
                try { content_len = static_cast<size_t>(std::stoul(cl)); } catch (...) {}
            }
            auto message = take_rtsp_message(acc, hdr_end, content_len);
            if (!message) break;
            const std::string& msg = *message;
            std::string cseq = extract_header(msg, "CSeq");

            if (msg.rfind("OPTIONS ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nPublic: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n\r\n";
                if (!send_str(resp.str())) { peer_closed = true; break; }

                if (!sent_m3) {
                    sent_m3 = true;
                    report("RTSP M3 查询接收端音频能力...");
                    const std::string body = build_m3_get_parameter_body();
                    std::ostringstream m3;
                    m3 << "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 2\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << body.size() << "\r\n\r\n" << body;
                    if (!send_str(m3.str())) { peer_closed = true; break; }
                }
            } else if (msg.rfind("RTSP/1.0 200 OK", 0) == 0) {
                if (cseq == "2" && !sent_m4) {
                    sent_m4 = true;
                    bool has_v2 = (msg.find("wfd_audio_codecs_v2") != std::string::npos);
                    bool has_v1 = (msg.find("wfd_audio_codecs:") != std::string::npos);
                    report("RTSP M4 下发" + std::string(prefer_lpcm_ ? "LPCM" : "ADTS-AAC") +
                           " " + std::to_string(sample_rate_) + "Hz 参数...");

                    std::string b = build_m4_audio_body(prefer_lpcm_, sample_rate_, sender_ip_, has_v2, has_v1);
                    std::ostringstream m4;
                    m4 << "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 3\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << b.size() << "\r\n\r\n" << b;
                    if (!send_str(m4.str())) { peer_closed = true; break; }
                } else if (cseq == "3" && !sent_m5) {
                    sent_m5 = true;
                    report("RTSP M5 触发 SETUP...");
                    const std::string b = "wfd_trigger_method: SETUP\r\n";
                    std::ostringstream m5;
                    m5 << "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                       << "CSeq: 4\r\nContent-Type: text/parameters\r\n"
                       << "Content-Length: " << b.size() << "\r\n\r\n" << b;
                    if (!send_str(m5.str())) { peer_closed = true; break; }
                }
            } else if (msg.rfind("SETUP ", 0) == 0) {
                report("RTSP M6 SETUP 建立数据通道...");
                const std::string mp = extract_header(hdr, "MultiPort");
                const int requested_port = mp.empty() ? 0 : parse_multi_port_media_port(mp);
                if (requested_port < 0 || static_cast<uint16_t>(requested_port) == peer_port) {
                    if (!send_str("RTSP/1.0 461 Unsupported Transport\r\nCSeq: " + cseq + "\r\n\r\n")) { peer_closed = true; break; }
                    report("RTSP MultiPort 无效，未建立音频通道");
                    continue;
                }
                if (streaming && static_cast<uint16_t>(requested_port) != media_port) {
                    if (!send_str("RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: " + cseq + "\r\n\r\n")) { peer_closed = true; break; }
                    continue;
                }
                media_port = static_cast<uint16_t>(requested_port);
                setup_complete = true;
                if (media_port > 0 && !sent_time_offset) {
                    // 官方小米接收端（含 MultiPort）在 SETUP 响应前期望 TIME_OFFSET
                    if (!send_str(build_time_offset_req(time_offset_us_, 5))) { peer_closed = true; break; }
                    sent_time_offset = true;
                }

                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=60"
                     << "\r\nTransport: RTP/AVP/TCP;interleaved=0-1;\r\n\r\n";
                if (!send_str(resp.str())) { peer_closed = true; break; }
            } else if (msg.rfind("PLAY ", 0) == 0) {
                if (!setup_complete) {
                    if (!send_str("RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: " + cseq + "\r\n\r\n")) { peer_closed = true; break; }
                    continue;
                }
                if (!streaming && media_port > 0) {
                    claimed_data_sock = claim_pending_port_socket(media_port);
                    if (claimed_data_sock != kInvalidSocket) {
                        data_writer = std::make_shared<net::ConnectionWriter>(claimed_data_sock, true);
                    }
                    if (!running_.load() || stopping_.load()) break;
                    if (claimed_data_sock == kInvalidSocket) {
                        if (!send_str("RTSP/1.0 503 Service Unavailable\r\nCSeq: " + cseq + "\r\n\r\n")) { peer_closed = true; break; }
                        report("RTSP MultiPort 音频反连超时，接收端端口 " + std::to_string(media_port));
                        peer_closed = true;
                        break;
                    }
                }

                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=60"
                     << "\r\nRange: npt=now-\r\n\r\n";
                if (!send_str(resp.str())) { peer_closed = true; break; }

                if (!sent_time_offset) {
                    if (!send_str(build_time_offset_req(time_offset_us_, 5))) { peer_closed = true; break; }
                    sent_time_offset = true;
                }

                if (!streaming) {
                    streaming = true;
                    const bool is_multi = (claimed_data_sock != kInvalidSocket);
                    report(is_multi
                               ? ("RTSP M7 同步推流中 (MultiPort，控制 " + std::to_string(peer_port) +
                                  "，音频 " + std::to_string(media_port) + ")")
                               : "RTSP M7 同步推流中 (TCP Interleaved，接收端未请求 MultiPort)");
                    SocketHandle out_sock = is_multi ? claimed_data_sock : ctrl_sock;
                    if (cbs_.on_stream_connection) {
                        cbs_.on_stream_connection(device_id_, peer_ip, is_multi ? data_writer : ctrl_writer,
                            [ctrl_writer] { ctrl_writer->abort(); });
                    } else if (cbs_.on_stream_ready) {
                        cbs_.on_stream_ready(device_id_, peer_ip, out_sock);
                    }
                }
            } else if (msg.rfind("GET_PARAMETER ", 0) == 0) {
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq
                     << "\r\nSession: " << session_id << ";timeout=20\r\n\r\n";
                if (!send_str(resp.str())) { peer_closed = true; break; }
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
                if (streaming && cbs_.on_stop_media) cbs_.on_stop_media(device_id_);
                std::ostringstream resp;
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n\r\n";
                if (!send_str(resp.str())) { peer_closed = true; break; }
                peer_closed = true;
                break;
            }
        }
    }

    if (streaming && cbs_.on_stop_media) cbs_.on_stop_media(device_id_);
    running_.store(false);
    pending_cv_.notify_all();
    if (stopping_.load() && !peer_closed) {
        const std::string body = "wfd_trigger_method: TEARDOWN\r\n";
        std::ostringstream request;
        request << "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                << "CSeq: 99\r\nContent-Type: text/parameters\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        if (send_str(request.str())) {
            // Give the sink time to finish the WFD exchange before closing TCP.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(75);
            while (std::chrono::steady_clock::now() < deadline) {
                if (const auto start = acc.find("TEARDOWN "); start != std::string::npos &&
                    acc.find("\r\n\r\n", start) != std::string::npos) {
                    const std::string cseq = extract_header(acc.substr(start), "CSeq");
                    send_str("RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n\r\n");
                    break;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0 || !net::wait_readable(ctrl_sock, static_cast<int>(remaining))) break;
                const auto n = recv(ctrl_sock, buf, sizeof(buf), 0);
                if (n <= 0 || acc.size() > 16384) break;
                acc.append(buf, static_cast<size_t>(n));
            }
        }
    } else if (!stopping_.load()) {
        // Stop the RTP writer before closing the sockets it borrows.
        report("RTSP 会话已关闭");
        if (cbs_.on_session_closed) cbs_.on_session_closed(device_id_);
    }
    if (data_writer) data_writer->abort();
    ctrl_writer->abort();
    std::vector<SocketHandle> extra_conns;
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        for (auto& [_, fd] : pending_conns_) extra_conns.push_back(fd);
        pending_conns_.clear();
    }
    for (SocketHandle fd : extra_conns) {
        net::close_socket(fd);
    }
}

} // namespace miplay::rtsp
