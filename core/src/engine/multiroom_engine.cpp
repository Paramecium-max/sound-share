#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"

#include <algorithm>
#include <future>
#include <iomanip>
#include <sstream>

namespace miplay {

MultiRoomEngine::MultiRoomEngine(std::string bind_ip)
    : bind_ip_(bind_ip.empty() ? net::pick_default_lan_ipv4() : std::move(bind_ip)) {
    net::init_network();
    audio_pipeline_.set_source_mode(media::AudioSourceMode::TestTone, "");
}

MultiRoomEngine::~MultiRoomEngine() {
    // 析构前必须停止所有会话并等待全部连接线程与回调退出，确保零悬空 this 访问
    set_event_callback(nullptr);
    stop_multiroom();
    stop_discovery();
    std::vector<std::shared_ptr<SessionHandle>> leftovers;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, dev] : devices_) {
            detach_device_stream_locked(dev, leftovers);
        }
    }
    cleanup_detached(leftovers);
}

void MultiRoomEngine::set_event_callback(JsonEventCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    event_cb_ = std::move(cb);
}

void MultiRoomEngine::set_last_error(const std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = err;
}

std::string MultiRoomEngine::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

std::vector<media::LoopbackSourceInfo> MultiRoomEngine::list_loopback_sources() {
    return media::AudioPipeline::list_loopback_sources();
}

bool MultiRoomEngine::validate_audio_config(bool prefer_lpcm, uint32_t sample_rate,
                                            AudioStreamConfig& out_cfg, std::string& out_error) {
    if (sample_rate == 0) sample_rate = 48000;
    if (prefer_lpcm) {
        if (sample_rate != 48000 && sample_rate != 44100) {
            out_error = "WFD LPCM 仅支持 48000 Hz 或 44100 Hz 采样率";
            return false;
        }
        out_cfg.lpcm = true;
        out_cfg.sample_rate = sample_rate;
        out_cfg.channels = 2;
        out_cfg.frame_samples = (sample_rate == 44100) ? 441 : 480;
        return true;
    }
    // WFD 1.0 规范 AAC-LC 仅定义 48kHz Stereo (00000001)
    if (sample_rate != 48000) {
        out_error = "WFD AAC-LC 仅支持 48000 Hz 采样率（如需 44100 Hz 请切换为 LPCM）";
        return false;
    }
    out_cfg.lpcm = false;
    out_cfg.sample_rate = 48000;
    out_cfg.channels = 2;
    out_cfg.frame_samples = 1024;
    return true;
}

void MultiRoomEngine::set_bind_ip(const std::string& ip) {
    if (ip.empty()) return;
    bool was_disc = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        bind_ip_ = ip;
        was_disc = discovery_running_;
    }
    if (was_disc) {
        stop_discovery();
        start_discovery();
    }
    emit_state_changed();
}

std::string MultiRoomEngine::bind_ip() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bind_ip_;
}

bool MultiRoomEngine::start_discovery() {
    std::string ip;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (discovery_running_) return true;
        ip = bind_ip_;
    }
    bool ok = discovery_.start(ip, [this](const discovery::DiscoveredDevice& dev) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = devices_.find(dev.id);
            if (it == devices_.end()) {
                ManagedDevice md;
                md.info = dev;
                devices_[dev.id] = std::move(md);
                changed = true;
            } else {
                if (it->second.info.ip != dev.ip || it->second.info.cmd_port != dev.cmd_port) {
                    changed = true;
                }
                it->second.info.ip = dev.ip;
                it->second.info.cmd_port = dev.cmd_port;
                if (!dev.name.empty() && it->second.info.name != dev.name) {
                    it->second.info.name = dev.name;
                    changed = true;
                }
                it->second.info.last_seen_us = dev.last_seen_us;
            }
        }
        if (changed) emit_state_changed();
    });
    if (ok) {
        std::lock_guard<std::mutex> lock(mu_);
        discovery_running_ = true;
    }
    emit_state_changed();
    return ok;
}

void MultiRoomEngine::stop_discovery() {
    discovery_.stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        discovery_running_ = false;
    }
    emit_state_changed();
}

void MultiRoomEngine::refresh_discovery() {
    discovery_.trigger_query();
}

