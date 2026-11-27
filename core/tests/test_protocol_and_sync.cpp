#include "cmd/cmd_codec.h"
#include "crypto/mini_crypto.h"
#include "discovery/mdns_discovery.h"
#include "discovery/probe_burst.h"
#include "engine/multiroom_engine.h"
#include "media/aac_encoder.h"
#include "media/aac_timeline.h"
#include "media/audio_pipeline.h"
#include "media/rtp_sender.h"
#include "media/ts_packetizer.h"
#include "platform/net_platform.h"
#include "platform/deadline_write.h"
#include "rtsp/wfd_rtsp_server.h"
#include "sidecar/ws_server.h"
#include "sync/master_clock.h"
#include "sync/timer_service.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace miplay;

// SS-15: 常驻检查宏，Debug 与 Release (-DNDEBUG) 均严格执行，绝不把动作写进 assert()
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " CHECK(" #cond ")\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

std::pair<SocketHandle, SocketHandle> make_socket_pair() {
    SocketHandle lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd != kInvalidSocket);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    a.sin_port = 0;
    int br = bind(lfd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    CHECK(br == 0);
    int lr = listen(lfd, 2);
    CHECK(lr == 0);
    socklen_t alen = sizeof(a);
    getsockname(lfd, reinterpret_cast<struct sockaddr*>(&a), &alen);
    SocketHandle cfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cfd != kInvalidSocket);
    int cr = connect(cfd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    CHECK(cr == 0);
    SocketHandle sfd = accept(lfd, nullptr, nullptr);
    CHECK(sfd != kInvalidSocket);
    net::close_socket(lfd);
    return {sfd, cfd};
}

void receive_exact(SocketHandle fd, uint8_t* buffer, size_t len) {
    while (len) {
        auto n = recv(fd, reinterpret_cast<char*>(buffer), static_cast<int>(len), 0);
        CHECK(n > 0);
        buffer += n;
        len -= static_cast<size_t>(n);
    }
}

void read_access_unit(SocketHandle fd, std::vector<uint8_t>& wire) {
    bool marker = false;
    while (!marker) {
        uint8_t header[4]{};
        receive_exact(fd, header, 4);
        CHECK(header[0] == '$' && header[1] == 0);
        const size_t size = (header[2] << 8) | header[3];
        CHECK(size >= 12 && size <= 12 + 7 * 188);
        wire.insert(wire.end(), header, header + 4);
        size_t old_size = wire.size();
        wire.resize(old_size + size);
        receive_exact(fd, wire.data() + old_size, size);
        marker = (wire[old_size + 1] & 0x80) != 0;
    }
}

void write_u16_le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void write_u32_le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

std::vector<uint8_t> build_wav_bytes(uint16_t fmt_tag, uint16_t ch, uint32_t rate,
                                     uint16_t bits, const std::vector<uint8_t>& pcm_data,
                                     bool include_odd_junk_chunk = false) {
    std::vector<uint8_t> body;
    body.insert(body.end(), {'W', 'A', 'V', 'E'});
    if (include_odd_junk_chunk) {
        body.insert(body.end(), {'J', 'U', 'N', 'K'});
        write_u32_le(body, 3); // 奇数长度 3 字节 + 1 字节 RIFF 对齐填充
        body.push_back(0x11);
        body.push_back(0x22);
        body.push_back(0x33);
        body.push_back(0x00); // padding byte
    }
    body.insert(body.end(), {'f', 'm', 't', ' '});
    write_u32_le(body, 16);
    write_u16_le(body, fmt_tag);
    write_u16_le(body, ch);
    write_u32_le(body, rate);
    uint16_t block_align = static_cast<uint16_t>(ch * (bits / 8));
    write_u32_le(body, rate * block_align);
    write_u16_le(body, block_align);
    write_u16_le(body, bits);

    body.insert(body.end(), {'d', 'a', 't', 'a'});
    write_u32_le(body, static_cast<uint32_t>(pcm_data.size()));
    body.insert(body.end(), pcm_data.begin(), pcm_data.end());

    std::vector<uint8_t> riff;
    riff.insert(riff.end(), {'R', 'I', 'F', 'F'});
    write_u32_le(riff, static_cast<uint32_t>(body.size()));
    riff.insert(riff.end(), body.begin(), body.end());
    return riff;
}

std::string write_temp_file(const std::string& name, const std::vector<uint8_t>& bytes) {
    const auto unique = std::to_string(sync::monotonic_now_us()) + "-" + name;
    const std::string p = (std::filesystem::temp_directory_path() / unique).string();
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(ofs));
    ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    return p;
}

} // namespace

static void test_cmd8899_codec_and_safety() {
    cmd::CmdFrame f;
    f.cmd_type = static_cast<uint16_t>(cmd::CmdType::HeartBeat);
    f.seq = 0x0870;
    f.payload = {1, 2, 3, 4};

    auto wire = cmd::encode_frame(f);
    CHECK(wire.size() == 13);
    CHECK(wire[0] == 0x24);
    CHECK(wire[1] == 0x00 && wire[2] == 0x1A);
    CHECK(wire[3] == 0x08 && wire[4] == 0x70);
    CHECK(wire[5] == 0 && wire[6] == 0 && wire[7] == 0 && wire[8] == 4);

    std::vector<cmd::CmdFrame> decoded;
    size_t consumed = cmd::decode_frames(wire.data(), wire.size(), decoded);
    CHECK(consumed == wire.size());
    CHECK(decoded.size() == 1);
    CHECK(decoded[0].cmd_type == 0x001A && decoded[0].seq == 0x0870 && decoded[0].payload == f.payload);

    std::string ch = "19936257588200";
    auto ch_resp = cmd::compute_legacy_challenge_response(std::vector<uint8_t>(ch.begin(), ch.end()));
    CHECK(ch_resp.size() == 40);

    std::string akey = cmd::derive_type1_auth_key("192.168.5.87", 8899, "192.168.5.84", 51326);
    CHECK(akey.size() == 32);

    auto tlv = cmd::encode_tlv_string("cmd", "{\"aesKeyTypes\":\"1\"}");
    std::string k;
    auto val = cmd::decode_tlv_string(tlv, &k);
    CHECK(val.has_value() && k == "cmd" && *val == "{\"aesKeyTypes\":\"1\"}");

    cmd::SafetyCipher enc, dec;
    const uint8_t* key16 = reinterpret_cast<const uint8_t*>(akey.data());
    const uint8_t* iv16 = reinterpret_cast<const uint8_t*>(akey.data() + 16);
    enc.init(key16, iv16);
    dec.init(key16, iv16);

    auto enc_empty = enc.encrypt_blob({});
    CHECK(enc_empty.size() == 25);
    CHECK(enc_empty[0] == 0x00 && enc_empty[1] == 0x07 && enc_empty[2] == 0x01 && enc_empty[3] == 0xE0);
    CHECK(enc_empty[4] == 0x10);
    auto dec_empty = dec.decrypt_blob(enc_empty);
    CHECK(dec_empty.has_value() && dec_empty->empty());

    std::vector<uint8_t> msg = {'w','f','d',':','/','/','1','9','2','.','1','6','8','.','5','.','8','4',':','4','0','0','0','0',0};
    auto enc_msg = enc.encrypt_blob(msg);
    auto dec_msg = dec.decrypt_blob(enc_msg);
    CHECK(dec_msg.has_value() && *dec_msg == msg);

    uint8_t dummy_aac[10] = {1,2,3,4,5,6,7,8,9,10};
    auto adts = media::AacEncoder::wrap_adts(dummy_aac, sizeof(dummy_aac), 48000, 2);
    CHECK(adts.size() == 17 && adts[0] == 0xFF && adts[1] == 0xF9 && adts[2] == 0x4C);

    std::cout << "[PASS] test_cmd8899_codec_and_safety\n";
}

static void test_master_clock_and_trendline() {
    sync::MasterMediaClock clk;
    clk.reset(48000, 350);
    auto f0 = clk.next_frame_timestamps(1024);
    auto f1 = clk.next_frame_timestamps(1024);
    CHECK(f1.rtp_ts_90khz - f0.rtp_ts_90khz == 1920);
    CHECK(f1.pts_90khz - f0.pts_90khz == 1920);

    sync::TrendlineEstimator est(16);
    sync::TrendlineEstimator::Estimate last{};
    for (int i = 0; i < 16; ++i) {
        uint64_t t_us = 1000000ULL * (i + 1);
        bool spike = (i % 4 == 3);
        double true_off = 5000.0 + 20.0 * (i + 1);
        double rtt = spike ? 50000.0 : 1200.0;
        double measured = spike ? (true_off + 25000.0) : true_off;
        last = est.update(t_us, measured, rtt);
    }
    CHECK(std::abs(last.offset_us - (5000.0 + 20.0 * 16.0)) < 100.0);
    CHECK(std::abs(last.drift_ppm - 20.0) < 2.0);
    std::cout << "[PASS] test_master_clock_and_trendline\n";
}

static void test_timer_service_multi_clients() {
    net::init_network();
    sync::TimerService srv;
    bool started = srv.start("127.0.0.1", 0);
    CHECK(started);
    uint16_t port = srv.port();
    CHECK(port > 0);

    for (int dev = 0; dev < 2; ++dev) {
        SocketHandle cli = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(cli != kInvalidSocket);
        CHECK(net::set_socket_timeout(cli, SO_RCVTIMEO, 1000));

        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

        sync::MiPlayTimerPacket req{};
        req.client_synced_us = sync::monotonic_now_us();
        req.client_local_us = 12345678ULL + dev;
        req.seq = 100 + dev;

        sendto(cli, reinterpret_cast<const char*>(&req), sizeof(req), 0,
               reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));

        sync::MiPlayTimerPacket resp{};
        auto n = recv(cli, reinterpret_cast<char*>(&resp), sizeof(resp), 0);
        CHECK(n == 40);
        CHECK(resp.client_synced_us == req.client_synced_us);
        CHECK(resp.client_local_us == req.client_local_us);
        CHECK(resp.seq == req.seq);
        CHECK(resp.server_recv_us > 0 && resp.server_send_us >= resp.server_recv_us);
        net::close_socket(cli);
    }
    srv.stop();
    auto tele = srv.snapshot_telemetry();
    CHECK(tele.count("127.0.0.1") == 1 && tele["127.0.0.1"].packet_count == 2);
    std::cout << "[PASS] test_timer_service_multi_clients\n";
}

// SS-02 & SS-09: WAV 奇数数据长度、截断、超长声明、奇数未知块填充、单声道/24位/44.1kHz 及失败保留原状态
static void test_wav_parser_bounds_and_formats() {
    std::vector<int16_t> out;
    media::AudioTrackMeta meta;
    std::string err;

    // 1. 合法 16-bit 单声道 44.1kHz + 奇数 JUNK 块 -> 重采样到 48kHz 立体声
    std::vector<uint8_t> mono16(441 * 2, 0);
    for (size_t i = 0; i < 441; ++i) {
        int16_t s = 10000;
        mono16[i * 2] = static_cast<uint8_t>(s & 0xFF);
        mono16[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
    }
    std::string p_mono = write_temp_file("miplay_test_mono441.wav",
                                         build_wav_bytes(1, 1, 44100, 16, mono16, true));
    bool ok = media::AudioPipeline::decode_wav_file(p_mono, 48000, out, meta, err);
    CHECK(ok);
    CHECK(out.size() == 480 * 2);
    CHECK(out[10] == 10000 && out[11] == 10000);

    // 2. 合法 24-bit 立体声 48kHz
    std::vector<uint8_t> pcm24(16 * 6, 0);
    for (size_t i = 0; i < 16; ++i) {
        // +0x123400 -> i16 = 0x1234
        pcm24[i * 6 + 0] = 0x00; pcm24[i * 6 + 1] = 0x34; pcm24[i * 6 + 2] = 0x12;
        pcm24[i * 6 + 3] = 0x00; pcm24[i * 6 + 4] = 0x34; pcm24[i * 6 + 5] = 0x12;
    }
    std::string p_24 = write_temp_file("miplay_test_24bit.wav", build_wav_bytes(1, 2, 48000, 24, pcm24));
    ok = media::AudioPipeline::decode_wav_file(p_24, 48000, out, meta, err);
    CHECK(ok);
    CHECK(out.size() == 16 * 2 && out[0] == 0x1234 && out[1] == 0x1234);

    // 3. SS-02: 奇数 data 长度 (15 字节，非 4 字节对齐) -> 必须拒绝且不越界
    std::vector<uint8_t> odd_data(15, 0x55);
    std::string p_odd = write_temp_file("miplay_test_odd.wav", build_wav_bytes(1, 2, 48000, 16, odd_data));
    ok = media::AudioPipeline::decode_wav_file(p_odd, 48000, out, meta, err);
    CHECK(!ok && !err.empty());

    // 4. SS-02: 超长 data_size 声明（文件被截断） -> 必须拒绝
    auto trunc = build_wav_bytes(1, 2, 48000, 16, std::vector<uint8_t>(64, 1));
    trunc.resize(trunc.size() - 32);
    std::string p_trunc = write_temp_file("miplay_test_trunc.wav", trunc);
    ok = media::AudioPipeline::decode_wav_file(p_trunc, 48000, out, meta, err);
    CHECK(!ok && !err.empty());

    // 5. SS-09: 加载坏文件时必须保留上一个成功曲目状态
    media::AudioPipeline pipe;
    CHECK(pipe.set_source_mode(media::AudioSourceMode::WavFile, p_mono));
    std::string prev_title = pipe.current_meta().title;
    std::string fail_err;
    bool bad_ok = pipe.set_source_mode(media::AudioSourceMode::WavFile, p_trunc, &fail_err);
    CHECK(!bad_ok && !fail_err.empty());
    CHECK(pipe.source_mode() == media::AudioSourceMode::WavFile);
    CHECK(pipe.current_meta().title == prev_title);

    std::remove(p_mono.c_str());
    std::remove(p_24.c_str());
    std::remove(p_odd.c_str());
    std::remove(p_trunc.c_str());
    std::cout << "[PASS] test_wav_parser_bounds_and_formats (SS-02, SS-09)\n";
}

// SS-03 & SS-04: 每设备独立有界队列隔离慢接收端 + 按全局 session_id 防止旧清理误删新目标
static void test_rtp_slow_receiver_isolation_and_session_id() {
    media::MultiAudioSender sender;
    sender.set_stream_type(media::TsAudioStreamType::WfdLpcm);

    auto [s_fast, r_fast] = make_socket_pair();
    auto [s_slow, r_slow] = make_socket_pair();

    // 缩小慢接收端 socket 缓冲，使其很快积压阻塞
    int tiny_buf = 4096;
    setsockopt(s_slow, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&tiny_buf), sizeof(tiny_buf));
    setsockopt(r_slow, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&tiny_buf), sizeof(tiny_buf));

    sender.add_or_update_target("fast_room", "127.0.0.1", s_fast, 0, true, 101);
    sender.add_or_update_target("slow_room", "127.0.0.1", s_slow, 0, true, 102);

    // SS-03: 过期 session_id (100) 既不能覆盖也不能删除 session_id (101)
    sender.remove_target("fast_room", 100);
    CHECK(sender.active_target_count() == 2);

    std::atomic<size_t> fast_bytes{0};
    std::atomic<bool> reader_stop{false};
    std::thread fast_reader([&]() {
        uint8_t buf[4096];
        CHECK(net::set_socket_timeout(r_fast, SO_RCVTIMEO, 100));
        while (!reader_stop.load()) {
            auto n = recv(r_fast, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n > 0) fast_bytes.fetch_add(static_cast<size_t>(n));
            else if (n == 0) break;
        }
    });

    // 慢接收端完全不读取 r_slow，连续推送 80 帧，验证推流线程不被阻塞且快接收端持续收包
    std::vector<int16_t> pcm(480 * 2, 1234);
    auto lpcm = media::TsPacketizer::wrap_wfd_lpcm(pcm.data(), 480);
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 80; ++i) {
        sender.broadcast_audio_frame(lpcm.data(), lpcm.size(), 90000ULL + i * 900ULL, static_cast<uint32_t>(90000ULL + i * 900ULL));
    }
    auto enqueue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(enqueue_ms < 500);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(fast_bytes.load() > 10000);

    // 停机必须在有界时间内返回（即使慢接收端阻塞中）
    auto t_stop = std::chrono::steady_clock::now();
    sender.clear_all();
    auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_stop).count();
    CHECK(stop_ms < 1500);

    reader_stop.store(true);
    net::shutdown_socket(r_fast);
    fast_reader.join();
    net::close_socket(r_fast);
    net::close_socket(r_slow);
    std::cout << "[PASS] test_rtp_slow_receiver_isolation_and_session_id (SS-03, SS-04, stop=" << stop_ms << "ms)\n";
}

