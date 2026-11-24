#include "cmd/cmd_codec.h"
#include "crypto/mini_crypto.h"
#include "engine/multiroom_engine.h"
#include "media/aac_encoder.h"
#include "media/rtp_sender.h"
#include "media/ts_packetizer.h"
#include "platform/net_platform.h"
#include "rtsp/wfd_rtsp_server.h"
#include "sync/master_clock.h"
#include "sync/timer_service.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace miplay;

static void test_cmd8899_codec_and_safety() {
    cmd::CmdFrame f;
    f.cmd_type = static_cast<uint16_t>(cmd::CmdType::HeartBeat);
    f.seq = 0x0870; // 2160
    f.payload = {1, 2, 3, 4};

    auto wire = cmd::encode_frame(f);
    assert(wire.size() == 13);
    assert(wire[0] == 0x24);
    assert(wire[1] == 0x00 && wire[2] == 0x1A); // Big-Endian HeartBeat
    assert(wire[3] == 0x08 && wire[4] == 0x70); // Big-Endian seq=2160
    assert(wire[5] == 0 && wire[6] == 0 && wire[7] == 0 && wire[8] == 4); // Big-Endian u32 plen=4

    std::vector<cmd::CmdFrame> decoded;
    size_t consumed = cmd::decode_frames(wire.data(), wire.size(), decoded);
    assert(consumed == wire.size());
    assert(decoded.size() == 1);
    assert(decoded[0].cmd_type == 0x001A && decoded[0].seq == 0x0870 && decoded[0].payload == f.payload);

    // 验证 0x0028 -> 0x0029 Legacy Challenge HMAC-SHA1 计算
    std::string ch = "19936257588200";
    auto ch_resp = cmd::compute_legacy_challenge_response(std::vector<uint8_t>(ch.begin(), ch.end()));
    assert(ch_resp.size() == 40);

    // 验证 Type-1 Safety 密钥派生 (32B 小写 ASCII hex)
    std::string akey = cmd::derive_type1_auth_key("192.168.5.87", 8899, "192.168.5.84", 51326);
    assert(akey.size() == 32);

    // TLV 测试 (对齐实机 0x1400 SafetyInfo 格式)
    auto tlv = cmd::encode_tlv_string("cmd", "{\"aesKeyTypes\":\"1\"}");
    std::string k;
    auto val = cmd::decode_tlv_string(tlv, &k);
    assert(val.has_value() && k == "cmd" && *val == "{\"aesKeyTypes\":\"1\"}");

    // 状态化 AES-128-CBC Zero-Padding SafetyData 封装测试（9B 头 + 16B 密文 = 25 字节）
    cmd::SafetyCipher enc, dec;
    const uint8_t* key16 = reinterpret_cast<const uint8_t*>(akey.data());
    const uint8_t* iv16  = reinterpret_cast<const uint8_t*>(akey.data() + 16);
    enc.init(key16, iv16);
    dec.init(key16, iv16);

    auto enc_empty = enc.encrypt_blob({});
    assert(enc_empty.size() == 25);
    assert(enc_empty[0] == 0x00 && enc_empty[1] == 0x07 && enc_empty[2] == 0x01 && enc_empty[3] == 0xE0);
    assert(enc_empty[4] == 0x10);
    auto dec_empty = dec.decrypt_blob(enc_empty);
    assert(dec_empty.has_value() && dec_empty->empty());

    // 连续第二包测试状态化 IV 链（CBC 续链必须完全一致）
    std::vector<uint8_t> msg = {'w','f','d',':','/','/','1','9','2','.','1','6','8','.','5','.','8','4',':','4','0','0','0','0',0};
    auto enc_msg = enc.encrypt_blob(msg);
    auto dec_msg = dec.decrypt_blob(enc_msg);
    assert(dec_msg.has_value() && *dec_msg == msg);

    // ADTS 7 字节头封装测试（对齐 K90 0xFF 0xF9 0x4C）
    uint8_t dummy_aac[10] = {1,2,3,4,5,6,7,8,9,10};
    auto adts = media::AacEncoder::wrap_adts(dummy_aac, sizeof(dummy_aac), 48000, 2);
    assert(adts.size() == 17 && adts[0] == 0xFF && adts[1] == 0xF9 && adts[2] == 0x4C);

    std::cout << "[PASS] test_cmd8899_codec_and_safety\n";
}

static void test_master_clock_and_trendline() {
    sync::MasterMediaClock clk;
    clk.reset(48000, 350);
    auto f0 = clk.next_frame_timestamps(1024);
    auto f1 = clk.next_frame_timestamps(1024);
    assert(f1.rtp_ts_90khz - f0.rtp_ts_90khz == 1920);
    assert(f1.pts_90khz - f0.pts_90khz == 1920);

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
    assert(std::abs(last.offset_us - (5000.0 + 20.0 * 16.0)) < 100.0);
    assert(std::abs(last.drift_ppm - 20.0) < 2.0);
    std::cout << "[PASS] test_master_clock_and_trendline (drift=" << last.drift_ppm << " ppm)\n";
}