std::string MultiRoomEngine::add_manual_device(const std::string& name, const std::string& ip, uint16_t cmd_port) {
    std::string id = "manual_" + ip + "_" + std::to_string(cmd_port);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& md = devices_[id];
        md.info.id = id;
        md.info.name = name.empty() ? ("MiPlay-" + ip) : name;
        md.info.ip = ip;
        md.info.cmd_port = cmd_port;
        md.info.is_manual = true;
        md.info.device_type = 4;
        md.info.support_audio = true;
        md.info.last_seen_us = sync::monotonic_now_us();
    }
    emit_state_changed();
    return id;
}

bool MultiRoomEngine::remove_device(const std::string& device_id) {
    std::vector<std::shared_ptr<SessionHandle>> detached;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        detach_device_stream_locked(it->second, detached);
        devices_.erase(it);
    }
    cleanup_detached(detached);
    emit_state_changed();
    return true;
}

bool MultiRoomEngine::start_multiroom(bool prefer_lpcm, uint32_t sample_rate, uint32_t base_play_delay_ms) {
    std::vector<std::shared_ptr<SessionHandle>> detached;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        if (lifecycle_ == EngineLifecycle::Running) return true;
        lifecycle_ = EngineLifecycle::Starting;

        AudioStreamConfig cfg;
        std::string err;
        if (!validate_audio_config(prefer_lpcm, sample_rate, cfg, err)) {
            lifecycle_ = EngineLifecycle::Stopped;
            set_last_error(err);
            return false;
        }

        // 若请求 AAC 但当前平台无原生 AAC 编码器，同步更新单一实际音频配置为 LPCM
        aac_encoder_.shutdown();
        if (!cfg.lpcm) {
            if (!aac_encoder_.init(cfg.sample_rate, cfg.channels, 192000)) {
                cfg.lpcm = true;
                cfg.frame_samples = (cfg.sample_rate == 44100) ? 441 : 480;
            }
        }

        master_clock_.reset(cfg.sample_rate, base_play_delay_ms);
        multi_sender_.set_stream_type(
            cfg.lpcm ? media::TsAudioStreamType::WfdLpcm : media::TsAudioStreamType::AdtsAac);

        if (!timer_service_.start("0.0.0.0", 0)) {
            aac_encoder_.shutdown();
            lifecycle_ = EngineLifecycle::Stopped;
            set_last_error("TimerService UDP 校时服务启动失败");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            active_audio_cfg_ = cfg;
            multiroom_running_ = true;
            last_error_.clear();
            for (auto& [_, dev] : devices_) {
                if (dev.in_group) {
                    attach_device_stream_locked(dev, detached);
                }
            }
        }

        const bool use_aac = !cfg.lpcm && aac_encoder_.is_available();
        const uint32_t active_rate = cfg.sample_rate;
        audio_pipeline_.start(
            cfg.sample_rate,
            cfg.frame_samples,
            [this, use_aac, active_rate](const int16_t* stereo_pcm, size_t samples) {
                master_clock_.catch_up_to_now(static_cast<uint32_t>(samples));
                if (use_aac) {
                    // 由实际编码产出的每一帧独立申请 +1024 samples (+1920 ticks) 时间戳，杜绝 0 输出丢号与多输出重号
                    aac_encoder_.encode_pcm(stereo_pcm, samples,
                        [this](const uint8_t* adts, size_t len) {
                            auto ts = master_clock_.next_frame_timestamps(1024);
                            multi_sender_.broadcast_audio_frame(
                                adts, len, ts.linear_pts_90khz, ts.rtp_ts_90khz);
                        });
                } else {
                    auto ts = master_clock_.next_frame_timestamps(static_cast<uint32_t>(samples));
                    auto lpcm_frame = media::TsPacketizer::wrap_wfd_lpcm(stereo_pcm, samples, active_rate);
                    multi_sender_.broadcast_audio_frame(
                        lpcm_frame.data(), lpcm_frame.size(), ts.linear_pts_90khz, ts.rtp_ts_90khz);
                }
            },
            [this](const std::array<float, 16>& levels) {
                std::ostringstream oss;
                oss << "{\"type\":\"spectrum\",\"levels\":[";
                for (size_t i = 0; i < levels.size(); ++i) {
                    if (i) oss << ",";
                    oss << std::fixed << std::setprecision(3) << levels[i];
                }
                oss << "]}";
                emit_event_json(oss.str());
            },
            [this] { return multi_sender_.has_streaming_target(); });

        lifecycle_ = EngineLifecycle::Running;
    }
    cleanup_detached(detached);
    emit_state_changed();
    return true;
}

