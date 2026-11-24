#pragma once

#include "cmd/cmd_session.h"
#include "discovery/mdns_discovery.h"
#include "media/aac_encoder.h"
#include "media/audio_pipeline.h"
#include "media/rtp_sender.h"
#include "rtsp/wfd_rtsp_server.h"
#include "sync/master_clock.h"
#include "sync/timer_service.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miplay {

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
    bool discovery_running = false;
    bool multiroom_running = false;
    uint16_t timer_service_port = 0;
    uint64_t session_time_offset_us = 0;
    uint32_t sample_rate = 48000;
    uint32_t base_play_delay_ms = 350;
    bool prefer_lpcm = false;
    int32_t master_volume = 70;
    media::AudioSourceMode source_mode = media::AudioSourceMode::TestTone;
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
    void refresh_discovery();

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

    void set_source_mode(media::AudioSourceMode mode, const std::string& param = "");
    void push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples);

    EngineStateSnapshot snapshot_state() const;
    std::string snapshot_state_json() const;

private:
    struct ManagedDevice {
        discovery::DiscoveredDevice info;
        bool in_group = false;
        bool cmd_connected = false;
        bool rtsp_streaming = false;
        int32_t volume = 65;
        int32_t delay_bias_ms = 0;
        int32_t reported_latency_ms = 0;
        std::string remote_version;
        std::string status_text = "待机";
        uint64_t attach_gen = 0;
        std::shared_ptr<cmd::CmdSession> cmd_session;
        std::unique_ptr<rtsp::WfdRtspServer> rtsp_server;
    };

    void attach_device_stream_locked(ManagedDevice& dev);
    void detach_device_stream_locked(ManagedDevice& dev);
    void update_device_status(const std::string& dev_id, const std::string& status);
    void emit_event_json(const std::string& json);
    void emit_state_changed();
    static std::string escape_json(const std::string& s);

    mutable std::mutex mu_;
    std::string bind_ip_;
    bool discovery_running_ = false;
    bool multiroom_running_ = false;
    bool prefer_lpcm_ = false;
    uint32_t sample_rate_ = 48000;
    int32_t master_volume_ = 70;
    std::atomic<uint64_t> last_state_emit_us_{0};

    discovery::MdnsDiscovery discovery_;
    sync::MasterMediaClock master_clock_;
    sync::TimerService timer_service_;
    media::MultiAudioSender multi_sender_;
    media::AacEncoder aac_encoder_;
    media::AudioPipeline audio_pipeline_;

    std::map<std::string, ManagedDevice> devices_;
    std::mutex cb_mu_;
    JsonEventCallback event_cb_;
};

} // namespace miplay