// SS-06 & SS-16: RTSP 监听失败绝不发送 Open + 采样率/M4 能力掩码严格一致
static void test_rtsp_bind_failure_and_audio_config() {
    // SS-16: 校验采样率与 M4 一致性
    AudioStreamConfig cfg{};
    std::string err;
    CHECK(MultiRoomEngine::validate_audio_config(false, 48000, cfg, err));
    CHECK(!cfg.lpcm && cfg.sample_rate == 48000 && cfg.frame_samples == 1024);
    CHECK(!MultiRoomEngine::validate_audio_config(false, 44100, cfg, err)); // AAC 拒绝 44.1kHz
    CHECK(MultiRoomEngine::validate_audio_config(true, 44100, cfg, err));
    CHECK(cfg.lpcm && cfg.sample_rate == 44100 && cfg.frame_samples == 441);
    CHECK(!MultiRoomEngine::validate_audio_config(true, 12345, cfg, err)); // 拒绝非法采样率

    std::string m4_lpcm44 = rtsp::WfdRtspServer::build_m4_audio_body(true, 44100, "192.168.5.84", true, true);
    CHECK(m4_lpcm44.find("wfd_audio_codecs_v2: 2 0") != std::string::npos);
    CHECK(m4_lpcm44.find("LPCM 00000001 00") != std::string::npos);
    std::string m4_lpcm48 = rtsp::WfdRtspServer::build_m4_audio_body(true, 48000, "192.168.5.84", true, true);
    CHECK(m4_lpcm48.find("wfd_audio_codecs_v2: 2 1") != std::string::npos);
    CHECK(m4_lpcm48.find("LPCM 00000002 00") != std::string::npos);

    // SS-06: 预占一个端口并注入 RTSP 监听，断言不会向 mock 8899 发送任何连接/Open
    SocketHandle blocker = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in baddr{};
    baddr.sin_family = AF_INET;
    baddr.sin_addr.s_addr = INADDR_ANY;
    baddr.sin_port = 0;
    CHECK(bind(blocker, reinterpret_cast<struct sockaddr*>(&baddr), sizeof(baddr)) == 0);
    CHECK(listen(blocker, 1) == 0);
    socklen_t blen = sizeof(baddr);
    getsockname(blocker, reinterpret_cast<struct sockaddr*>(&baddr), &blen);
    uint16_t busy_port = ntohs(baddr.sin_port);

    SocketHandle cmd_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in caddr{};
    caddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &caddr.sin_addr);
    caddr.sin_port = 0;
    CHECK(bind(cmd_listen, reinterpret_cast<struct sockaddr*>(&caddr), sizeof(caddr)) == 0);
    CHECK(listen(cmd_listen, 2) == 0);
    socklen_t clen = sizeof(caddr);
    getsockname(cmd_listen, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
    uint16_t cmd_port = ntohs(caddr.sin_port);

    std::atomic<bool> got_any_cmd_conn{false};
    std::thread spy([&]() {
        if (!net::wait_readable(cmd_listen, 350)) return;
        SocketHandle cli = accept(cmd_listen, nullptr, nullptr);
        if (cli != kInvalidSocket) {
            got_any_cmd_conn.store(true);
            net::close_socket(cli);
        }
    });

    MultiRoomEngine eng("127.0.0.1");
    eng.set_rtsp_bind_port_for_test(busy_port);
    std::string dev_id = eng.add_manual_device("BusyRtspDev", "127.0.0.1", cmd_port);
    CHECK(eng.add_device_to_group(dev_id));
    CHECK(eng.start_multiroom(true, 48000, 350));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    eng.stop_multiroom();
    spy.join();
    net::close_socket(cmd_listen);
    net::close_socket(blocker);

    CHECK(!got_any_cmd_conn.load());
    std::cout << "[PASS] test_rtsp_bind_failure_and_audio_config (SS-06, SS-16)\n";
}

