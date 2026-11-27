#pragma once

#include "cmd/cmd_codec.h"
#include "platform/net_platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace miplay::cmd {

struct MediaInfoPayload {
    std::string title;
    std::string artist;
    std::string album;
    uint64_t duration_ms = 0;
    uint64_t position_ms = 0;
    std::string lyrics;
    std::string lyric_line;
    std::string source_name = "MiPlay MultiRoom";
    int meta_change_type = 0;
};

struct CmdSessionCallbacks {
    std::function<void(const std::string& remote_version)> on_version;
    std::function<void(const CmdFrame& frame)> on_frame;
    std::function<void(const RemoteControlEvent& ev)> on_remote_control;
    std::function<void(bool connected)> on_state_changed;
    std::function<void(const std::string& status)> on_status;
};

// 单设备 TCP 8899 控制会话管理（完整支持 0x0028/0x0029 挑战认证 + 0x1400~0x1403 Safety 双向加密）
class CmdSession {
public:
    enum class HandshakeStage {
        Idle,
        WaitingChallenge,
        NegotiatingSafety,
        AuthenticatingSafety,
        ReadySafety,
        ReadyLegacy,
        Failed
    };

    CmdSession(std::string ip, uint16_t port = 8899, std::string bind_ip = {});
    ~CmdSession();

    bool connect(CmdSessionCallbacks cbs, const std::atomic<bool>* cancelled = nullptr);
    void abort();
    void disconnect();
    void close_and_disconnect();
    void send_close_and_abort();
    bool is_connected() const { return connected_.load(); }
    bool is_safety_enabled() const { return safety_active_.load(); }
    HandshakeStage handshake_stage() const;

    bool send_get_version(const std::string& ver = "3.1.6030516");
    bool send_get_device_info();
    bool send_set_local_device_info(const std::string& source_name = "MiPlay MultiRoom");
    bool send_set_play_source(int play_source = 1);
    bool send_open_device(const std::string& sender_ip, uint16_t rtsp_port, int mirror_mode = 1);
    bool send_close_device();
    bool send_resume();
    bool send_pause();
    bool send_heartbeat();
    bool send_set_volume(int volume_0_100);
    bool send_set_position_ms(uint64_t position_ms);
    bool send_set_media_info(const std::string& title, const std::string& artist,
                             const std::string& album, uint64_t duration_ms);
    bool send_set_media_info(const MediaInfoPayload& info);
    void queue_media_info(const MediaInfoPayload& info);
    void queue_position_ms(uint64_t position_ms);

private:
    bool send_raw_locked(uint16_t cmd_type, uint16_t seq, const std::vector<uint8_t>& payload);
    bool send_cmd(CmdType type, const std::vector<uint8_t>& raw_payload, bool allow_encrypt = true);
    void handle_incoming_frame(const CmdFrame& f);
    void emit_status(const std::string& s);
    void mark_disconnected(const std::string& reason);
    void recv_loop();
    void heartbeat_loop();

    std::string ip_;
    uint16_t port_;
    std::string bind_ip_;
    std::string local_ip_;
    uint16_t local_port_ = 0;

    std::mutex sock_mu_;
    SocketHandle sock_ = kInvalidSocket;
    std::atomic<bool> aborted_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint16_t> seq_{1};

    std::mutex send_mu_;
    std::unique_ptr<net::ConnectionWriter> writer_;

    std::mutex updates_mu_;
    std::condition_variable updates_cv_;
    std::optional<MediaInfoPayload> pending_media_;
    std::optional<uint64_t> pending_position_;

    void wake_update_worker();
    mutable std::mutex auth_mu_;
    std::condition_variable auth_cv_;

    HandshakeStage stage_ = HandshakeStage::Idle;
    bool legacy_auth_done_ = false;
    bool version_ack_received_ = false;
    bool safety_info_ack_received_ = false;
    bool safety_info_ok_ = false;
    bool peer_challenge_acked_ = false;
    bool local_challenge_verified_ = false;
    bool peer_reset_ = false;
    std::string failure_reason_;
    std::atomic<bool> safety_active_{false};

    std::string auth_key_;
    std::string local_auth_msg_;
    SafetyCipher enc_cipher_;
    SafetyCipher dec_cipher_;
    bool dec_cipher_locked_ = false;
    SafetyCipher dec_cand_type1_;
    SafetyCipher dec_cand_type2_;

    CmdSessionCallbacks cbs_;
    std::thread recv_th_;
    std::thread hb_th_;
};

} // namespace miplay::cmd
