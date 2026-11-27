#pragma once

#include "cmd/cmd_session.h"
#include "discovery/mdns_discovery.h"
#include "media/aac_encoder.h"
#include "media/audio_pipeline.h"
#include "media/rtp_sender.h"
#include "rtsp/wfd_rtsp_server.h"
#include "sync/master_clock.h"
#include "sync/timer_service.h"
#include "util/json.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay {

struct AudioStreamConfig {
    bool lpcm = false;
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t frame_samples = 1024;
};

struct DeviceSnapshot {
    discovery::DiscoveredDevice info;
    bool in_group = false;
    bool cmd_connected = false;
    bool rtsp_streaming = false;
    int32_t volume = 65;
    int32_t delay_bias_ms = 0;
    int32_t reported_latency_ms = 0;
    int64_t sync_residual_us = 0;
    uint64_t sync_packets = 0;
    uint64_t audio_packets_sent = 0;
    std::string remote_version;
    std::string status_text;
};

struct EngineStateSnapshot {
    std::string bind_ip;
    bool auto_bind = true;
    bool discovery_running = false;
    bool multiroom_running = false;
    bool loopback_supported = true;
    uint16_t timer_service_port = 0;
    uint64_t session_time_offset_us = 0;
    uint32_t sample_rate = 48000;
    uint32_t base_play_delay_ms = 350;
    bool prefer_lpcm = false;
    int32_t master_volume = 70;
    media::AudioSourceMode source_mode = media::AudioSourceMode::TestTone;
    std::string source_param;
    std::string last_error;
    media::AudioTrackMeta track_meta;
    std::vector<DeviceSnapshot> devices;
};

using JsonEventCallback = std::function<void(const std::string& json_event)>;

class MultiRoomEngine {
public:
    explicit MultiRoomEngine(std::string bind_ip = "");
    ~MultiRoomEngine();

    void set_event_callback(JsonEventCallback cb);
    void set_bind_ip(const std::string& ip);
    std::string bind_ip() const;

    bool start_discovery();
    void stop_discovery();
    void refresh_discovery(bool automatic = false);

    std::string add_manual_device(const std::string& name, const std::string& ip, uint16_t cmd_port = 8899);
    bool remove_device(const std::string& device_id);

    bool start_multiroom(bool prefer_lpcm = false, uint32_t sample_rate = 48000, uint32_t base_play_delay_ms = 350);
    void stop_multiroom();

    bool add_device_to_group(const std::string& device_id);
    bool remove_device_from_group(const std::string& device_id);

    void set_base_play_delay_ms(uint32_t delay_ms);
    bool set_device_delay_bias(const std::string& device_id, int32_t delay_bias_ms);
    bool set_device_volume(const std::string& device_id, int32_t volume_0_100);
    void set_master_volume(int32_t volume_0_100);

    bool set_source_mode(media::AudioSourceMode mode, const std::string& param = "", std::string* err_out = nullptr);
    void push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples);
    void set_media_metadata(const cmd::MediaInfoPayload& info, bool broadcast = true);
    void set_media_position_ms(uint64_t position_ms, bool broadcast = true);
    bool load_lrc_file(const std::string& path, std::string* err_out = nullptr);
    void set_remote_control_handled_by_host(bool handled);
    void tick_media_sync();
    void seek_wav_ms(uint64_t position) { audio_pipeline_.seek_wav_ms(position); }
    void set_wav_paused(bool paused) { audio_pipeline_.set_wav_paused(paused); }
    std::string last_error() const;

    static std::vector<media::LoopbackSourceInfo> list_loopback_sources();
    static bool validate_audio_config(bool prefer_lpcm, uint32_t sample_rate,
                                      AudioStreamConfig& out_cfg, std::string& out_error);

    // 仅供单元测试注入 RTSP 监听端口冲突
    void set_rtsp_bind_port_for_test(uint16_t port) { test_rtsp_bind_port_.store(port); }

    EngineStateSnapshot snapshot_state() const;
    void publish_discovery_snapshot_for_test(const discovery::DiscoveredDevice& device) {
        on_discovery_update(device);
    }

    json::Value snapshot_state_value() const;
    std::string snapshot_state_json() const;