// SS-01 & SS-05: 延迟握手期间立刻停止、删设备、并发启停及立刻析构引擎（ASan/TSan 零 UAF/死锁）
static void test_lifecycle_cancellation_and_concurrent_start_stop() {
    SocketHandle slow_cmd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    CHECK(bind(slow_cmd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(slow_cmd, 16) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(slow_cmd, reinterpret_cast<struct sockaddr*>(&addr), &alen);
    uint16_t slow_port = ntohs(addr.sin_port);

    // Mock 服务器接受连接但故意不回复 0x0028 挑战，让握手卡在等待阶段
    std::atomic<bool> stop_srv{false};
    std::thread srv([&]() {
        std::vector<SocketHandle> held;
        while (!stop_srv.load()) {
            if (!net::wait_readable(slow_cmd, 100)) continue;
            SocketHandle c = accept(slow_cmd, nullptr, nullptr);
            if (c != kInvalidSocket) held.push_back(c);
        }
        for (SocketHandle c : held) net::close_socket(c);
    });

    // 1. 延迟握手期间立即删设备并析构引擎
    {
        MultiRoomEngine eng("127.0.0.1");
        std::string id = eng.add_manual_device("SlowHandshake", "127.0.0.1", slow_port);
        CHECK(eng.add_device_to_group(id));
        CHECK(eng.start_multiroom(true, 48000, 350));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(eng.remove_device(id));
        // 重新加同 ID 后直接析构，验证无 detached 线程访问已销毁 this
        std::string id2 = eng.add_manual_device("SlowHandshake", "127.0.0.1", slow_port);
        CHECK(eng.add_device_to_group(id2));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    } // ~MultiRoomEngine() 必须干净等待全部连接线程退出

    // 2. SS-05: 并发启停压测
    {
        MultiRoomEngine eng("127.0.0.1");
        std::string id = eng.add_manual_device("ConcurrentDev", "127.0.0.1", slow_port);
        CHECK(eng.add_device_to_group(id));
        std::thread t1([&]() {
            for (int i = 0; i < 4; ++i) {
                eng.start_multiroom(true, 48000, 350);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        std::thread t2([&]() {
            for (int i = 0; i < 4; ++i) {
                eng.stop_multiroom();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        t1.join();
        t2.join();
        eng.stop_multiroom();
    }

    stop_srv.store(true);
    srv.join();
    net::close_socket(slow_cmd);
    std::cout << "[PASS] test_lifecycle_cancellation_and_concurrent_start_stop (SS-01, SS-05)\n";
}

#if defined(__linux__)
// SS-10: Linux 无输出采集源（模拟卡死的 parec）停止与切换必须在有界时间内返回
static void test_linux_stalled_loopback_cancellation() {
    media::AudioPipeline pipe;
    pipe.set_loopback_command_override_for_test({"/bin/sleep", "30"});
    CHECK(pipe.set_source_mode(media::AudioSourceMode::SystemLoopback, ""));
    CHECK(pipe.start(48000, 480, [](const int16_t*, size_t) {}));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    auto t0 = std::chrono::steady_clock::now();
    pipe.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(elapsed_ms < 600);
    std::cout << "[PASS] test_linux_stalled_loopback_cancellation (SS-10, stop=" << elapsed_ms << "ms)\n";
}
#endif

static void test_slow_spectrum_consumer_keeps_capture_running() {
    media::AudioPipeline pipeline;
    std::atomic<unsigned> frames{0};
    std::atomic<bool> spectrum_entered{false};
    std::atomic<bool> release_spectrum{false};
    std::atomic<bool> first_spectrum_finished{false};
    std::atomic<unsigned> spectra{0};

    CHECK(pipeline.start(48000, 1024,
        [&](const int16_t*, size_t) { ++frames; },
        [&](const std::array<float, 16>& bands) {
            ++spectra;
            if (spectrum_entered.exchange(true)) return;
            // The built-in 440/554/659 Hz tones must peak in the corresponding bands.
            const float tone = std::max({bands[4], bands[5], bands[6]});
            CHECK(tone > 0.10f && bands[0] < tone * 0.2f && bands[15] < tone * 0.2f);
            while (!release_spectrum.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            first_spectrum_finished.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!spectrum_entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(spectrum_entered.load());
    const unsigned before = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(240));
    const unsigned frames_during_block = frames.load() - before;
    release_spectrum.store(true);

    const auto next_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (spectra.load() < 2 && std::chrono::steady_clock::now() < next_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pipeline.stop();
    CHECK(first_spectrum_finished.load());
    CHECK(spectra.load() >= 2);
    CHECK(frames_during_block >= 8);
    std::cout << "[PASS] test_slow_spectrum_consumer_keeps_capture_running ("
              << frames_during_block << " PCM frames while spectrum was blocked)\n";
}

static void test_ws_shutdown_with_idle_client() {
    sidecar::LocalWsServer ws;
    CHECK(ws.start(0, "test-token", {}));
    const auto port = ws.port();
    CHECK(port > 0);
    SocketHandle client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client != kInvalidSocket);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(client, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const auto start = std::chrono::steady_clock::now();
    ws.stop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    CHECK(ws.port() == 0);
    net::close_socket(client);
    CHECK(ws.start(0, "next-token", {}));
    ws.stop();
    std::cout << "[PASS] test_ws_shutdown_with_idle_client\n";
}

static void test_disconnect_no_deadlock_and_teardown(bool stop_all) {
    net::init_network();

    SocketHandle cmd_listen = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cmd_listen != kInvalidSocket);
    net::set_reuse_addr(cmd_listen);
    struct sockaddr_in caddr{};
    caddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &caddr.sin_addr);
    caddr.sin_port = 0;
    CHECK(bind(cmd_listen, reinterpret_cast<struct sockaddr*>(&caddr), sizeof(caddr)) == 0);
    CHECK(listen(cmd_listen, 4) == 0);
    socklen_t clen = sizeof(caddr);
    getsockname(cmd_listen, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
    uint16_t mock_cmd_port = ntohs(caddr.sin_port);

    std::atomic<bool> got_close_cmd{false};
    std::atomic<bool> got_rtsp_teardown{false};
    std::atomic<bool> rtsp_streaming_reached{false};
    std::atomic<bool> mock_stop{false};

    std::thread mock_receiver([&]() {
        if (!net::wait_readable(cmd_listen, 3000)) return;
        SocketHandle cli = accept(cmd_listen, nullptr, nullptr);
        if (cli == kInvalidSocket) return;
        CHECK(net::set_socket_timeout(cli, SO_RCVTIMEO, 3000));

        cmd::CmdFrame ch;
        ch.cmd_type = static_cast<uint16_t>(cmd::CmdType::Challenge);
        ch.seq = 1;
        ch.payload = {'1','2','3','4','5','6'};
        auto wire = cmd::encode_frame(ch);
        net::send_all(cli, wire.data(), wire.size());

        std::vector<uint8_t> acc;
        uint8_t buf[2048];
        std::thread rtsp_cli_th;
        while (!mock_stop.load()) {
            auto n = recv(cli, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            acc.insert(acc.end(), buf, buf + n);
            std::vector<cmd::CmdFrame> frames;
            size_t consumed = cmd::decode_frames(acc.data(), acc.size(), frames);
            if (consumed > 0) acc.erase(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(consumed));
            for (const auto& f : frames) {
                if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::SafetyInfo)) {
                    cmd::CmdFrame ack;
                    ack.cmd_type = static_cast<uint16_t>(cmd::CmdType::SafetyInfoAck);
                    ack.seq = f.seq;
                    ack.payload = cmd::encode_tlv_string("ack", "{\"result\":\"1\"}");
                    auto aw = cmd::encode_frame(ack);
                    net::send_all(cli, aw.data(), aw.size());
                } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::Open)) {
                    std::string url(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
                    auto colon = url.rfind(':');
                    auto q = url.find('?');
                    uint16_t rport = static_cast<uint16_t>(std::stoi(url.substr(colon + 1, q - colon - 1)));
                    rtsp_cli_th = std::thread([rport, &got_rtsp_teardown, &rtsp_streaming_reached, &mock_stop]() {
                        SocketHandle rs = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ra{};
                        ra.sin_family = AF_INET;
                        inet_pton(AF_INET, "127.0.0.1", &ra.sin_addr);
                        ra.sin_port = htons(rport);
                        if (connect(rs, reinterpret_cast<struct sockaddr*>(&ra), sizeof(ra)) != 0) {
                            net::close_socket(rs);
                            return;
                        }
                        CHECK(net::set_socket_timeout(rs, SO_RCVTIMEO, 2000));
                        char rbuf[4096];
                        recv(rs, rbuf, sizeof(rbuf), 0);
                        std::string m2 = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
                        net::send_all(rs, reinterpret_cast<const uint8_t*>(m2.data()), m2.size());
                        recv(rs, rbuf, sizeof(rbuf), 0);
                        std::string m3ok = "RTSP/1.0 200 OK\r\nCSeq: 2\r\nwfd_audio_codecs: LPCM 00000002 00\r\n\r\n";
                        net::send_all(rs, reinterpret_cast<const uint8_t*>(m3ok.data()), m3ok.size());
                        recv(rs, rbuf, sizeof(rbuf), 0);
                        std::string m4ok = "RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n";
                        net::send_all(rs, reinterpret_cast<const uint8_t*>(m4ok.data()), m4ok.size());
                        recv(rs, rbuf, sizeof(rbuf), 0);
                        std::string setup = "SETUP rtsp://localhost/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 2\r\n\r\n";
                        net::send_all(rs, reinterpret_cast<const uint8_t*>(setup.data()), setup.size());
                        recv(rs, rbuf, sizeof(rbuf), 0);
                        std::string play = "PLAY rtsp://localhost/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 3\r\n\r\n";
                        net::send_all(rs, reinterpret_cast<const uint8_t*>(play.data()), play.size());
                        rtsp_streaming_reached.store(true);
                        std::string received;
                        while (!mock_stop.load()) {
                            auto rn = recv(rs, rbuf, sizeof(rbuf), 0);
                            if (rn <= 0) break;
                            received.append(rbuf, static_cast<size_t>(rn));
                            if (received.find("wfd_trigger_method: TEARDOWN") != std::string::npos) {
                                const std::string teardown =
                                    "TEARDOWN rtsp://localhost/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 12\r\n\r\n";
                                CHECK(net::send_all(rs, reinterpret_cast<const uint8_t*>(teardown.data()), teardown.size()));
                                received.clear();
                                while ((rn = recv(rs, rbuf, sizeof(rbuf), 0)) > 0) {
                                    received.append(rbuf, static_cast<size_t>(rn));
                                    if (received.find("RTSP/1.0 200 OK\r\nCSeq: 12") != std::string::npos) {
                                        got_rtsp_teardown.store(true);
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        net::close_socket(rs);
                    });
                } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::Close)) {
                    got_close_cmd.store(true);
                }
            }
        }
        if (rtsp_cli_th.joinable()) rtsp_cli_th.join();
        net::close_socket(cli);
    });

    MultiRoomEngine eng("127.0.0.1");
    std::string dev_id = eng.add_manual_device("MockSpeaker", "127.0.0.1", mock_cmd_port);
    CHECK(eng.add_device_to_group(dev_id));
    CHECK(eng.start_multiroom(true, 48000, 350));

    for (int i = 0; i < 60 && !rtsp_streaming_reached.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(rtsp_streaming_reached.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    auto t0 = std::chrono::steady_clock::now();
    if (stop_all) {
        eng.stop_multiroom();
    } else {
        CHECK(eng.remove_device_from_group(dev_id));
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(elapsed_ms < 500);

    auto snap = eng.snapshot_state();
    CHECK(snap.devices.size() == 1);
    CHECK(snap.devices[0].in_group == stop_all);
    CHECK(!snap.devices[0].cmd_connected);
    CHECK(!snap.devices[0].rtsp_streaming);

    eng.stop_multiroom();
    for (int i = 0; i < 20 && (!got_close_cmd.load() || !got_rtsp_teardown.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    mock_stop.store(true);
    net::close_socket(cmd_listen);
    if (mock_receiver.joinable()) mock_receiver.join();

    CHECK(got_close_cmd.load());
    CHECK(got_rtsp_teardown.load());
    std::cout << "[PASS] test_disconnect_no_deadlock_and_teardown ("
              << (stop_all ? "stop" : "remove") << ", " << elapsed_ms << "ms)\n";
}

// SS-31: C++ RTSP M3 官方能力查询与 MultiPort 双 TCP 协商/回退/超时/取消回归
static void test_rtsp_multiport_dual_tcp_negotiation() {
    net::init_network();

    // 1. 验证 M3 查询体包含小爱音箱触发 MultiPort 所需的全部官方能力字段
    const std::string m3 = rtsp::WfdRtspServer::build_m3_get_parameter_body();
    CHECK(m3.find("wfd_tcp_enable\r\n") != std::string::npos);
    CHECK(m3.find("wfd_tcp_multi_session_enable\r\n") != std::string::npos);
    CHECK(m3.find("wfd_image_enable_v2\r\n") != std::string::npos);
    CHECK(m3.find("wfd_standby_resume_capability\r\n") != std::string::npos);
    CHECK(m3.find("wfd_support_secure_win\r\n") != std::string::npos);
    CHECK(m3.find("wfd_mirror_control_enable\r\n") != std::string::npos);

    // 2. MultiPort 头解析边界
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("image_port=47070;multi_port=47072") == 47072);
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("MULTI_PORT = 51854 ; image_port=51852") == 51854);
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("image_port=47070") == -1);
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("multi_port=0") == -1);
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("multi_port=70000") == -1);
    CHECK(rtsp::WfdRtspServer::parse_multi_port_media_port("multi_port=abc") == -1);

    auto connect_loopback = [](uint16_t port, uint16_t& local_port_out) -> SocketHandle {
        SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(s != kInvalidSocket);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons(port);
        CHECK(connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        CHECK(net::set_socket_timeout(s, SO_RCVTIMEO, 2000));
        struct sockaddr_in local{};
        socklen_t llen = sizeof(local);
        CHECK(getsockname(s, reinterpret_cast<struct sockaddr*>(&local), &llen) == 0);
        local_port_out = ntohs(local.sin_port);
        return s;
    };

    auto recv_until = [](SocketHandle s, std::string& acc, const std::string& needle, int timeout_ms = 2000) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        char buf[4096];
        while (acc.find(needle) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
            if (!net::wait_readable(s, 50)) continue;
            auto n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            acc.append(buf, static_cast<size_t>(n));
        }
        return acc.find(needle) != std::string::npos;
    };

    auto handshake_to_m5 = [&](SocketHandle ctrl, std::string& acc) {
        CHECK(recv_until(ctrl, acc, "OPTIONS * RTSP/1.0"));
        const std::string m2 = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(m2.data()), m2.size()));
        CHECK(recv_until(ctrl, acc, "wfd_image_enable_v2"));
        const std::string m3_body =
            "wfd_audio_codecs_v2: 63 3 3\r\nwfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n";
        const std::string m3ok =
            "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: text/parameters\r\nContent-Length: " +
            std::to_string(m3_body.size()) + "\r\n\r\n" + m3_body;
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(m3ok.data()), m3ok.size()));
        CHECK(recv_until(ctrl, acc, "wfd_audio_codecs_v2:"));
        const std::string m4ok = "RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(m4ok.data()), m4ok.size()));
        CHECK(recv_until(ctrl, acc, "wfd_trigger_method: SETUP"));
        const std::string m5ok = "RTSP/1.0 200 OK\r\nCSeq: 4\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(m5ok.data()), m5ok.size()));
    };

    // 3. 完整双 TCP（含 image_port 辅助连接、重复 PLAY、控制保活、独立音频写通道）
    {
        std::atomic<int> ready_count{0};
        std::atomic<bool> fail_media{false};
        std::mutex status_mu;
        std::string last_m7_status;
        media::MultiAudioSender sender;
        auto [other_tx, other_rx] = make_socket_pair();
        CHECK(net::set_socket_timeout(other_rx, SO_RCVTIMEO, 1000));
        sender.add_or_update_target("other", "127.0.0.1", other_tx, 0, true, 2);
        std::promise<void> closed;
        auto closed_future = closed.get_future();
        rtsp::WfdRtspServer srv;
        rtsp::RtspServerCallbacks cbs;
        cbs.on_stream_connection = [&](const std::string& id, const std::string& ip,
                                       std::shared_ptr<net::ConnectionWriter> writer, std::function<void()> failure) {
            auto injected = std::make_shared<net::ConnectionWriter>(writer->socket(), false,
                [writer, &fail_media](const uint8_t* data, size_t size) -> net::WriteAttempt {
                    using State = net::WriteAttempt::State;
                    if (fail_media.load()) return {State::Failed};
                    const auto result = writer->write(data, size, 100);
                    return result == net::SendAllStatus::Ok ? net::WriteAttempt{State::Progress, size}
                                                           : net::WriteAttempt{State::Failed};
                });
            const auto fd = writer->socket();
            sender.add_or_update_target(id, ip, fd, 0, false, 3, std::move(injected), std::move(failure));
            ready_count.fetch_add(1);
            const char marker[] = "$AUDIO_MULTIPORT_OK";
            CHECK(writer->write(reinterpret_cast<const uint8_t*>(marker), sizeof(marker) - 1, 100) ==
                  net::SendAllStatus::Ok);
        };
        cbs.on_stop_media = [&](const auto& id) { sender.remove_target(id, 3); };
        cbs.on_session_closed = [&](const auto&) { closed.set_value(); };
        cbs.on_status = [&](const std::string&, const std::string& st) {
            if (st.find("RTSP M7") != std::string::npos) {
                std::lock_guard<std::mutex> lk(status_mu);
                last_m7_status = st;
            }
        };
        CHECK(srv.start("mp_dev", "127.0.0.1", 40000, 123456789ULL, false, std::move(cbs), 48000, 0));
        const uint16_t port = srv.listen_port();
        CHECK(port > 0);

        uint16_t ctrl_port = 0, img_port = 0, audio_port = 0;
        SocketHandle ctrl = connect_loopback(port, ctrl_port);
        std::string acc;
        handshake_to_m5(ctrl, acc);

        // 模拟真实小爱音箱：同时反连 image_port 与 multi_port
        SocketHandle img = connect_loopback(port, img_port);
        SocketHandle audio = connect_loopback(port, audio_port);

        // 非法 MultiPort（含控制端口自身）应返回 461
        const std::string bad_setup =
            "SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;interleaved=0-1\r\nMultiPort: image_port=" +
            std::to_string(img_port) + ";multi_port=" + std::to_string(ctrl_port) + "\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(bad_setup.data()), bad_setup.size()));
        CHECK(recv_until(ctrl, acc, "461 Unsupported Transport"));

        // 合法 MultiPort SETUP
        const std::string setup =
            "SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 3\r\n"
            "Transport: RTP/AVP/TCP;interleaved=0-1\r\nMultiPort: image_port=" +
            std::to_string(img_port) + ";multi_port=" + std::to_string(audio_port) + "\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(setup.data()), setup.size()));
        CHECK(recv_until(ctrl, acc, "TIME_OFFSET rtsp://localhost/wfd1.0"));
        CHECK(recv_until(ctrl, acc, "CSeq: 3\r\nSession: 1189641421"));

        // PLAY 绑定独立音频连接
        const std::string play = "PLAY rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 4\r\nSession: 1189641421\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(play.data()), play.size()));
        CHECK(recv_until(ctrl, acc, "Range: npt=now-"));

        std::string audio_data;
        CHECK(recv_until(audio, audio_data, "$AUDIO_MULTIPORT_OK"));
        CHECK(ready_count.load() == 1);

        // 重复 PLAY 不应重复触发 on_stream_ready，且控制连接可继续保活
        const std::string play2 = "PLAY rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 5\r\nSession: 1189641421\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(play2.data()), play2.size()));
        CHECK(recv_until(ctrl, acc, "CSeq: 5\r\nSession: 1189641421"));
        CHECK(ready_count.load() == 1);

        const std::string ka = "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\nCSeq: 6\r\nSession: 1189641421\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(ka.data()), ka.size()));
        CHECK(recv_until(ctrl, acc, "CSeq: 6\r\nSession: 1189641421;timeout=20"));

        {
            std::lock_guard<std::mutex> lk(status_mu);
            CHECK(last_m7_status.find("MultiPort") != std::string::npos);
            CHECK(last_m7_status.find(std::to_string(ctrl_port)) != std::string::npos);
            CHECK(last_m7_status.find(std::to_string(audio_port)) != std::string::npos);
        }

        // The control connection has just answered a keepalive. Kill only its media writer.
        fail_media.store(true);
        std::vector<uint8_t> audio_payload(128, 0x19);
        sender.broadcast_audio_frame(audio_payload.data(), audio_payload.size(), 90'000, 90'000);
        CHECK(closed_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(!srv.is_running() && sender.active_target_count() == 1);
        std::vector<uint8_t> other_wire;
        read_access_unit(other_rx, other_wire);
        CHECK(!other_wire.empty());
        net::close_socket(other_rx);

        srv.stop();
        net::close_socket(audio);
        net::close_socket(img);
        net::close_socket(ctrl);
    }

    // 4. 延后音频反连（PLAY 发出后再连接）验证 condition_variable 唤醒并成功绑定
    {
        std::atomic<int> ready_count{0};
        rtsp::WfdRtspServer srv;
        rtsp::RtspServerCallbacks cbs;
        cbs.on_stream_ready = [&](const std::string&, const std::string&, SocketHandle data_sock) {
            ready_count.fetch_add(1);
            const char marker[] = "$LATE_AUDIO_OK";
            net::send_all(data_sock, reinterpret_cast<const uint8_t*>(marker), sizeof(marker) - 1);
        };
        CHECK(srv.start("mp_late", "127.0.0.1", 40000, 123456789ULL, false, std::move(cbs), 48000, 0));
        const uint16_t srv_port = srv.listen_port();
        uint16_t ctrl_port = 0;
        SocketHandle ctrl = connect_loopback(srv_port, ctrl_port);
        std::string acc;
        handshake_to_m5(ctrl, acc);

        // 先绑定一个本地临时端口，待 PLAY 后再发起 connect
        SocketHandle late_audio = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(late_audio != kInvalidSocket);
        struct sockaddr_in laddr{};
        laddr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &laddr.sin_addr);
        laddr.sin_port = 0;
        CHECK(bind(late_audio, reinterpret_cast<struct sockaddr*>(&laddr), sizeof(laddr)) == 0);
        socklen_t llen = sizeof(laddr);
        CHECK(getsockname(late_audio, reinterpret_cast<struct sockaddr*>(&laddr), &llen) == 0);
        const uint16_t late_port = ntohs(laddr.sin_port);

        const std::string setup =
            "SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;interleaved=0-1\r\nMultiPort: image_port=0;multi_port=" +
            std::to_string(late_port) + "\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(setup.data()), setup.size()));
        CHECK(recv_until(ctrl, acc, "CSeq: 2\r\nSession: 1189641421"));

        const std::string play = "PLAY rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 3\r\nSession: 1189641421\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(play.data()), play.size()));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
        dst.sin_port = htons(srv_port);
        CHECK(connect(late_audio, reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst)) == 0);
        CHECK(net::set_socket_timeout(late_audio, SO_RCVTIMEO, 2000));

        CHECK(recv_until(ctrl, acc, "Range: npt=now-"));
        std::string late_data;
        CHECK(recv_until(late_audio, late_data, "$LATE_AUDIO_OK"));
        CHECK(ready_count.load() == 1);

        srv.stop();
        net::close_socket(late_audio);
        net::close_socket(ctrl);
    }

    // 5. 缺失音频反连返回 503 且绝不回落控制连接
    {
        std::atomic<int> ready_count{0};
        rtsp::WfdRtspServer srv;
        rtsp::RtspServerCallbacks cbs;
        cbs.on_stream_ready = [&](const std::string&, const std::string&, SocketHandle) {
            ready_count.fetch_add(1);
        };
        CHECK(srv.start("mp_timeout", "127.0.0.1", 40000, 123456789ULL, false, std::move(cbs), 48000, 0));
        uint16_t ctrl_port = 0;
        SocketHandle ctrl = connect_loopback(srv.listen_port(), ctrl_port);
        std::string acc;
        handshake_to_m5(ctrl, acc);

        const uint16_t missing_port = (ctrl_port == 54321) ? 54322 : 54321;
        const std::string setup =
            "SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;interleaved=0-1\r\nMultiPort: image_port=0;multi_port=" +
            std::to_string(missing_port) + "\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(setup.data()), setup.size()));
        CHECK(recv_until(ctrl, acc, "CSeq: 2\r\nSession: 1189641421"));

        const std::string play = "PLAY rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 3\r\nSession: 1189641421\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(play.data()), play.size()));
        CHECK(recv_until(ctrl, acc, "503 Service Unavailable", 3000));
        CHECK(ready_count.load() == 0);

        srv.stop();
        net::close_socket(ctrl);
    }

    // 5. 等待 MultiPort 反连期间调用 stop() 必须有界快速中断
    {
        rtsp::WfdRtspServer srv;
        CHECK(srv.start("mp_cancel", "127.0.0.1", 40000, 123456789ULL, false, {}, 48000, 0));
        uint16_t ctrl_port = 0;
        SocketHandle ctrl = connect_loopback(srv.listen_port(), ctrl_port);
        std::string acc;
        handshake_to_m5(ctrl, acc);

        const uint16_t missing_port = (ctrl_port == 54323) ? 54324 : 54323;
        const std::string setup =
            "SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 2\r\n"
            "MultiPort: multi_port=" + std::to_string(missing_port) + "\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(setup.data()), setup.size()));
        CHECK(recv_until(ctrl, acc, "CSeq: 2\r\nSession: 1189641421"));

        const std::string play = "PLAY rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\nCSeq: 3\r\n\r\n";
        CHECK(net::send_all(ctrl, reinterpret_cast<const uint8_t*>(play.data()), play.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        const auto t0 = std::chrono::steady_clock::now();
        srv.stop();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(stop_ms < 500);
        net::close_socket(ctrl);
    }

    std::cout << "[PASS] test_rtsp_multiport_dual_tcp_negotiation (SS-31)\n";
}

// 音轨平滑性与抗抖动专项回归：
// 1) 多帧拼合 ADTS 拆分与独立 PTS 递增
// 2) MasterMediaClock 阻塞后墙钟追齐（杜绝声卡切换后全程迟到帧）
// 3) SmoothLoopbackFifo 预缓冲、缺少量样本防归零、±1 样本线性插值频偏消化、极端积压交叉淡化
// 4) MultiAudioSender 淘汰旧帧与动态下调延迟时保持线上 RTP Seq / MPEG-TS CC / PTS 严格连续不倒流
// 5) WFD LPCM 44.1kHz / 48kHz 采样率头一致性
static void test_smooth_audio_anti_jitter_pipeline() {
    // 1. 拼合双帧 ADTS 切分与 MPEG-4(0xF1) -> MPEG-2(0xF9) 规范化
    uint8_t raw1[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t raw2[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    auto f1 = media::AacEncoder::wrap_adts(raw1, sizeof(raw1), 48000, 2);
    auto f2 = media::AacEncoder::wrap_adts(raw2, sizeof(raw2), 48000, 2);
    f1[1] = 0xF1; // 模拟 Windows MFT payloadType=1 输出 MPEG-4 ADTS 同步字
    std::vector<uint8_t> concat = f1;
    concat.insert(concat.end(), f2.begin(), f2.end());

    sync::MasterMediaClock aac_clk;
    aac_clk.reset(48000, 350);
    std::vector<uint64_t> emitted_pts;
    std::vector<size_t> emitted_lens;
    media::AacEncoder::emit_adts_frames(concat.data(), concat.size(), 48000, 2,
        [&](const uint8_t* adts, size_t len) {
            CHECK(adts[0] == 0xFF && adts[1] == 0xF9);
            auto ts = aac_clk.next_frame_timestamps(1024);
            emitted_pts.push_back(ts.pts_90khz);
            emitted_lens.push_back(len);
        });
    CHECK(emitted_pts.size() == 2);
    CHECK(emitted_lens[0] == f1.size() && emitted_lens[1] == f2.size());
    CHECK(emitted_pts[1] - emitted_pts[0] == 1920);

    // 2. 模拟声卡重路由阻塞 250ms 后，MasterMediaClock 自动按整帧边界追齐当前墙钟
    sync::MasterMediaClock stall_clk;
    stall_clk.reset(48000, 350);
    const uint64_t origin = stall_clk.session_origin_us();
    stall_clk.catch_up_to_now(480, origin + 250000ULL); // 模拟卡顿 250ms (= 12000 samples)
    auto after_stall = stall_clk.next_frame_timestamps(480);
    // 追齐后目标播放时刻必须精准落在 (origin + 250ms) + 350ms = origin + 600ms
    CHECK(after_stall.play_target_us == origin + 250000ULL + 350000ULL);

    // 3. SmoothLoopbackFifo：WASAPI 10ms(480帧) 分包喂给 AAC 21.33ms(1024帧) 消费者，持续 200 帧零静音坑
    media::SmoothLoopbackFifo fifo;
    fifo.reset(48000, 1024);
    std::vector<int16_t> chunk480(480 * 2, 4096);
    std::vector<int16_t> out1024(1024 * 2, 0);

    // 预热：推入 5 个 10ms 包 (2400 帧 >= preroll 1984 帧)
    for (int i = 0; i < 5; ++i) fifo.push_interleaved_stereo(chunk480.data(), 480);
    double time_acc_ms = 0.0;
    for (int step = 0; step < 120; ++step) {
        fifo.pop_smooth_frame(out1024.data(), 1024);
        CHECK(fifo.is_primed());
        // 断言整帧没有任何样本跌落归零（振幅恒在 4096 附近）
        for (size_t s = 0; s < out1024.size(); ++s) {
            CHECK(std::abs(out1024[s] - 4096) <= 2);
        }
        // 模拟存在 +500 ppm 声卡偏快时钟，按真实 10ms 颗粒度异步投递 480 帧包
        time_acc_ms += (1024.0 * 1000.0 / 48000.0) * 1.0005;
        while (time_acc_ms >= 10.0) {
            fifo.push_interleaved_stereo(chunk480.data(), 480);
            time_acc_ms -= 10.0;
        }
    }

    // 4. LPCM 44.1kHz vs 48kHz 采样率头验证
    int16_t one_stereo[2] = {100, -100};
    auto lpcm48 = media::TsPacketizer::wrap_wfd_lpcm(one_stereo, 1, 48000);
    auto lpcm44 = media::TsPacketizer::wrap_wfd_lpcm(one_stereo, 1, 44100);
    CHECK(lpcm48[0] == 0xA0 && lpcm48[3] == 0x11);
    CHECK(lpcm44[0] == 0xA0 && lpcm44[3] == 0x09);

    // 5. MultiAudioSender 动态调小延迟偏置时，线上 RTP Seq / MPEG-TS Audio CC / PES PTS 严格连续不倒退
    {
        media::MultiAudioSender sender;
        sender.set_stream_type(media::TsAudioStreamType::WfdLpcm);
        auto [s_tx, s_rx] = make_socket_pair();
        CHECK(net::set_socket_timeout(s_rx, SO_RCVTIMEO, 500));
        sender.add_or_update_target("room_smooth", "127.0.0.1", s_tx, +100, true, 1);

        std::vector<int16_t> pcm10ms(480 * 2, 2000);
        auto payload = media::TsPacketizer::wrap_wfd_lpcm(pcm10ms.data(), 480, 48000);

        // 先在 +100ms 偏置下发 3 帧，随后突然把偏置下调到 0ms（前几帧将落在重叠区间被干净跳过），再继续发 15 帧
        std::vector<uint8_t> stream;
        for (int i = 0; i < 3; ++i) {
            uint64_t pts = 90000ULL + static_cast<uint64_t>(i) * 900ULL;
            sender.broadcast_audio_frame(payload.data(), payload.size(), pts, static_cast<uint32_t>(pts));
            read_access_unit(s_rx, stream);
        }
        sender.set_target_delay_bias("room_smooth", 0);
        for (int i = 3; i < 20; ++i) {
            uint64_t pts = 90000ULL + static_cast<uint64_t>(i) * 900ULL;
            sender.broadcast_audio_frame(payload.data(), payload.size(), pts, static_cast<uint32_t>(pts));
        }
        for (int i = 13; i < 20; ++i) read_access_unit(s_rx, stream);
        sender.clear_all();
        net::close_socket(s_rx);

        // 逐个解析 RTP-over-TCP 包及内层 188B MPEG-TS 包，验证 0 帧错位、RTP Seq 连续、Audio CC 连续、PTS 严格递增
        size_t pos = 0;
        int rtp_cnt = 0;
        uint16_t expected_seq = 0;
        int expected_audio_cc = -1;
        int64_t prev_pes_pts = -1;
        while (pos < stream.size()) {
            CHECK(pos + 4 <= stream.size());
            CHECK(stream[pos] == 0x24 && stream[pos + 1] == 0x00);
            const size_t rtp_len = (static_cast<size_t>(stream[pos + 2]) << 8) | stream[pos + 3];
            CHECK(rtp_len >= 12 + 188 && pos + 4 + rtp_len <= stream.size());
            const uint8_t* rtp = stream.data() + pos + 4;
            const uint16_t seq = (static_cast<uint16_t>(rtp[2]) << 8) | rtp[3];
            if (rtp_cnt > 0) CHECK(seq == expected_seq);
            expected_seq = static_cast<uint16_t>(seq + 1);
            ++rtp_cnt;

            const size_t ts_bytes = rtp_len - 12;
            CHECK(ts_bytes % 188 == 0);
            for (size_t off = 0; off < ts_bytes; off += 188) {
                const uint8_t* ts = rtp + 12 + off;
                CHECK(ts[0] == 0x47);
                const uint16_t pid = (static_cast<uint16_t>(ts[1] & 0x1F) << 8) | ts[2];
                const bool pusi = (ts[1] & 0x40) != 0;
                const bool has_payload = (ts[3] & 0x10) != 0;
                const int cc = ts[3] & 0x0F;
                if (pid == 0x1100 && has_payload) {
                    if (expected_audio_cc >= 0) {
                        CHECK(cc == expected_audio_cc);
                    }
                    expected_audio_cc = (cc + 1) & 0x0F;
                    if (pusi) {
                        size_t hdr_off = 4;
                        if ((ts[3] & 0x20) != 0) hdr_off += 1 + ts[4];
                        CHECK(hdr_off + 14 <= 188);
                        const uint8_t* p = ts + hdr_off;
                        CHECK(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01);
                        uint64_t pts =
                            (static_cast<uint64_t>((p[9] >> 1) & 0x07) << 30) |
                            (static_cast<uint64_t>(p[10]) << 22) |
                            (static_cast<uint64_t>((p[11] >> 1) & 0x7F) << 15) |
                            (static_cast<uint64_t>(p[12]) << 7) |
                            (static_cast<uint64_t>((p[13] >> 1) & 0x7F));
                        if (prev_pes_pts >= 0) {
                            CHECK(static_cast<int64_t>(pts) >= prev_pes_pts + 900);
                        }
                        prev_pes_pts = static_cast<int64_t>(pts);
                    }
                }
            }
            pos += 4 + rtp_len;
        }
        CHECK(rtp_cnt >= 6 && prev_pes_pts > 0);
    }

    // 5b. 高码率 AAC (>720B, 如 800B 需 5 个 Audio TS 包) 遇到 PAT+PMT+PCR 帧时：
    //     PAT+PMT+PCR 必须独立为 Marker=0 (0x21) 的 RTP 包，5 个 Audio TS 包必须完整落在单个 Marker=1 (0xA1) RTP 包内，
    //     杜绝 MiPCAudio.exe ATSParser 报 "numBitsLeft = 5760 (720B), required = 6400" 截断错误！
    {
        media::MultiAudioSender sender;
        sender.set_stream_type(media::TsAudioStreamType::AdtsAac);
        auto [s_tx, s_rx] = make_socket_pair();
        CHECK(net::set_socket_timeout(s_rx, SO_RCVTIMEO, 500));
        sender.add_or_update_target("mipcaudio_pes", "127.0.0.1", s_tx, 0, true, 1);

        std::vector<uint8_t> big_raw(793, 0x5A);
        auto adts800 = media::AacEncoder::wrap_adts(big_raw.data(), big_raw.size(), 48000, 2);
        CHECK(adts800.size() == 800);
        sender.broadcast_audio_frame(adts800.data(), adts800.size(), 90000ULL, 90000U);
        std::vector<uint8_t> wire;
        read_access_unit(s_rx, wire);
        sender.clear_all();
        net::close_socket(s_rx);

        // 应正好拆成 2 个 RTP 包：
        // 包 1: PAT+PMT+PCR (3 * 188 = 564B), Marker = 0 (0x21)
        // 包 2: 完整 5 个 Audio PES TS 包 (5 * 188 = 940B), Marker = 1 (0xA1)
        size_t pos = 0;
        std::vector<std::pair<uint8_t, size_t>> pkts;
        while (pos < wire.size()) {
            CHECK(pos + 4 <= wire.size() && wire[pos] == 0x24 && wire[pos + 1] == 0x00);
            const size_t rtp_len = (static_cast<size_t>(wire[pos + 2]) << 8) | wire[pos + 3];
            CHECK(pos + 4 + rtp_len <= wire.size());
            pkts.push_back({wire[pos + 5], rtp_len - 12});
            pos += 4 + rtp_len;
        }
        CHECK(pkts.size() == 2);
        CHECK(pkts[0].first == 0x21 && pkts[0].second == 3 * 188);
        CHECK(pkts[1].first == 0xA1 && pkts[1].second == 5 * 188);
    }

    // 6. StreamLinearResampler + SmoothLoopbackFifo 波形连续性/零毛刺专项检验：
    //    模拟 44.1kHz（每包 441 帧）和 96kHz（每包 960 帧）正弦波分包重采样至 48kHz，
    //    校验跨包边界最大一阶差分与理论斜率一致，绝无边界阶跃或平顶毛刺
    for (uint32_t src_sr : {44100u, 96000u}) {
        media::StreamLinearResampler rs;
        rs.reset(src_sr, 48000);
        const double freq = 1000.0;
        const double amp = 20000.0;
        const size_t pkt_frames = src_sr / 100; // 10ms 分包
        std::vector<int16_t> in_pkt(pkt_frames * 2);
        std::vector<int16_t> out_pkt;
        std::vector<int16_t> all_out;
        size_t idx = 0;
        for (int p = 0; p < 50; ++p) {
            for (size_t i = 0; i < pkt_frames; ++i) {
                const double t = static_cast<double>(idx++) / static_cast<double>(src_sr);
                const int16_t v = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * 3.14159265358979323846 * freq * t)));
                in_pkt[i * 2] = v;
                in_pkt[i * 2 + 1] = static_cast<int16_t>(-v);
            }
            rs.process_interleaved_stereo(in_pkt.data(), pkt_frames, out_pkt);
            all_out.insert(all_out.end(), out_pkt.begin(), out_pkt.end());
        }
        const size_t total_out_frames = all_out.size() / 2;
        CHECK(total_out_frames >= 23900 && total_out_frames <= 24100); // 0.5s @ 48kHz ~= 24000 帧
        // 1kHz @ 48kHz, amp=20000 的理论最大相邻样本差分 = 20000 * 2*pi*1000/48000 ~= 2618；
        // 二阶差分理论最大 = 20000 * (2*pi*1000/48000)^2 ~= 343；若有边界毛刺会瞬间飙升到 >1000！
        int32_t max_d1 = 0;
        int32_t max_d2 = 0;
        for (size_t i = 2; i < total_out_frames; ++i) {
            const int32_t x0 = all_out[(i - 2) * 2];
            const int32_t x1 = all_out[(i - 1) * 2];
            const int32_t x2 = all_out[i * 2];
            max_d1 = std::max(max_d1, std::abs(x2 - x1));
            max_d2 = std::max(max_d2, std::abs((x2 - x1) - (x1 - x0)));
            CHECK(all_out[i * 2 + 1] == static_cast<int16_t>(-all_out[i * 2]));
        }
        CHECK(max_d1 <= 2650);
        CHECK(max_d2 <= 420);
    }

    std::cout << "[PASS] test_smooth_audio_anti_jitter_pipeline\n";
}

// mDNS 高可靠发现专项回归：
// 1) RFC 6762 QU (0x8001) 与 QM (0x0001) 查询报文编码校验
// 2) _mi-connect._udp.local. TXT appsData Base64 真实控制端口解析
// 3) _lyra-mdns._udp.local. 新一代服务解析与 TTL=0 Goodbye 退网记录过滤
// 4) MdnsDiscovery 多网卡 QU 启停 + trigger_query() 突发唤醒无阻塞快速退出
static void test_mdns_qu_unicast_and_multi_service_discovery() {
    // 1. 验证 QU (0x8001) vs QM (0x0001)
    auto qu = discovery::MdnsDiscovery::build_ptr_query_packet("_mi-connect._udp.local", true);
    auto qm = discovery::MdnsDiscovery::build_ptr_query_packet("_mi-connect._udp.local", false);
    CHECK(qu.size() == qm.size() && qu.size() > 16);
    CHECK(qu[qu.size() - 4] == 0x00 && qu[qu.size() - 3] == 0x0C); // PTR
    CHECK(qu[qu.size() - 2] == 0x80 && qu[qu.size() - 1] == 0x01); // QU = 1, IN = 1
    CHECK(qm[qm.size() - 2] == 0x00 && qm[qm.size() - 1] == 0x01); // QM = 0, IN = 1

    auto append_name = [](std::vector<uint8_t>& buf, const std::string& name) {
        size_t start = 0;
        while (start < name.size()) {
            size_t dot = name.find('.', start);
            if (dot == std::string::npos) dot = name.size();
            size_t len = dot - start;
            if (len > 0) {
                buf.push_back(static_cast<uint8_t>(len));
                buf.insert(buf.end(), name.begin() + static_cast<ptrdiff_t>(start),
                           name.begin() + static_cast<ptrdiff_t>(dot));
            }
            start = dot + 1;
        }
        buf.push_back(0);
    };
    auto append_rr = [&](std::vector<uint8_t>& buf, const std::string& name,
                         uint16_t type, uint32_t ttl, const std::vector<uint8_t>& rdata) {
        append_name(buf, name);
        buf.push_back(static_cast<uint8_t>(type >> 8));
        buf.push_back(static_cast<uint8_t>(type & 0xFF));
        buf.push_back(0x00);
        buf.push_back(0x01); // IN
        buf.push_back(static_cast<uint8_t>((ttl >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((ttl >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((ttl >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(ttl & 0xFF));
        buf.push_back(static_cast<uint8_t>(rdata.size() >> 8));
        buf.push_back(static_cast<uint8_t>(rdata.size() & 0xFF));
        buf.insert(buf.end(), rdata.begin(), rdata.end());
    };
    auto make_txt = [](const std::vector<std::string>& kvs) {
        std::vector<uint8_t> out;
        for (const auto& kv : kvs) {
            out.push_back(static_cast<uint8_t>(kv.size()));
            out.insert(out.end(), kv.begin(), kv.end());
        }
        return out;
    };

    // 2. 真机 _mi-connect 响应包（SoundFlow-Pad(F8TnlfFbXH), apps=[5], dev=4, appsData=gQAEBIMiww== -> 8899）
    {
        std::vector<uint8_t> pkt(12, 0);
        pkt[2] = 0x84; // Response + AA
        pkt[7] = 2;    // ANCOUNT = 2
        std::vector<uint8_t> ptr_rdata;
        append_name(ptr_rdata, "SoundFlow-Pad(F8TnlfFbXH)._mi-connect._udp.local");
        append_rr(pkt, "_mi-connect._udp.local", 12, 10, ptr_rdata);
        append_rr(pkt, "SoundFlow-Pad(F8TnlfFbXH)._mi-connect._udp.local", 16, 10,
                  make_txt({"version=196608", "apps=[5]", "name=SoundFlow-Pad", "idHash=RjhU",
                            "dev=4", "appsData=gQAEBIMiww=="}));
        discovery::DiscoveredDevice dev;
        CHECK(discovery::MdnsDiscovery::parse_response_packet(pkt.data(), pkt.size(), "192.168.5.84", dev));
        CHECK(dev.name == "SoundFlow-Pad");
        CHECK(dev.id_hash == "RjhU");
        CHECK(dev.device_type == 4);
        CHECK(dev.ip == "192.168.5.84");
        CHECK(dev.cmd_port == 8899);

        // 自定义非 8899 端口 appsData (9988 -> gQAAAAAnBA==)
        std::vector<uint8_t> pkt2(12, 0);
        pkt2[2] = 0x84;
        pkt2[7] = 2;
        append_rr(pkt2, "_mi-connect._udp.local", 12, 120, ptr_rdata);
        append_rr(pkt2, "SoundFlow-Pad(F8TnlfFbXH)._mi-connect._udp.local", 16, 120,
                  make_txt({"apps=[5]", "name=Redmi K90 Pro Max", "mac=AA:BB:CC:DD:EE:FF", "appsData=gQAAAAAnBA=="}));
        discovery::DiscoveredDevice dev2;
        CHECK(discovery::MdnsDiscovery::parse_response_packet(pkt2.data(), pkt2.size(), "192.168.5.84", dev2));
        CHECK(dev2.name == "Redmi K90 Pro Max");
        CHECK(dev2.cmd_port == 9988);
    }

    // 3. 真机 _lyra-mdns AppData 二进制 TLV (T=0x02) 中英文真名完整解码（v0 7B头与 v2 9B头）+ 防护校验
    {
        // v0 7B 头部实机抓包: C79DF89E._lyra-mdns -> "SoundFlow-Pad" (绝不显示 C79DF89E)
        std::vector<uint8_t> lyra_ptr1;
        append_name(lyra_ptr1, "C79DF89E._lyra-mdns._udp.local");
        std::vector<uint8_t> lyra_pkt1(12, 0);
        lyra_pkt1[2] = 0x84;
        lyra_pkt1[7] = 2;
        append_rr(lyra_pkt1, "_lyra-mdns._udp.local", 12, 10, lyra_ptr1);
        append_rr(lyra_pkt1, "C79DF89E._lyra-mdns._udp.local", 16, 10,
                  make_txt({"AppData=AEAFx534ngAFGSQQAQMKAwHargEBgAINU291bmRGbG93LVBhZCUBAw==", "MediumType=8192"}));
        discovery::DiscoveredDevice d1;
        CHECK(discovery::MdnsDiscovery::parse_response_packet(lyra_pkt1.data(), lyra_pkt1.size(), "192.168.5.84", d1));
        CHECK(d1.name == "SoundFlow-Pad");
        CHECK(d1.name_priority == 1);

        // v2 9B 头部实机抓包: A85D47E1._lyra-mdns -> "帅小白的REDMI K90 Pro Max"
        std::vector<uint8_t> lyra_ptr2;
        append_name(lyra_ptr2, "A85D47E1._lyra-mdns._udp.local");
        std::vector<uint8_t> lyra_pkt2(12, 0);
        lyra_pkt2[2] = 0x84;
        lyra_pkt2[7] = 2;
        append_rr(lyra_pkt2, "_lyra-mdns._udp.local", 12, 120, lyra_ptr2);
        append_rr(lyra_pkt2, "A85D47E1._lyra-mdns._udp.local", 16, 120,
                  make_txt({"AppData=AkEBqF1H4UGlAAUZPxdBBwoDAagTAQIBICMAIwKRPwId5biF5bCP55m955qEUkVETUkgSzkwIFBybyBNYXgmACYFBBUViro="}));
        discovery::DiscoveredDevice d2;
        CHECK(discovery::MdnsDiscovery::parse_response_packet(lyra_pkt2.data(), lyra_pkt2.size(), "192.168.5.84", d2));
        CHECK(d2.name == "帅小白的REDMI K90 Pro Max");
        CHECK(d2.name_priority == 1);

        // 代理 TUN 虚拟网段 198.18.0.1 不得被误识别为局域网音箱
        discovery::DiscoveredDevice d;
        std::vector<uint8_t> mi_ptr;
        append_name(mi_ptr, "TY1234._mi-connect._udp.local");
        std::vector<uint8_t> tun_pkt(12, 0);
        tun_pkt[2] = 0x84;
        tun_pkt[7] = 2;
        append_rr(tun_pkt, "_mi-connect._udp.local", 12, 120, mi_ptr);
        append_rr(tun_pkt, "TY1234._mi-connect._udp.local", 16, 120, make_txt({"apps=[5]", "name=TY1234"}));
        CHECK(!discovery::MdnsDiscovery::parse_response_packet(tun_pkt.data(), tun_pkt.size(), "198.18.0.1", d));

        // 非 MiPlay 服务的 _mi-connect (apps=[1,2] 不含 5) 必须被忽略
        std::vector<uint8_t> non_audio(12, 0);
        non_audio[2] = 0x84;
        non_audio[7] = 2;
        append_rr(non_audio, "_mi-connect._udp.local", 12, 120, mi_ptr);
        append_rr(non_audio, "TY1234._mi-connect._udp.local", 16, 120, make_txt({"apps=[1,2]", "name=TY1234"}));
        CHECK(!discovery::MdnsDiscovery::parse_response_packet(non_audio.data(), non_audio.size(), "192.168.5.90", d));

        // TTL == 0 Goodbye 包不得误报在线
        std::vector<uint8_t> bye(12, 0);
        bye[2] = 0x84;
        bye[7] = 1;
        append_rr(bye, "_mi-connect._udp.local", 12, 0, mi_ptr);
        CHECK(!discovery::MdnsDiscovery::parse_response_packet(bye.data(), bye.size(), "192.168.5.90", d));
    }

    // 4. 真实启停 + trigger_query() 突发唤醒快速回收
    {
        discovery::MdnsDiscovery disc;
        CHECK(disc.start("127.0.0.1", [](const discovery::DiscoveredDevice&) {}));
        disc.trigger_query();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        auto t0 = std::chrono::steady_clock::now();
        disc.stop();
        auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(stop_ms < 800);
    }

    std::cout << "[PASS] test_mdns_qu_unicast_and_multi_service_discovery\n";
}

static void test_transport_boundaries(bool dump = false) {
    uint64_t mask33 = (1ULL << 33) - 1;
    std::ifstream cases(MIPLAY_TEST_FIXTURES "/transport_cases.tsv");
    CHECK(cases);
    std::string line;
    while (std::getline(cases, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream values(line);
        std::string codec, fragments;
        size_t payload_size;
        unsigned previous, seq;
        uint64_t pts;
        CHECK(values >> codec >> payload_size >> previous >> seq >> pts >> fragments);
        const bool lpcm = codec == "LPCM";
        auto ts_type = lpcm ? media::TsAudioStreamType::WfdLpcm : media::TsAudioStreamType::AdtsAac;
        std::vector<uint8_t> payload(payload_size);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 37 + 11);
        media::TsPacketizer packer(ts_type);
        for (unsigned i = 0; i < previous; ++i) {
            packer.packetize_audio_frame(payload.data(), payload.size(), 1920ULL * i);
        }
        const bool force = previous == 2;
        auto ts = packer.packetize_audio_frame(payload.data(), payload.size(), pts, force);
        std::vector<uint8_t> interleaved;
        auto seq16 = static_cast<uint16_t>(seq);
        media::MultiAudioSender::append_rtp_batch(interleaved, ts, seq16, static_cast<uint32_t>(pts));

        std::istringstream fragments_stream(fragments);
        std::string count;
        std::vector<uint8_t> pes;
        unsigned audioCount = (previous * ((16 + payload_size + 183) / 184)) & 15;
        unsigned psiCount = ((previous + 3) / 4) & 15;
        bool first_audio = true;
        std::vector<uint16_t> psi;
        size_t pos = 0;
        while (std::getline(fragments_stream, count, ',')) {
            CHECK(pos + 16 <= interleaved.size());
            const auto* pkt = interleaved.data() + pos;
            CHECK(pkt[0] == '$' && pkt[1] == 0 && pkt[4] == 0x80);
            const size_t rtp_len = (pkt[2] << 8) | pkt[3];
            CHECK(rtp_len == 12 + std::stoul(count) * 188);
            CHECK(rtp_len <= 12 + 7 * 188 && pos + 4 + rtp_len <= interleaved.size());
            CHECK(pkt[5] == (pos + 4 + rtp_len == interleaved.size() ? 0xa1 : 0x21));
            CHECK(((pkt[6] << 8) | pkt[7]) == (seq++ & 0xffff));
            uint32_t rtp_ts = 0;
            for (int i = 8; i < 12; ++i) rtp_ts = (rtp_ts << 8) | pkt[i];
            CHECK(rtp_ts == static_cast<uint32_t>(pts));
            for (size_t offset = 16; offset < rtp_len + 4; offset += 188) {
                const auto* part = pkt + offset;
                CHECK(part[0] == 0x47);
                const uint16_t pid = ((part[1] & 31) << 8) | part[2];
                if (pid == 0x1100) {
                    CHECK(((part[1] & 0x40) != 0) == first_audio);
                    CHECK((part[3] & 15) == (audioCount++ & 15));
                    CHECK((part[3] & 16) != 0);
                    size_t data = part[3] & 32 ? 5 + part[4] : 4;
                    CHECK(data < 188);
                    pes.insert(pes.end(), part + data, part + 188);
                    first_audio = false;
                } else {
                    CHECK(first_audio);
                    psi.push_back(pid);
                    if (pid == 0x1000) {
                        CHECK(part[3] == 0x20 && part[4] == 183 && part[5] == 0x10);
                        uint64_t pcr = 0;
                        for (int i = 6; i < 10; ++i) pcr = (pcr << 8) | part[i];
                        CHECK(((pcr << 1) | (part[10] >> 7)) == (pts & mask33));
                    } else {
                        CHECK((part[3] & 15) == psiCount);
                        CHECK((part[1] & 0x40) != 0);
                        if (pid == 0x0100) CHECK(part[17] == static_cast<uint8_t>(ts_type));
                    }
                }
            }
            pos += 4 + rtp_len;
        }
        CHECK(pos == interleaved.size());
        CHECK((force || previous % 4 == 0) ? (psi == std::vector<uint16_t>{0, 0x100, 0x1000}) : psi.empty());
        CHECK(pes.size() == payload.size() + 16);
        CHECK(pes[0] == 0 && pes[1] == 0 && pes[2] == 1 && pes[3] == (lpcm ? 0xbd : 0xc0));
        CHECK(((pes[4] << 8) | pes[5]) == 10 + payload.size());
        uint64_t pes_pts = (uint64_t((pes[9] >> 1) & 7) << 30) | (uint64_t(pes[10]) << 22) |
                           (uint64_t(pes[11] >> 1) << 15) | (uint64_t(pes[12]) << 7) | (pes[13] >> 1);
        CHECK(pes_pts == (pts & mask33));
        CHECK(std::equal(payload.begin(), payload.end(), pes.begin() + 16));
        if (dump) {
            for (auto byte : interleaved) std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
            std::cout << "\n";
        }
    }
    if (!dump) std::cout << "[PASS] test_transport_boundaries\n";
}

static void test_capture_conversion_and_fifo_limit() {
    for (uint32_t mask : {0x3fu, 0x63fu}) {
        const size_t channels = mask == 0x3f ? 6 : 8;
        media::StereoDownmixer mixer(media::CaptureSampleFormat::Pcm16, channels, mask, channels * 2);
        for (size_t ch = 0; ch < channels; ++ch) {
            std::vector<uint8_t> input(channels * 2);
            input[2 * ch] = 0xff;
            input[2 * ch + 1] = 0x7f;
            int16_t stereo[2]{};
            mixer.convert(input.data(), 1, stereo);
            if (ch == 2 || ch == 3) CHECK(stereo[0] > 3000 && stereo[0] == stereo[1]);
            else if (ch % 2 == 0) CHECK(stereo[0] > 3000 && stereo[1] == 0);
            else CHECK(stereo[1] > 3000 && stereo[0] == 0);
        }
        std::vector<uint8_t> input(channels * 2);
        for (size_t i = 1; i < input.size(); i += 2) input[i] = 0x80;
        int16_t stereo[2]{};
        mixer.convert(input.data(), 1, stereo);
        CHECK(stereo[0] == INT16_MIN && stereo[1] == INT16_MIN);
    }

    const uint8_t packed[] = {0xff, 0xff, 0x7f, 0, 0, 0x80};
    const uint8_t aligned[] = {0, 0xff, 0xff, 0x7f, 0, 0, 0, 0x80};
    const uint8_t floats[] = {0, 0, 0xc0, 0x7f, 0, 0, 0x80, 0xff};
    int16_t converted[2]{};
    media::StereoDownmixer(media::CaptureSampleFormat::Pcm24, 2, 3, 6).convert(packed, 1, converted);
    CHECK(converted[0] == INT16_MAX && converted[1] == INT16_MIN);
    media::StereoDownmixer(media::CaptureSampleFormat::Pcm32, 2, 3, 8).convert(aligned, 1, converted);
    CHECK(converted[0] == INT16_MAX && converted[1] == INT16_MIN);
    media::StereoDownmixer(media::CaptureSampleFormat::Float32, 2, 3, 8).convert(floats, 1, converted);
    CHECK(converted[0] == 0 && converted[1] == 0);
    media::StereoDownmixer(media::CaptureSampleFormat::Pcm24, 1, 4, 3).convert(packed, 1, converted);
    CHECK(converted[0] == INT16_MAX && converted[1] == INT16_MAX);

    media::SmoothLoopbackFifo fifo;
    fifo.reset(48000, 480);
    std::vector<int16_t> warm(3000 * 2, 10000), out(480 * 2);
    fifo.push_interleaved_stereo(warm.data(), warm.size() / 2);
    fifo.pop_smooth_frame(out.data(), 480);
    CHECK(out.back() == 10000);
    std::vector<int16_t> flood(50000 * 2, -10000);
    for (int i = 0; i < 10; ++i) {
        fifo.push_interleaved_stereo(flood.data(), flood.size() / 2);
        CHECK(fifo.available_frames() <= 12000);
    }
    fifo.pop_smooth_frame(out.data(), 480);
    CHECK(out.front() > 9600 && out.back() == -10000);
    std::cout << "[PASS] test_capture_conversion_and_fifo_limit\n";
}

static void test_resampling_partitions_and_alias_rejection() {
    const std::array<unsigned, 5> chunks{1, 13, 113, 1, 941};
    for (const uint32_t rate : {44100u, 96000u, 192000u}) {
        media::StreamLinearResampler res;
        std::vector<int16_t> input(rate * 2), bulk, part, partitioned;
        for (size_t i = 0; i < input.size() / 2; ++i) {
            input[2 * i] = static_cast<int16_t>((i * 1273) % 65536 - 32768);
            input[2 * i + 1] = static_cast<int16_t>((i * 7919) % 65536 - 32768);
        }
        res.reset(rate, 48000);
        res.process_interleaved_stereo(input.data(), input.size() / 2, bulk);
        CHECK(bulk.size() / 2 == ((uint64_t(rate) - 1) * 48000) / rate + 1);
        for (bool single : {true, false}) {
            res.reset(rate, 48000);
            partitioned.clear();
            size_t index = 0;
            for (size_t i = 0; i < rate;) {
                const size_t count = std::min<size_t>(rate - i, single ? 1 : chunks[index++ % chunks.size()]);
                res.process_interleaved_stereo(input.data() + 2 * i, count, part);
                partitioned.insert(partitioned.end(), part.begin(), part.end());
                i += count;
            }
            CHECK(partitioned == bulk);
        }
        res.reset(rate, 48000);
        const std::array<int16_t, 2> full{INT16_MIN, INT16_MAX};
        res.process_interleaved_stereo(full.data(), 1, part);
        CHECK(part.size() == 2 && part[0] == INT16_MIN && part[1] == INT16_MAX);
    }

    for (const unsigned frequency : {1000, 30000}) {
        media::StreamLinearResampler res;
        res.reset(96000, 48000);
        std::vector<int16_t> input(9600 * 2), out;
        for (size_t i = 0; i < input.size() / 2; ++i) {
            input[2 * i] = static_cast<int16_t>(std::lround(20000 * std::sin(6.283185307179586 * frequency * i / 96000)));
        }
        res.process_interleaved_stereo(input.data(), input.size() / 2, out);
        int peak = 0;
        for (size_t i = 400; i < out.size() / 2; ++i) peak = std::max(peak, std::abs(int(out[2 * i])));
        if (frequency == 1000) CHECK(peak > 19800 && peak < 20100);
        else CHECK(peak < 100);
    }

    std::cout << "[PASS] test_resampling_partitions_and_alias_rejection\n";
}

static void test_mdns_fixtures_and_expiry() {
    std::ifstream fixtures(MIPLAY_TEST_FIXTURES "/mdns_cases.tsv");
    CHECK(fixtures);
    std::string line;
    while (std::getline(fixtures, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string label, source, hex, expected;
        std::getline(fields, label, '\t');
        std::getline(fields, source, '\t');
        std::getline(fields, hex, '\t');
        std::getline(fields, expected);
        std::vector<uint8_t> packet;
        for (size_t i = 0; i < hex.size(); i += 2) packet.push_back(std::stoi(hex.substr(i, 2), nullptr, 16));
        std::set<std::string> results;
        for (const auto& entry : discovery::MdnsDiscovery::parse_announcements(packet.data(), packet.size(), source)) {
            const auto& d = entry.device;
            if (!entry.ttl_seconds) results.insert("Bye");
            else results.insert("Live:" + d.name + ":" + d.ip + ":" + std::to_string(d.cmd_port) + ":" +
                                 std::to_string(d.name_priority) + ":" + std::to_string(d.has_explicit_port()));
        }
        std::set<std::string> wanted;
        if (expected != "-") {
            std::istringstream values(expected);
            std::string value;
            while (std::getline(values, value, ',')) wanted.insert(value);
        }
        if (results != wanted) std::cerr << "mDNS fixture " << label << "\n";
        CHECK(results == wanted);
    }

    discovery::DiscoveryCache cache;
    discovery::DiscoveredDevice device;
    device.name = "Room";
    device.name_priority = 2;
    device.ip = device.id = "192.168.10.5";
    device.cmd_port = 9988;
    device.port_source = discovery::PortSource::Txt;
    auto updates = cache.update({{"first", device.ip, 2, device}}, 100);
    CHECK(updates.size() == 1 && updates[0].online && updates[0].cmd_port == 9988);
    const std::string stable = updates[0].id;
    const auto first_revision = updates[0].revision;

    device.mac = "AA:BB:CC:DD:EE:FF";
    device.id = device.mac;
    device.port_source = discovery::PortSource::Default;
    device.cmd_port = 8899;
    updates = cache.update({{"first", device.ip, 3, device}}, 700'000);
    CHECK(updates.size() == 1 && updates[0].id == stable && updates[0].cmd_port == 9988);
    CHECK(updates[0].revision > first_revision && updates[0].mac == device.mac);
    CHECK(cache.update({}, 2'000'100).empty());

    device.ip = "192.168.10.6";
    device.port_source = discovery::PortSource::Txt;
    updates = cache.update({{"first", device.ip, 2, device}, {"second", device.ip, 1, device}}, 2'100'000);
    CHECK(updates.size() == 1 && updates[0].id == stable && updates[0].ip == device.ip);
    CHECK(updates[0].cmd_port == 8899 && cache.list_devices().size() == 1);
    updates = cache.update({{"second", device.ip, 0, {}}}, 2'100'100);
    CHECK(updates.size() == 1 && updates[0].online);
    updates = cache.update({}, 4'100'000);
    CHECK(updates.size() == 1 && !updates[0].online && cache.list_devices().empty());
    updates = cache.update({{"second", device.ip, 10, device}}, 4'200'000);
    CHECK(updates.size() == 1 && updates[0].online && updates[0].id == stable);
    std::cout << "[PASS] test_mdns_fixtures_and_expiry\n";
}

static void test_discovery_burst_deadlines() {
    using namespace std::chrono_literals;
    const auto start = discovery::ProbeBurst::Time{};
    discovery::ProbeBurst burst;
    CHECK(burst.begin(start));
    CHECK(burst.take_due(start) == 0);
    CHECK(!burst.begin(start + 5ms));
    CHECK(!burst.take_due(start + 219ms));
    CHECK(burst.take_due(start + 220ms) == 1);
    CHECK(!burst.begin(start + 300ms));
    CHECK(!burst.take_due(start + 649ms));
    CHECK(burst.take_due(start + 650ms) == 2);
    CHECK(!burst.active() && !burst.take_due(start + 900ms));
    CHECK(burst.begin(start + 1500ms));
    CHECK(burst.take_due(start + 1500ms) == 0);
    std::cout << "[PASS] test_discovery_burst_deadlines\n";
}

static void test_discovery_concurrent_lifecycle_and_manual_identity() {
    MultiRoomEngine engine("127.0.0.1");
    std::barrier start(4);
    std::vector<std::thread> threads;
    threads.emplace_back([&] { start.arrive_and_wait(); for (int i = 0; i < 8; ++i) CHECK(engine.start_discovery()); });
    threads.emplace_back([&] { start.arrive_and_wait(); for (int i = 0; i < 8; ++i) engine.stop_discovery(); });
    threads.emplace_back([&] { start.arrive_and_wait(); for (int i = 0; i < 8; ++i) engine.refresh_discovery(i % 2); });
    threads.emplace_back([&] { start.arrive_and_wait(); for (int i = 0; i < 8; ++i) engine.set_bind_ip(i % 2 ? "127.0.0.1" : ""); });
    for (auto& t : threads) t.join();
    engine.stop_discovery();
    CHECK(!engine.snapshot_state().discovery_running);
    MultiRoomEngine manual_engine("127.0.0.1");
    const auto id = manual_engine.add_manual_device("Speaker", "127.0.0.3", 8899);
    CHECK(manual_engine.add_device_to_group(id));
    CHECK(manual_engine.add_manual_device("Updated", "127.0.0.3", 8899) == id);
    CHECK(manual_engine.add_manual_device("Other port", "127.0.0.3", 9988) != id);
    const auto snap = manual_engine.snapshot_state();
    CHECK(snap.devices.size() == 2);
    for (const auto& d : snap.devices) {
        if (d.info.id == id) CHECK(d.info.name == "Updated" && d.in_group && d.info.is_manual);
    }
    std::cout << "[PASS] test_discovery_concurrent_lifecycle_and_manual_identity\n";
}

static void test_manual_and_discovered_identity_revisions() {
    MultiRoomEngine engine("127.0.0.1");
    const auto manual = engine.add_manual_device("Living Room", "127.0.0.3", 9988);
    CHECK(engine.add_device_to_group(manual));
    discovery::DiscoveredDevice first;
    first.id = "aa:bb:cc:dd:ee:ff";
    first.mac = first.id;
    first.name = "Announced room";
    first.ip = "127.0.0.3";
    first.cmd_port = 9988;
    first.port_source = discovery::PortSource::Srv;
    first.revision = 10;
    engine.publish_discovery_snapshot_for_test(first);
    CHECK(engine.snapshot_state().devices.size() == 1);
    auto moved = first;
    moved.ip = "127.0.0.4";
    moved.cmd_port = 8899;
    moved.revision = 12;

    std::promise<void> newer_delivered;
    auto newer = newer_delivered.get_future();
    std::thread delayed_old_callback([&] {
        newer.wait();
        first.revision = 11;
        engine.publish_discovery_snapshot_for_test(first);
    });
    engine.publish_discovery_snapshot_for_test(moved);
    newer_delivered.set_value();
    delayed_old_callback.join();

    auto devices = engine.snapshot_state().devices;
    CHECK(devices.size() == 1 && devices[0].info.id == manual && devices[0].in_group);
    CHECK(devices[0].info.ip == moved.ip && devices[0].info.cmd_port == 8899);
    CHECK(devices[0].info.name == "Living Room" && devices[0].info.is_manual);
    moved.online = false;
    moved.revision = 13;
    engine.publish_discovery_snapshot_for_test(moved);
    CHECK(engine.snapshot_state().devices[0].info.online);

    discovery::DiscoveredDevice automatic;
    automatic.id = automatic.ip = "127.0.0.9";
    automatic.name = "Automatic";
    automatic.revision = 14;
    engine.publish_discovery_snapshot_for_test(automatic);
    CHECK(engine.add_manual_device("Named by user", automatic.ip, 8899) == automatic.id);
    CHECK(engine.snapshot_state().devices.size() == 2);
    std::cout << "[PASS] test_manual_and_discovered_identity_revisions\n";
}

static void test_write_deadline_and_serialization() {
    using Status = net::SendAllStatus;
    using State = net::WriteAttempt::State;
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    auto get_now = [&] { return now; };
    auto wait = [&](auto) { now += 20ms; };
    const std::array<uint8_t, 30> data{};
    std::atomic<bool> cancel{false};
    auto clean = [](auto, auto) -> net::WriteAttempt { return {State::WouldBlock}; };
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, clean, wait) == Status::TimeoutClean);
    CHECK(now.time_since_epoch() == 120ms);

    auto fatal = [](auto, auto) -> net::WriteAttempt { return {State::Failed}; };
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, fatal, wait) == Status::Failed);

    auto interrupted = [&](auto, auto) -> net::WriteAttempt {
        now += 10ms;
        return {State::Interrupted};
    };
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, interrupted, wait) == Status::Failed);

    unsigned progress = 0;
    auto short_writes = [&](auto, auto) -> net::WriteAttempt {
        ++progress;
        now += 10ms;
        return {State::Progress, 1};
    };
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, short_writes, wait) == Status::Partial);
    CHECK(progress == 12);
    auto cancel_partial = [&](auto, auto) -> net::WriteAttempt {
        cancel.store(true);
        return {State::Progress, 1};
    };
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, cancel_partial, wait) == Status::Partial);
    CHECK(net::write_until(data.data(), data.size(), now + 120ms, &cancel, get_now, fatal, wait) == Status::Cancelled);

    net::ConnectionWriter setup_failure(kInvalidSocket);
    CHECK(setup_failure.write(data.data(), data.size(), 10) == Status::Failed);

    auto [tx, rx] = make_socket_pair();
    CHECK(net::set_socket_timeout(rx, SO_RCVTIMEO, 750));
    std::barrier after_short_write(2);
    std::barrier sending_control(2);
    bool first = true;
    net::ConnectionWriter writer(tx, true, [&](const uint8_t* bytes, size_t length) -> net::WriteAttempt {
        auto send_length = first ? 1 : length;
        auto n = send(tx, reinterpret_cast<const char*>(bytes), static_cast<int>(send_length), 0);
        CHECK(n > 0);
        if (std::exchange(first, false)) {
            after_short_write.arrive_and_wait();
            sending_control.arrive_and_wait();
        }
        return {State::Progress, static_cast<size_t>(n)};
    });
    const std::array<uint8_t, 8> interleaved{'$', 0, 0, 4, 1, 2, 3, 4};
    const std::string response = "RTSP/1.0 200 OK\r\nCSeq: 7\r\n\r\n";
    const std::string teardown = "RTSP/1.0 200 OK\r\nCSeq: 8\r\n\r\n";
    std::thread media([&] { CHECK(writer.write(interleaved.data(), interleaved.size(), 2000) == Status::Ok); });
    std::thread control([&] {
        after_short_write.arrive_and_wait();
        sending_control.arrive_and_wait();
        for (auto& message : {response, teardown}) {
            CHECK(writer.write(reinterpret_cast<const uint8_t*>(message.data()), message.size(), 2000) == Status::Ok);
        }
    });
    std::vector<uint8_t> wire(interleaved.size() + response.size() + teardown.size());
    receive_exact(rx, wire.data(), wire.size());
    media.join();
    control.join();
    CHECK(std::equal(interleaved.begin(), interleaved.end(), wire.begin()));
    CHECK(std::string(wire.begin() + interleaved.size(), wire.end()) == response + teardown);
    net::close_socket(rx);

    auto [partial_tx, partial_rx] = make_socket_pair();
    unsigned attempts = 0;
    net::ConnectionWriter broken(partial_tx, true, [&](const uint8_t*, size_t) -> net::WriteAttempt {
        if (++attempts == 1) return {State::Progress, 1};
        return {State::Failed};
    });
    CHECK(broken.write(data.data(), data.size(), 100) == Status::Partial);
    CHECK(broken.write(data.data(), data.size(), 100) == Status::Failed && attempts == 2);
    net::close_socket(partial_rx);
    std::cout << "[PASS] test_write_deadline_and_serialization\n";
}

static void test_aac_backpressure_timeline_and_truncated_adts() {
    media::AacTimeline aac;
    using Result = media::AacInputResult;
    auto silent = [] { return true; };
    auto accepted = [] { return Result::Accepted; };
    CHECK(aac.submit(0, accepted, silent));
    CHECK(aac.submit(1920, accepted, silent));

    std::vector<uint64_t> output;
    unsigned attempts = 0;
    auto deliver = [&] {
        while (auto pts = aac.take_output()) output.push_back(*pts);
        return true;
    };
    CHECK(aac.submit(21600, [&] { return ++attempts == 1 ? Result::Backpressure : Result::Accepted; }, deliver));
    CHECK(attempts == 2 && (output == std::vector<uint64_t>{0, 1920, 21600}));

    attempts = 0;
    CHECK(!aac.submit(23520, [&] { ++attempts; return Result::Backpressure; }, silent));
    CHECK(attempts == 3 && !aac.take_output());
    CHECK(!aac.submit(25440, [] { return Result::Failed; }, silent) && !aac.take_output());
    for (uint64_t i = 0; i < 64; ++i) CHECK(aac.submit(i * 1920, accepted, silent));
    CHECK(!aac.submit(64 * 1920, accepted, silent));
    aac.clear();
    CHECK(!aac.take_output());

    const uint8_t raw[] = {1, 2, 3, 4};
    auto adts = media::AacEncoder::wrap_adts(raw, 4, 48000, 2);
    const size_t length = adts.size();
    std::vector<uint8_t> prefix(adts.begin(), adts.begin() + 5);
    adts.insert(adts.end(), prefix.begin(), prefix.end());
    size_t frames = 0;
    media::AacEncoder::emit_adts_frames(adts.data(), adts.size(), 48000, 2,
                                      [&](auto, auto) { ++frames; });
    CHECK(frames == 0);
    media::AacEncoder::emit_adts_frames(adts.data(), length - 1, 48000, 2,
                                      [&](auto, auto) { ++frames; });
    CHECK(frames == 0);
    std::cout << "[PASS] test_aac_backpressure_timeline_and_truncated_adts\n";
}

static void test_fixed_capture_sample_clock() {
    sync::MasterMediaClock clock;
    clock.reset(48000, 350);
    const auto origin = clock.session_origin_us();
    const auto first = clock.next_frame_timestamps(480, false);
    CHECK(first.linear_pts_90khz == 0);
    for (uint64_t elapsed : {40000, 55000, 60000, 80000, 240000}) {
        clock.reset(48000, 350);
        clock.next_frame_timestamps(480);
        const auto skipped = media::AudioPipeline::skipped_capture_samples(480, 48000, 480, elapsed);
        clock.skip_samples(skipped);
        const auto recovered = clock.next_frame_timestamps(480);
        const auto nominal = (elapsed / 10000) * 10000;
        CHECK(recovered.play_target_us == clock.session_origin_us() + 350000 + nominal);
        CHECK(recovered.play_target_us >= origin);
        CHECK(clock.next_frame_timestamps(480).linear_pts_90khz - recovered.linear_pts_90khz == 900);
    }
    std::cout << "[PASS] test_fixed_capture_sample_clock\n";
}

#if defined(__linux__)
static int run_continuous_loopback_source() {
    const std::array<uint8_t, 4> sample{0x34, 0x12, 0x34, 0x12};
    std::array<uint8_t, 8193> bytes{};
    size_t position = 0;
    while (true) {
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = sample[(position + i) % 4];
        const auto written = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (written <= 0) return 0;
        position += static_cast<size_t>(written);
    }
}

static void test_linux_continuous_loopback_and_source_switch() {
    media::AudioPipeline pipeline;
    pipeline.set_loopback_command_override_for_test({"/proc/self/exe", "--continuous-loopback-source"});
    CHECK(pipeline.set_source_mode(media::AudioSourceMode::SystemLoopback));
    std::mutex mu;
    std::condition_variable notices;
    size_t matching = 0, switched = 0, mixed_old = 0;
    std::atomic<bool> expect_other{false};
    CHECK(pipeline.start(48000, 480, [&](const int16_t* pcm, size_t frames) {
        std::lock_guard lock(mu);
        if (std::all_of(pcm, pcm + frames * 2, [](auto sample) { return sample == 0x1234; })) ++matching;
        if (expect_other.load()) {
            ++switched;
            if (std::find(pcm, pcm + frames * 2, 0x1234) != pcm + frames * 2) ++mixed_old;
        }
        notices.notify_one();
    }));
    {
        std::unique_lock lock(mu);
        CHECK(notices.wait_for(lock, std::chrono::seconds(3), [&] { return matching >= 5; }));
    }
    CHECK(pipeline.set_source_mode(media::AudioSourceMode::ExternalPcm));
    expect_other.store(true);
    {
        std::unique_lock lock(mu);
        CHECK(notices.wait_for(lock, std::chrono::seconds(1), [&] { return switched >= 3; }));
        CHECK(mixed_old == 0);
    }
    const auto start = std::chrono::steady_clock::now();
    pipeline.stop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));
    std::cout << "[PASS] test_linux_continuous_loopback_and_source_switch\n";
}
#endif

static void test_remote_control_and_lrc_sync() {
    // 1. 0x0022 Notify 反向播控键 + key-seek (0x09 u64be) + volume (0x03 u8)
    {
        std::vector<uint8_t> combined;
        auto append = [&](const std::vector<uint8_t>& v) {
            combined.insert(combined.end(), v.begin(), v.end());
        };
        append(cmd::encode_notify_control_key("key-pause"));
        append(cmd::encode_notify_control_key("key-resume"));
        append(cmd::encode_notify_control_key("key-next"));
        append(cmd::encode_notify_control_key("key-prev"));
        append(cmd::encode_notify_seek_ms(98765ULL));
        const uint8_t vol_tlv[] = {6, 'v', 'o', 'l', 'u', 'm', 'e', 0x03, 42};
        combined.insert(combined.end(), std::begin(vol_tlv), std::end(vol_tlv));

        auto evs = cmd::decode_remote_control_events(static_cast<uint16_t>(cmd::CmdType::Notify), combined);
        CHECK(evs.size() == 6);
        CHECK(evs[0].action == cmd::RemoteControlAction::Pause);
        CHECK(evs[1].action == cmd::RemoteControlAction::Resume);
        CHECK(evs[2].action == cmd::RemoteControlAction::Next);
        CHECK(evs[3].action == cmd::RemoteControlAction::Previous);
        CHECK(evs[4].action == cmd::RemoteControlAction::Seek && evs[4].position_ms == 98765ULL);
        CHECK(evs[5].action == cmd::RemoteControlAction::SetVolume && evs[5].volume == 42);
    }

    // 2. 直发 Pause / Resume / SetPosition 兜底解析
    {
        auto p = cmd::decode_remote_control_events(static_cast<uint16_t>(cmd::CmdType::Pause), {});
        CHECK(p.size() == 1 && p[0].action == cmd::RemoteControlAction::Pause);
        auto r = cmd::decode_remote_control_events(static_cast<uint16_t>(cmd::CmdType::Resume), {});
        CHECK(r.size() == 1 && r[0].action == cmd::RemoteControlAction::Resume);
        std::vector<uint8_t> pos_le(8, 0);
        uint64_t pos_val = 45200ULL;
        for (int i = 0; i < 8; ++i) pos_le[i] = static_cast<uint8_t>((pos_val >> (8 * i)) & 0xFF);
        auto s = cmd::decode_remote_control_events(static_cast<uint16_t>(cmd::CmdType::SetPosition), pos_le);
        CHECK(s.size() == 1 && s[0].action == cmd::RemoteControlAction::Seek && s[0].position_ms == 45200ULL);
    }

    // 3. LRC 解析（UTF-8 BOM + 多时间标签 + [offset] + 毫秒二分查找）
    {
        const std::string sample =
            "\xEF\xBB\xBF[ti:探窗]\r\n[offset:500]\r\n[00:01.50]第一句歌词\r\n"
            "[00:05.00][00:12.25]副歌重复行\n[00:09.00]间奏\n";
        auto doc = media::LrcDocument::parse(sample);
        CHECK(doc.size() == 4);
        CHECK(doc.line_at(500).empty());
        CHECK(doc.line_at(1000) == "第一句歌词");
        CHECK(doc.line_at(4500) == "副歌重复行");
        CHECK(doc.line_at(8500) == "间奏");
        CHECK(doc.line_at(12000) == "副歌重复行");
    }

    // 4. 端到端 8899 加密会话：发送含换行/引号歌词的 SetMediaInfo，并接收反向 Notify 播控事件
    {
        SocketHandle srv = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(srv != kInvalidSocket);
        net::set_reuse_addr(srv);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK(bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        CHECK(listen(srv, 2) == 0);
        socklen_t alen = sizeof(addr);
        CHECK(getsockname(srv, reinterpret_cast<struct sockaddr*>(&addr), &alen) == 0);
        const uint16_t port = ntohs(addr.sin_port);

        std::string captured_media_json;
        std::atomic<bool> server_done{false};
        std::thread fake_receiver([&]() {
            struct sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            SocketHandle cli = accept(srv, reinterpret_cast<struct sockaddr*>(&peer), &plen);
            if (cli == kInvalidSocket) return;
            net::set_socket_timeout(cli, SO_RCVTIMEO, 2000);
            const uint16_t client_port = ntohs(peer.sin_port);

            // 发送明文 Challenge
            const std::string ch = "13139245183899";
            auto ch_frame = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::Challenge), 1,
                                               std::vector<uint8_t>(ch.begin(), ch.end())});
            send(cli, reinterpret_cast<const char*>(ch_frame.data()), ch_frame.size(), 0);

            std::string key_str = cmd::derive_type1_auth_key("127.0.0.1", port, "127.0.0.1", client_port);
            const uint8_t* k16 = reinterpret_cast<const uint8_t*>(key_str.data());
            cmd::SafetyCipher rx_dec, tx_enc;
            rx_dec.init(k16, k16);
            tx_enc.init(k16, k16);
            bool safety_on = false;

            std::vector<uint8_t> acc;
            uint8_t tmp[4096];
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline && captured_media_json.empty()) {
                auto n = recv(cli, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
                if (n <= 0) break;
                acc.insert(acc.end(), tmp, tmp + n);
                std::vector<cmd::CmdFrame> frames;
                size_t used = cmd::decode_frames(acc.data(), acc.size(), frames);
                if (used) acc.erase(acc.begin(), acc.begin() + static_cast<ptrdiff_t>(used));
                for (const auto& f : frames) {
                    if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::GetVersion)) {
                        const std::string ver = "2.1.5112421";
                        auto vf = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::GetVersionAck), f.seq,
                                                     std::vector<uint8_t>(ver.begin(), ver.end())});
                        send(cli, reinterpret_cast<const char*>(vf.data()), vf.size(), 0);
                    } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::SafetyInfo)) {
                        auto env = cmd::encode_tlv_string("ack", "{\"result\":\"0\"}");
                        auto af = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::SafetyInfoAck), f.seq, env});
                        send(cli, reinterpret_cast<const char*>(af.data()), af.size(), 0);
                    } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::SafetyAuth)) {
                        auto plain = rx_dec.decrypt_blob(f.payload);
                        if (plain) {
                            auto ack_env = cmd::encode_tlv_string("ack", "{\"result\":\"0\"}");
                            auto ack_blob = tx_enc.encrypt_blob(ack_env);
                            auto af = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::SafetyAuthAck), f.seq, ack_blob});
                            send(cli, reinterpret_cast<const char*>(af.data()), af.size(), 0);

                            auto req_env = cmd::encode_tlv_string("cmd", "{\"authMsg\":\"peer_nonce\"}");
                            auto req_blob = tx_enc.encrypt_blob(req_env);
                            auto rf = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::SafetyAuth), 9, req_blob});
                            send(cli, reinterpret_cast<const char*>(rf.data()), rf.size(), 0);
                        }
                    } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::SafetyAuthAck)) {
                        (void)rx_dec.decrypt_blob(f.payload);
                        safety_on = true;
                    } else if (f.cmd_type == static_cast<uint16_t>(cmd::CmdType::SetMediaInfo)) {
                        auto plain = safety_on ? rx_dec.decrypt_blob(f.payload) : std::optional<std::vector<uint8_t>>(f.payload);
                        if (plain) {
                            captured_media_json.assign(plain->begin(), plain->end());
                            // 收到歌词后，反向发送加密的 key-next 和 key-seek 通知
                            auto n1 = tx_enc.encrypt_blob(cmd::encode_notify_control_key("key-next"));
                            auto f1 = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::Notify), 100, n1});
                            send(cli, reinterpret_cast<const char*>(f1.data()), f1.size(), 0);

                            auto n2 = tx_enc.encrypt_blob(cmd::encode_notify_seek_ms(12345ULL));
                            auto f2 = cmd::encode_frame({static_cast<uint16_t>(cmd::CmdType::Notify), 101, n2});
                            send(cli, reinterpret_cast<const char*>(f2.data()), f2.size(), 0);
                        }
                    } else if (safety_on && f.payload.size() >= 25) {
                        (void)rx_dec.decrypt_blob(f.payload);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            net::close_socket(cli);
            server_done.store(true);
        });

        cmd::CmdSession sess("127.0.0.1", port, "127.0.0.1");
        std::mutex ev_mu;
        std::condition_variable ev_cv;
        std::vector<cmd::RemoteControlEvent> received_evs;
        cmd::CmdSessionCallbacks cbs;
        cbs.on_remote_control = [&](const cmd::RemoteControlEvent& ev) {
            std::lock_guard<std::mutex> lk(ev_mu);
            received_evs.push_back(ev);
            ev_cv.notify_all();
        };
        CHECK(sess.connect(std::move(cbs)));
        CHECK(sess.is_safety_enabled());

        cmd::MediaInfoPayload info;
        info.title = "测试\"曲目\"";
        info.artist = "歌手A";
        info.album = "专辑B";
        info.duration_ms = 180000;
        info.position_ms = 12000;
        info.lyrics = "[00:01.00]第一行\n[00:10.00]第二行\"带引号\"";
        info.lyric_line = "第二行\"带引号\"";
        CHECK(sess.send_set_media_info(info));

        {
            std::unique_lock<std::mutex> lk(ev_mu);
            CHECK(ev_cv.wait_for(lk, std::chrono::seconds(2), [&] { return received_evs.size() >= 2; }));
        }
        sess.disconnect();
        fake_receiver.join();
        net::close_socket(srv);

        // 验证接收端收到的 JSON 合法且包含完整转义后的歌词与当前行
        const auto parsed = json::parse(captured_media_json);
        const auto* obj = std::get_if<json::Value::Object>(&parsed.data);
        CHECK(obj != nullptr);
        CHECK(std::get<std::string>(obj->at("mTitle").data) == info.title);
        CHECK(std::get<std::string>(obj->at("mLrc").data) == info.lyrics);
        CHECK(std::get<std::string>(obj->at("lyric_line").data) == info.lyric_line);

        CHECK(received_evs.size() == 2);
        CHECK(received_evs[0].action == cmd::RemoteControlAction::Next);
        CHECK(received_evs[1].action == cmd::RemoteControlAction::Seek && received_evs[1].position_ms == 12345ULL);
    }

    std::cout << "[PASS] test_remote_control_and_lrc_sync\n";
}