static void test_timer_service_multi_clients() {
    net::init_network();
    sync::TimerService srv;
    assert(srv.start("127.0.0.1", 0));
    uint16_t port = srv.port();
    assert(port > 0);

    for (int dev = 0; dev < 2; ++dev) {
        SocketHandle cli = socket(AF_INET, SOCK_DGRAM, 0);
        assert(cli != kInvalidSocket);
        struct timeval tv{1, 0};
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
        assert(n == 40);
        assert(resp.client_synced_us == req.client_synced_us);
        assert(resp.client_local_us == req.client_local_us);
        assert(resp.seq == req.seq);
        assert(resp.server_recv_us > 0 && resp.server_send_us >= resp.server_recv_us);
        net::close_socket(cli);
    }
    auto tele = srv.snapshot_telemetry();
    assert(tele.count("127.0.0.1") == 1 && tele["127.0.0.1"].packet_count == 2);
    srv.stop();
    std::cout << "[PASS] test_timer_service_multi_clients (port=" << port << ")\n";
}

static void test_mpegts_and_dual_device_multiroom_sync() {
    std::string m1 = rtsp::WfdRtspServer::build_m1_options("192.168.5.84", 44274, 1);
    assert(m1.find("wfd_timer_server_port:3232236884:44274") != std::string::npos);

    media::MultiAudioSender sender;
    sender.set_stream_type(media::TsAudioStreamType::WfdLpcm);

    auto make_socket_pair = []() -> std::pair<SocketHandle, SocketHandle> {
        SocketHandle lfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        a.sin_port = 0;
        bind(lfd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        listen(lfd, 1);
        socklen_t alen = sizeof(a);
        getsockname(lfd, reinterpret_cast<struct sockaddr*>(&a), &alen);
        SocketHandle cfd = socket(AF_INET, SOCK_STREAM, 0);
        connect(cfd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        SocketHandle sfd = accept(lfd, nullptr, nullptr);
        net::close_socket(lfd);
        return {sfd, cfd};
    };

    auto [s1, r1] = make_socket_pair();
    auto [s2, r2] = make_socket_pair();

    sender.add_or_update_target("speaker_A", "127.0.0.1", s1, 0);
    sender.add_or_update_target("speaker_B", "127.0.0.1", s2, 0);

    std::vector<int16_t> pcm(480 * 2, 1234);
    auto lpcm = media::TsPacketizer::wrap_wfd_lpcm(pcm.data(), 480);
    sender.broadcast_audio_frame(lpcm.data(), lpcm.size(), 2150366ULL, 2150366U);

    uint8_t b1[2048]{}, b2[2048]{};
    auto n1 = recv(r1, reinterpret_cast<char*>(b1), sizeof(b1), 0);
    auto n2 = recv(r2, reinterpret_cast<char*>(b2), sizeof(b2), 0);
    assert(n1 > 16 && n1 == n2);
    assert(std::memcmp(b1, b2, static_cast<size_t>(n1)) == 0);
    assert(b1[0] == 0x24 && b1[1] == 0x00);
    assert(b1[4] == 0x80 && b1[5] == 0xA1);
    assert(b1[12] == 0xDE && b1[13] == 0xAD && b1[14] == 0xBE && b1[15] == 0xEF);
    assert(b1[16] == 0x47);

    net::close_socket(r1);
    net::close_socket(r2);
    sender.clear_all();
    std::cout << "[PASS] test_mpegts_and_dual_device_multiroom_sync (len=" << n1 << "B)\n";
}

static void test_volume_codec_and_master_semantics() {
    auto v0 = cmd::encode_u32_be(0);
    auto v5 = cmd::encode_u32_be(5);
    auto v41 = cmd::encode_u32_be(41);
    auto v100 = cmd::encode_u32_be(100);
    assert(v0 == (std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00}));
    assert(v5 == (std::vector<uint8_t>{0x00, 0x00, 0x00, 0x05}));
    assert(v41 == (std::vector<uint8_t>{0x00, 0x00, 0x00, 0x29}));
    assert(v100 == (std::vector<uint8_t>{0x00, 0x00, 0x00, 0x64}));

    // 验证全屋统一主音量语义（不再使用旧的 delta 累加）：
    // A(组内 20%)、B(组内 80%)、C(组外 55%) -> 主音量 70 -> 40 -> 70，组内统一到目标值，组外不变
    MultiRoomEngine eng("127.0.0.1");
    std::string idA = eng.add_manual_device("DevA", "127.0.0.1", 18891);
    std::string idB = eng.add_manual_device("DevB", "127.0.0.1", 18892);
    std::string idC = eng.add_manual_device("DevC", "127.0.0.1", 18893);
    eng.add_device_to_group(idA);
    eng.add_device_to_group(idB);
    eng.set_device_volume(idA, 20);
    eng.set_device_volume(idB, 80);
    eng.set_device_volume(idC, 55);

    auto vol_of = [&](const std::string& id) {
        auto snap = eng.snapshot_state();
        for (const auto& d : snap.devices) {
            if (d.info.id == id) return d.volume;
        }
        return -1;
    };

    eng.set_master_volume(40);
    assert(vol_of(idA) == 40 && vol_of(idB) == 40 && vol_of(idC) == 55);

    eng.set_master_volume(70);
    assert(vol_of(idA) == 70 && vol_of(idB) == 70 && vol_of(idC) == 55);

    // 单独调 A 到 41%，B 和 C 必须保持原值
    eng.set_device_volume(idA, 41);
    assert(vol_of(idA) == 41 && vol_of(idB) == 70 && vol_of(idC) == 55);

    std::cout << "[PASS] test_volume_codec_and_master_semantics\n";
}

int main() {
    test_cmd8899_codec_and_safety();
    test_master_clock_and_trendline();
    test_timer_service_multi_clients();
    test_mpegts_and_dual_device_multiroom_sync();
    test_volume_codec_and_master_semantics();
    std::cout << "\nALL MIPLAY_CORE TESTS PASSED!\n";
    return 0;
}