void MultiRoomEngine::stop_multiroom() {
    std::vector<std::shared_ptr<SessionHandle>> detached;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        if (lifecycle_ == EngineLifecycle::Stopped) return;
        lifecycle_ = EngineLifecycle::Stopping;
        {
            std::lock_guard<std::mutex> lock(mu_);
            multiroom_running_ = false;
            for (auto& [_, dev] : devices_) {
                detach_device_stream_locked(dev, detached);
            }
        }
        // 在持有 lifecycle_mu_（串行化启停）且释放 mu_（允许回调无死锁完成）的状态下彻底清理全部资源
        audio_pipeline_.stop();
        cleanup_detached(detached);
        multi_sender_.clear_all();
        aac_encoder_.shutdown();
        timer_service_.stop();
        lifecycle_ = EngineLifecycle::Stopped;
    }
    emit_state_changed();
}

bool MultiRoomEngine::attach_device_stream_locked(
    ManagedDevice& dev, std::vector<std::shared_ptr<SessionHandle>>& detached_out) {
    detach_device_stream_locked(dev, detached_out);

    const std::string dev_id = dev.info.id;
    const std::string dev_ip = dev.info.ip;
    const uint16_t cmd_port = dev.info.cmd_port;
    const int32_t init_vol = dev.volume;
    const std::string sender_ip = bind_ip_;
    const AudioStreamConfig cfg = active_audio_cfg_;
    const uint64_t sid = next_session_id_.fetch_add(1);
    dev.active_session_id = sid;
    dev.status_text = "启动 RTSP 监听与 8899 握手...";

    auto handle = std::make_shared<SessionHandle>();
    handle->session_id = sid;
    handle->device_id = dev_id;
    handle->rtsp_server = std::make_unique<rtsp::WfdRtspServer>();

    rtsp::RtspServerCallbacks rcbs;
    rcbs.on_stream_ready = [this, sid](const std::string& id, const std::string& peer_ip, SocketHandle data_sock) {
        int32_t bias = 0;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = devices_.find(id);
            if (it != devices_.end() && it->second.active_session_id == sid &&
                it->second.in_group && multiroom_running_) {
                it->second.rtsp_streaming = true;
                bias = it->second.delay_bias_ms;
                valid = true;
                // 按全局唯一 session_id 原子注册目标，防旧会话回调覆盖新目标
                multi_sender_.add_or_update_target(id, peer_ip, data_sock, bias, false, sid);
            }
        }
        if (valid) emit_state_changed();
    };
    rcbs.on_latency_report = [this, sid](const rtsp::RtspLatencyReport& rep) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = devices_.find(rep.device_id);
        if (it != devices_.end() && it->second.active_session_id == sid) {
            it->second.reported_latency_ms = rep.latency_ms;
        }
    };
    rcbs.on_session_closed = [this, sid](const std::string& id) {
        bool valid = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = devices_.find(id);
            if (it != devices_.end() && it->second.active_session_id == sid) {
                it->second.rtsp_streaming = false;
                valid = true;
            }
        }
        multi_sender_.remove_target(id, sid);
        if (valid) emit_state_changed();
    };
    rcbs.on_status = [this, sid](const std::string& id, const std::string& st) {
        bool valid = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = devices_.find(id);
            if (it != devices_.end() && it->second.active_session_id == sid) {
                it->second.status_text = st;
                valid = true;
            }
        }
        if (valid) emit_state_changed();
    };

    const bool rtsp_ok = handle->rtsp_server->start(
        dev_id,
        sender_ip,
        timer_service_.port(),
        master_clock_.session_origin_us(),
        cfg.lpcm,
        std::move(rcbs),
        cfg.sample_rate,
        test_rtsp_bind_port_.load());
    const uint16_t rtsp_port = rtsp_ok ? handle->rtsp_server->listen_port() : 0;

    // SS-06：RTSP 监听失败或端口无效时绝不继续建连或发送 Open
    if (!rtsp_ok || rtsp_port == 0) {
        handle->rtsp_server->stop();
        handle->rtsp_server.reset();
        dev.active_session_id = 0;
        dev.cmd_connected = false;
        dev.rtsp_streaming = false;
        dev.status_text = "RTSP 监听启动失败，未发起 8899 Open";
        return false;
    }

    auto cmd_sess = std::make_shared<cmd::CmdSession>(dev_ip, cmd_port);
    handle->cmd_session = cmd_sess;
    auto meta = audio_pipeline_.current_meta();

    // SS-01：使用可取消、可等待的托管连接线程（绝不 detach）
    handle->connect_thread = std::thread([this, handle, cmd_sess, dev_id, sender_ip, rtsp_port, init_vol, meta, sid]() {
        if (handle->cancelled.load()) return;

        cmd::CmdSessionCallbacks ccbs;
        ccbs.on_version = [this, dev_id, sid](const std::string& v) {
            bool valid = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end() && it->second.active_session_id == sid) {
                    it->second.remote_version = v;
                    valid = true;
                }
            }
            if (valid) emit_state_changed();
        };
        ccbs.on_state_changed = [this, dev_id, sid](bool conn) {
            bool valid = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end() && it->second.active_session_id == sid) {
                    it->second.cmd_connected = conn;
                    valid = true;
                }
            }
            if (valid) emit_state_changed();
        };
        ccbs.on_status = [this, dev_id, sid](const std::string& st) {
            bool valid = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                if (it != devices_.end() && it->second.active_session_id == sid) {
                    it->second.status_text = st;
                    valid = true;
                }
            }
            if (valid) emit_state_changed();
        };

        if (cmd_sess->connect(std::move(ccbs), &handle->cancelled)) {
            bool still_active = false;
            if (!handle->cancelled.load()) {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                still_active = (multiroom_running_ && it != devices_.end() &&
                                it->second.in_group && it->second.active_session_id == sid);
            }
            if (still_active) {
                cmd_sess->send_open_device(sender_ip, rtsp_port, 1);
                cmd_sess->send_set_media_info(meta.title, meta.artist, meta.album, meta.duration_ms);
                cmd_sess->send_set_volume(init_vol);
            } else {
                cmd_sess->close_and_disconnect();
            }
        }
    });

    dev.session = std::move(handle);
    return true;
}