int main(int argc, char** argv) {
#if defined(__linux__)
    if (argc == 2 && std::string_view(argv[1]) == "--continuous-loopback-source") {
        return run_continuous_loopback_source();
    }
#endif
    net::init_network();
    if (argc == 2 && std::string_view(argv[1]) == "--dump-transport-fixtures") {
        test_transport_boundaries(true);
        return 0;
    }
    test_transport_boundaries();
    test_write_deadline_and_serialization();
    test_fixed_capture_sample_clock();
    test_aac_backpressure_timeline_and_truncated_adts();
    test_capture_conversion_and_fifo_limit();
    test_resampling_partitions_and_alias_rejection();
    test_mdns_fixtures_and_expiry();
    test_discovery_concurrent_lifecycle_and_manual_identity();
    test_discovery_burst_deadlines();
    test_manual_and_discovered_identity_revisions();
    test_cmd8899_codec_and_safety();
    test_remote_control_and_lrc_sync();
    test_master_clock_and_trendline();
    test_timer_service_multi_clients();
    test_wav_parser_bounds_and_formats();
    test_rtp_slow_receiver_isolation_and_session_id();
    test_rtsp_bind_failure_and_audio_config();
    test_lifecycle_cancellation_and_concurrent_start_stop();
#if defined(__linux__)
    test_linux_stalled_loopback_cancellation();
    test_linux_continuous_loopback_and_source_switch();
#endif
    test_slow_spectrum_consumer_keeps_capture_running();
    test_ws_shutdown_with_idle_client();
    test_disconnect_no_deadlock_and_teardown(false);
    test_disconnect_no_deadlock_and_teardown(true);
    test_rtsp_multiport_dual_tcp_negotiation();
    test_smooth_audio_anti_jitter_pipeline();
    test_mdns_qu_unicast_and_multi_service_discovery();
    std::cout << "\nALL MIPLAY_CORE TESTS PASSED!\n";
    return 0;
}