private:
    enum class EngineLifecycle {
        Stopped,
        Starting,
        Running,
        Stopping
    };

    // 可取消、可等待的设备会话句柄（彻底替代 detached 裸 this 线程）
    struct SessionHandle {
        uint64_t session_id = 0;
        std::string device_id;
        std::string local_ip;
        std::string remote_ip;
        uint16_t remote_port = 0;
        std::atomic<bool> cancelled{false};
        std::shared_ptr<cmd::CmdSession> cmd_session;
        std::unique_ptr<rtsp::WfdRtspServer> rtsp_server;
        std::thread connect_thread;
    };

    struct ManagedDevice {
        discovery::DiscoveredDevice info;
        std::string discovery_key;
        std::string manual_name;
        bool in_group = false;
        bool cmd_connected = false;
        bool rtsp_streaming = false;
        int32_t volume = 65;
        int32_t delay_bias_ms = 0;
        int32_t reported_latency_ms = 0;
        std::string remote_version;
        std::string status_text = "待机";
        uint64_t active_session_id = 0;
        std::shared_ptr<SessionHandle> session;
    };

    bool start_discovery_locked();
    void on_discovery_update(const discovery::DiscoveredDevice& device);
    void reconcile_streams();
    std::string sender_ip_for(const discovery::DiscoveredDevice& device) const;
    bool attach_device_stream_locked(ManagedDevice& dev, std::vector<std::shared_ptr<SessionHandle>>& detached_out);
    void detach_device_stream_locked(ManagedDevice& dev, std::vector<std::shared_ptr<SessionHandle>>& detached_out);
    void cleanup_failed_sessions();
    void cleanup_detached(std::vector<std::shared_ptr<SessionHandle>>& detached, bool graceful = true);
    void handle_remote_control(const std::string& device_id, uint64_t session_id, const cmd::RemoteControlEvent& ev);
    cmd::MediaInfoPayload build_effective_media_info_locked() const;
    void broadcast_media_info(const cmd::MediaInfoPayload& info);
    void broadcast_position_ms(uint64_t position_ms);
    void set_last_error(const std::string& err);
    void emit_event_json(const std::string& json);
    void emit_state_changed();

    // 单独串行化引擎启停状态机，不阻塞回调访问状态锁 mu_
    std::mutex lifecycle_mu_;
    EngineLifecycle lifecycle_ = EngineLifecycle::Stopped;

    mutable std::mutex mu_;
    std::mutex discovery_lifecycle_mu_;
    std::string bind_ip_;
    bool auto_bind_ = true;
    bool discovery_running_ = false;
    bool multiroom_running_ = false;
    AudioStreamConfig active_audio_cfg_;
    int32_t master_volume_ = 70;
    std::string last_error_;
    std::atomic<uint64_t> next_session_id_{1};
    std::atomic<uint16_t> test_rtsp_bind_port_{0};

    discovery::MdnsDiscovery discovery_;
    sync::MasterMediaClock master_clock_;
    sync::TimerService timer_service_;
    media::MultiAudioSender multi_sender_;
    media::AacEncoder aac_encoder_;
    media::AudioPipeline audio_pipeline_;

    std::map<std::string, ManagedDevice> devices_;
    bool has_external_media_ = false;
    cmd::MediaInfoPayload external_media_;
    media::LrcDocument external_lrc_;
    bool external_media_paused_ = false;
    std::string last_broadcast_media_sig_;
    std::atomic<bool> remote_control_handled_by_host_{false};
    std::mutex cb_mu_;
    JsonEventCallback event_cb_;
    std::mutex failed_mu_;
    std::condition_variable failed_cv_;
    bool stop_failed_worker_ = false;
    std::vector<std::shared_ptr<SessionHandle>> failed_sessions_;
    std::thread failed_worker_;
};

} // namespace miplay