void MultiRoomEngine::detach_device_stream_locked(
    ManagedDevice& dev, std::vector<std::shared_ptr<SessionHandle>>& detached_out) {
    dev.active_session_id = 0;
    if (dev.session) {
        dev.session->cancelled.store(true);
        detached_out.push_back(std::move(dev.session));
    }
    dev.cmd_connected = false;
    dev.rtsp_streaming = false;
    dev.status_text = "待机";
}

void MultiRoomEngine::cleanup_detached(std::vector<std::shared_ptr<SessionHandle>>& detached) {
    auto cleanup = [this](const std::shared_ptr<SessionHandle>& h) {
        if (!h) return;
        h->cancelled.store(true);
        // 按唯一 session_id 移除音频目标，绝不误删同一 device_id 的新会话
        multi_sender_.remove_target(h->device_id, h->session_id);
        // On interleaved connections, send RTSP teardown after the RTP writer stops.
        if (h->rtsp_server) {
            h->rtsp_server->stop();
        }
        if (h->cmd_session) {
            h->cmd_session->send_close_and_abort();
        }
        // 等连接线程完全退出后再由主线程安全 join CmdSession 内部线程并关闭 socket
        if (h->connect_thread.joinable() && h->connect_thread.get_id() != std::this_thread::get_id()) {
            h->connect_thread.join();
        }
        if (h->cmd_session) {
            h->cmd_session->disconnect();
        }
    };
    if (detached.size() == 1) {
        cleanup(detached.front());
    } else if (!detached.empty()) {
        // Independent receivers must not accumulate each other's shutdown timeouts.
        std::vector<std::future<void>> tasks;
        tasks.reserve(detached.size());
        for (const auto& h : detached) {
            tasks.push_back(std::async(std::launch::async, cleanup, h));
        }
        for (auto& task : tasks) task.get();
    }
    detached.clear();
}

bool MultiRoomEngine::add_device_to_group(const std::string& device_id) {
    std::vector<std::shared_ptr<SessionHandle>> detached;
    bool ok = true;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.in_group = true;
        if (multiroom_running_) {
            ok = attach_device_stream_locked(it->second, detached);
        }
    }
    cleanup_detached(detached);
    emit_state_changed();
    return ok;
}

bool MultiRoomEngine::remove_device_from_group(const std::string& device_id) {
    std::vector<std::shared_ptr<SessionHandle>> detached;
    {
        std::lock_guard<std::mutex> lc(lifecycle_mu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = devices_.find(device_id);
            if (it == devices_.end()) return false;
            it->second.in_group = false;
            detach_device_stream_locked(it->second, detached);
        }
        cleanup_detached(detached);
    }
    emit_state_changed();
    return true;
}

