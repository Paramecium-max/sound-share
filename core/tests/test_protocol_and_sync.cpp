#include "cmd/cmd_codec.h"
#include "crypto/mini_crypto.h"
#include "engine/multiroom_engine.h"
#include "media/aac_encoder.h"
#include "media/audio_pipeline.h"
#include "media/rtp_sender.h"
#include "media/ts_packetizer.h"
#include "platform/net_platform.h"
#include "rtsp/wfd_rtsp_server.h"
#include "sidecar/ws_server.h"
#include "sync/master_clock.h"
#include "sync/timer_service.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    auto tele = srv.snapshot_telemetry();
    CHECK(tele.count("127.0.0.1") == 1 && tele["127.0.0.1"].packet_count == 2);
    srv.stop();
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
        [&](const std::array<float, 16>&) {
            ++spectra;
            if (spectrum_entered.exchange(true)) return;
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
        std::atomic<SocketHandle> ready_sock{kInvalidSocket};
        std::mutex status_mu;
        std::string last_m7_status;
        rtsp::WfdRtspServer srv;
        rtsp::RtspServerCallbacks cbs;
        cbs.on_stream_ready = [&](const std::string&, const std::string&, SocketHandle data_sock) {
            ready_sock.store(data_sock);
            ready_count.fetch_add(1);
            const char marker[] = "$AUDIO_MULTIPORT_OK";
            net::send_all(data_sock, reinterpret_cast<const uint8_t*>(marker), sizeof(marker) - 1);
        };
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
        for (int i = 0; i < 3; ++i) {
            uint64_t pts = 90000ULL + static_cast<uint64_t>(i) * 900ULL;
            sender.broadcast_audio_frame(payload.data(), payload.size(), pts, static_cast<uint32_t>(pts));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sender.set_target_delay_bias("room_smooth", 0);
        for (int i = 3; i < 20; ++i) {
            uint64_t pts = 90000ULL + static_cast<uint64_t>(i) * 900ULL;
            sender.broadcast_audio_frame(payload.data(), payload.size(), pts, static_cast<uint32_t>(pts));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        sender.clear_all();

        std::vector<uint8_t> stream;
        uint8_t rbuf[4096];
        for (;;) {
            auto n = recv(s_rx, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0);
            if (n <= 0) break;
            stream.insert(stream.end(), rbuf, rbuf + n);
        }
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

int main() {
    test_cmd8899_codec_and_safety();
    test_master_clock_and_trendline();
    test_timer_service_multi_clients();
    test_wav_parser_bounds_and_formats();
    test_rtp_slow_receiver_isolation_and_session_id();
    test_rtsp_bind_failure_and_audio_config();
    test_lifecycle_cancellation_and_concurrent_start_stop();
#if defined(__linux__)
    test_linux_stalled_loopback_cancellation();
#endif
    test_slow_spectrum_consumer_keeps_capture_running();
    test_ws_shutdown_with_idle_client();
    test_disconnect_no_deadlock_and_teardown(false);
    test_disconnect_no_deadlock_and_teardown(true);
    test_rtsp_multiport_dual_tcp_negotiation();
    test_smooth_audio_anti_jitter_pipeline();
    std::cout << "\nALL MIPLAY_CORE TESTS PASSED!\n";
    return 0;
}