void MultiRoomEngine::set_base_play_delay_ms(uint32_t delay_ms) {
    delay_ms = std::clamp<uint32_t>(delay_ms, 100, 1000);
    master_clock_.set_base_play_delay_ms(delay_ms);
    emit_state_changed();
}

bool MultiRoomEngine::set_device_delay_bias(const std::string& device_id, int32_t delay_bias_ms) {
    delay_bias_ms = std::clamp(delay_bias_ms, -300, 300);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.delay_bias_ms = delay_bias_ms;
    }
    multi_sender_.set_target_delay_bias(device_id, delay_bias_ms);
    emit_state_changed();
    return true;
}

bool MultiRoomEngine::set_device_volume(const std::string& device_id, int32_t volume_0_100) {
    volume_0_100 = std::clamp(volume_0_100, 0, 100);
    std::shared_ptr<cmd::CmdSession> s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end()) return false;
        it->second.volume = volume_0_100;
        if (it->second.session) s = it->second.session->cmd_session;
    }
    if (s && s->is_connected()) s->send_set_volume(volume_0_100);
    emit_state_changed();
    return true;
}

void MultiRoomEngine::set_master_volume(int32_t volume_0_100) {
    volume_0_100 = std::clamp(volume_0_100, 0, 100);
    std::vector<std::pair<std::shared_ptr<cmd::CmdSession>, int32_t>> to_send;
    {
        std::lock_guard<std::mutex> lock(mu_);
        master_volume_ = volume_0_100;
        for (auto& [_, dev] : devices_) {
            if (!dev.in_group) continue;
            dev.volume = volume_0_100;
            if (dev.session && dev.session->cmd_session && dev.session->cmd_session->is_connected()) {
                to_send.emplace_back(dev.session->cmd_session, dev.volume);
            }
        }
    }
    for (auto& [s, v] : to_send) s->send_set_volume(v);
    emit_state_changed();
}

bool MultiRoomEngine::set_source_mode(media::AudioSourceMode mode, const std::string& param, std::string* err_out) {
    std::string err;
    if (!audio_pipeline_.set_source_mode(mode, param, &err)) {
        set_last_error(err);
        if (err_out) *err_out = err;
        emit_state_changed();
        return false;
    }
    set_last_error("");
    auto meta = audio_pipeline_.current_meta();
    std::vector<std::shared_ptr<cmd::CmdSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [_, dev] : devices_) {
            if (dev.session && dev.session->cmd_session && dev.session->cmd_session->is_connected()) {
                sessions.push_back(dev.session->cmd_session);
            }
        }
    }
    for (auto& s : sessions) {
        s->send_set_media_info(meta.title, meta.artist, meta.album, meta.duration_ms);
    }
    emit_state_changed();
    return true;
}

void MultiRoomEngine::push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples) {
    audio_pipeline_.push_external_pcm(stereo_pcm, frame_samples);
}

EngineStateSnapshot MultiRoomEngine::snapshot_state() const {
    EngineStateSnapshot snap;
    auto timer_map = timer_service_.snapshot_telemetry();
    auto rtp_nodes = multi_sender_.snapshot_nodes();
    std::map<std::string, uint64_t> sent_pkts;
    for (const auto& n : rtp_nodes) sent_pkts[n.device_id] = n.packets_sent;

    std::lock_guard<std::mutex> lock(mu_);
    snap.bind_ip = bind_ip_;
    snap.discovery_running = discovery_running_;
    snap.multiroom_running = multiroom_running_;
    snap.loopback_supported = media::AudioPipeline::is_loopback_supported();
    snap.timer_service_port = timer_service_.port();
    snap.session_time_offset_us = master_clock_.session_origin_us();
    snap.sample_rate = active_audio_cfg_.sample_rate;
    snap.base_play_delay_ms = master_clock_.base_play_delay_ms();
    snap.prefer_lpcm = active_audio_cfg_.lpcm;
    snap.master_volume = master_volume_;
    snap.source_mode = audio_pipeline_.source_mode();
    snap.source_param = audio_pipeline_.source_param();
    snap.last_error = last_error_;
    snap.track_meta = audio_pipeline_.current_meta();

    for (const auto& [_, dev] : devices_) {
        DeviceSnapshot ds;
        ds.info = dev.info;
        ds.in_group = dev.in_group;
        ds.cmd_connected = dev.cmd_connected;
        ds.rtsp_streaming = dev.rtsp_streaming;
        ds.volume = dev.volume;
        ds.delay_bias_ms = dev.delay_bias_ms;
        ds.reported_latency_ms = dev.reported_latency_ms;
        ds.remote_version = dev.remote_version;
        ds.status_text = dev.status_text;
        auto tit = timer_map.find(dev.info.ip);
        if (tit != timer_map.end()) {
            ds.sync_residual_us = tit->second.residual_error_us;
            ds.sync_packets = tit->second.packet_count;
        }
        auto pit = sent_pkts.find(dev.info.id);
        if (pit != sent_pkts.end()) {
            ds.audio_packets_sent = pit->second;
        }
        snap.devices.push_back(std::move(ds));
    }
    return snap;
}

std::string MultiRoomEngine::escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

std::string MultiRoomEngine::snapshot_state_json() const {
    auto snap = snapshot_state();
    std::ostringstream oss;
    oss << "{"
        << "\"bindIp\":\"" << escape_json(snap.bind_ip) << "\","
        << "\"discoveryRunning\":" << (snap.discovery_running ? "true" : "false") << ","
        << "\"multiroomRunning\":" << (snap.multiroom_running ? "true" : "false") << ","
        << "\"loopbackSupported\":" << (snap.loopback_supported ? "true" : "false") << ","
        << "\"timerServicePort\":" << snap.timer_service_port << ","
        << "\"sessionTimeOffsetUs\":" << snap.session_time_offset_us << ","
        << "\"sampleRate\":" << snap.sample_rate << ","
        << "\"basePlayDelayMs\":" << snap.base_play_delay_ms << ","
        << "\"codec\":\"" << (snap.prefer_lpcm ? "LPCM" : "AAC_LC") << "\","
        << "\"masterVolume\":" << snap.master_volume << ","
        << "\"sourceMode\":" << static_cast<int>(snap.source_mode) << ","
        << "\"sourceParam\":\"" << escape_json(snap.source_param) << "\","
        << "\"lastError\":\"" << escape_json(snap.last_error) << "\","
        << "\"track\":{"
            << "\"title\":\"" << escape_json(snap.track_meta.title) << "\","
            << "\"artist\":\"" << escape_json(snap.track_meta.artist) << "\","
            << "\"album\":\"" << escape_json(snap.track_meta.album) << "\","
            << "\"durationMs\":" << snap.track_meta.duration_ms << ","
            << "\"positionMs\":" << snap.track_meta.position_ms
        << "},"
        << "\"devices\":[";
    for (size_t i = 0; i < snap.devices.size(); ++i) {
        const auto& d = snap.devices[i];
        if (i) oss << ",";
        oss << "{"
            << "\"id\":\"" << escape_json(d.info.id) << "\","
            << "\"name\":\"" << escape_json(d.info.name) << "\","
            << "\"ip\":\"" << escape_json(d.info.ip) << "\","
            << "\"cmdPort\":" << d.info.cmd_port << ","
            << "\"mac\":\"" << escape_json(d.info.mac) << "\","
            << "\"deviceType\":" << d.info.device_type << ","
            << "\"roomName\":\"" << escape_json(d.info.room_name) << "\","
            << "\"isManual\":" << (d.info.is_manual ? "true" : "false") << ","
            << "\"inGroup\":" << (d.in_group ? "true" : "false") << ","
            << "\"cmdConnected\":" << (d.cmd_connected ? "true" : "false") << ","
            << "\"rtspStreaming\":" << (d.rtsp_streaming ? "true" : "false") << ","
            << "\"volume\":" << d.volume << ","
            << "\"delayBiasMs\":" << d.delay_bias_ms << ","
            << "\"reportedLatencyMs\":" << d.reported_latency_ms << ","
            << "\"syncResidualUs\":" << d.sync_residual_us << ","
            << "\"syncPackets\":" << d.sync_packets << ","
            << "\"audioPacketsSent\":" << d.audio_packets_sent << ","
            << "\"remoteVersion\":\"" << escape_json(d.remote_version) << "\","
            << "\"statusText\":\"" << escape_json(d.status_text) << "\""
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void MultiRoomEngine::emit_event_json(const std::string& json) {
    JsonEventCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = event_cb_;
    }
    if (cb) cb(json);
}

void MultiRoomEngine::emit_state_changed() {
    emit_event_json("{\"type\":\"state\",\"data\":" + snapshot_state_json() + "}");
}

} // namespace miplay
