#include "engine/multiroom_engine.h"
#include "platform/net_platform.h"

#include <algorithm>
#include <future>
#include <iomanip>
#include <sstream>

namespace miplay {

MultiRoomEngine::MultiRoomEngine(std::string bind_ip)
    : bind_ip_(std::move(bind_ip)), auto_bind_(bind_ip_.empty()) {
    net::init_network();
    if (auto_bind_) bind_ip_ = net::pick_default_lan_ipv4();
    audio_pipeline_.set_source_mode(media::AudioSourceMode::TestTone, "");
    failed_worker_ = std::thread([this]() { cleanup_failed_sessions(); });
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
    {
        std::lock_guard<std::mutex> lk(failed_mu_);
        stop_failed_worker_ = true;
    }
    failed_cv_.notify_one();
    if (failed_worker_.joinable()) {
        failed_worker_.join();
    }
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
    {
        std::lock_guard lifecycle(discovery_lifecycle_mu_);
        bool restart;
        {
            std::lock_guard lock(mu_);
            const bool automatic = ip.empty();
            if (auto_bind_ == automatic && (automatic || bind_ip_ == ip)) return;
            auto_bind_ = automatic;
            bind_ip_ = automatic ? net::pick_default_lan_ipv4() : ip;
            restart = discovery_running_;
            discovery_running_ = false;
        }
        if (restart) {
            discovery_.stop();
            start_discovery_locked();
        }
    }
    reconcile_streams();
    emit_state_changed();
}

std::string MultiRoomEngine::bind_ip() const {
    std::lock_guard lock(mu_);
    return bind_ip_;
}

bool MultiRoomEngine::start_discovery_locked() {
    std::string ip;
    {
        std::lock_guard lock(mu_);
        if (discovery_running_) return true;
        ip = auto_bind_ ? "" : bind_ip_;
    }
    const bool ok = discovery_.start(ip, [this](const auto& device) { on_discovery_update(device); },
                                     [this] { reconcile_streams(); });
    {
        std::lock_guard lock(mu_);
        discovery_running_ = ok;
    }
    return ok;
}

bool MultiRoomEngine::start_discovery() {
    bool ok;
    {
        std::lock_guard lifecycle(discovery_lifecycle_mu_);
        ok = start_discovery_locked();
    }
    emit_state_changed();
    return ok;
}

void MultiRoomEngine::stop_discovery() {
    {
        std::lock_guard lifecycle(discovery_lifecycle_mu_);
        discovery_.stop();
        std::lock_guard lock(mu_);
        discovery_running_ = false;
    }
    emit_state_changed();
}

void MultiRoomEngine::refresh_discovery(bool automatic) {
    std::lock_guard lifecycle(discovery_lifecycle_mu_);
    discovery_.trigger_query(automatic);
}

void MultiRoomEngine::on_discovery_update(const discovery::DiscoveredDevice& device) {
    if (reject_self_ip_.load() && net::is_local_ipv4(device.ip)) {
        return;
    }
    bool changed = false;
    bool reconcile = false;
    {
        std::lock_guard lock(mu_);
        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const auto& entry) {
            return entry.second.discovery_key == device.id;
        });
        if (it == devices_.end()) {
            if (!device.online) return;
            it = std::find_if(devices_.begin(), devices_.end(), [&](const auto& entry) {
                return entry.second.info.ip == device.ip && entry.second.info.cmd_port == device.cmd_port;
            });
        }
        if (it == devices_.end()) {
            ManagedDevice md;
            md.discovery_key = device.id;
            md.info = device;
            devices_.emplace(device.id, std::move(md));
            changed = true;
        } else {
            auto& md = it->second;
            if (md.discovery_key == device.id && device.revision <= md.info.revision) return;
            const auto old = md.info;
            md.discovery_key = device.id;
            md.info = device;
            md.info.id = it->first;
            md.info.is_manual = old.is_manual;
            if (old.is_manual) {
                md.info.online = true;
                if (!md.manual_name.empty()) md.info.name = md.manual_name;
            }
            reconcile = old.ip != md.info.ip || old.cmd_port != md.info.cmd_port || old.online != md.info.online;
            changed = reconcile || old.name != md.info.name || old.mac != md.info.mac ||
                      old.room_name != md.info.room_name || old.device_type != md.info.device_type;
        }
    }
    if (reconcile) reconcile_streams();
    if (changed) emit_state_changed();
}

std::string MultiRoomEngine::sender_ip_for(const discovery::DiscoveredDevice& device) const {
    if (auto_bind_) return net::local_ipv4_for_peer(device.ip, device.cmd_port);
    return net::is_local_ipv4(bind_ip_) ? bind_ip_ : "";
}

void MultiRoomEngine::reconcile_streams() {
    std::lock_guard lifecycle(lifecycle_mu_);
    std::vector<std::shared_ptr<SessionHandle>> detached;
    std::vector<std::string> restart;
    bool changed = false;
    {
        std::lock_guard lock(mu_);
        if (auto_bind_) {
            auto available = net::pick_default_lan_ipv4();
            changed = bind_ip_ != available;
            bind_ip_ = std::move(available);
        }
        if (reject_self_ip_.load()) {
            for (auto it = devices_.begin(); it != devices_.end();) {
                if (net::is_local_ipv4(it->second.info.ip)) {
                    detach_device_stream_locked(it->second, detached);
                    it = devices_.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        if (multiroom_running_) {
            for (auto& [id, dev] : devices_) {
                if (!dev.in_group) continue;
                auto local_ip = sender_ip_for(dev.info);
                if (dev.session && (dev.session->local_ip != local_ip || dev.session->remote_ip != dev.info.ip ||
                                    dev.session->remote_port != dev.info.cmd_port || !dev.info.online)) {
                    detach_device_stream_locked(dev, detached);
                    changed = true;
                }
                if (!dev.session && dev.info.online) restart.push_back(id);
            }
        }
    }
    cleanup_detached(detached);
    {
        std::lock_guard lock(mu_);
        for (const auto& id : restart) {
            if (auto it = devices_.find(id); it != devices_.end() && it->second.in_group && it->second.info.online) {
                attach_device_stream_locked(it->second, detached);
                changed = true;
            }
        }
    }
    cleanup_detached(detached);
    if (changed) emit_state_changed();
}

std::string MultiRoomEngine::add_manual_device(const std::string& name, const std::string& ip, uint16_t cmd_port) {
    std::string id;
    {
        std::lock_guard lock(mu_);
        auto existing = std::find_if(devices_.begin(), devices_.end(), [&](const auto& entry) {
            return entry.second.info.ip == ip && entry.second.info.cmd_port == cmd_port;
        });
        id = existing == devices_.end() ? "manual_" + ip + "_" + std::to_string(cmd_port) : existing->first;
        auto& md = devices_[id];
        md.info.id = id;
        if (!name.empty()) md.manual_name = name;
        if (!md.manual_name.empty()) md.info.name = md.manual_name;
        else if (md.info.name.empty()) md.info.name = "MiPlay-" + ip;
        md.info.ip = ip;
        md.info.cmd_port = cmd_port;
        md.info.port_source = discovery::PortSource::Manual;
        md.info.is_manual = true;
        md.info.online = true;
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
                auto ts = master_clock_.next_frame_timestamps(static_cast<uint32_t>(samples), !use_aac);
                if (use_aac) {
                    if (!aac_encoder_.encode_pcm(stereo_pcm, samples, ts.linear_pts_90khz,
                        [this](const uint8_t* adts, size_t len, uint64_t capture_pts) {
                            const uint64_t pts = capture_pts + master_clock_.base_play_delay_ms() * 90ULL;
                            multi_sender_.broadcast_audio_frame(adts, len, pts, static_cast<uint32_t>(pts));
                        })) {
                        set_last_error("AAC 编码失败，已重置编码器并跳过受影响的 PCM");
                    }
                } else {
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
            [this] { return multi_sender_.has_streaming_target(); },
            [this](uint64_t skipped) { master_clock_.skip_samples(skipped); },
            master_clock_.session_origin_us());

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
    if (!dev.info.online) {
        dev.status_text = "设备离线";
        return false;
    }
    const std::string sender_ip = sender_ip_for(dev.info);
    if (sender_ip.empty()) {
        dev.status_text = auto_bind_ ? "找不到到达设备的本地 IPv4 地址" : "本地绑定地址不可用: " + bind_ip_;
        return false;
    }
    const AudioStreamConfig cfg = active_audio_cfg_;
    const uint64_t sid = next_session_id_.fetch_add(1);
    dev.active_session_id = sid;
    dev.status_text = "启动 RTSP 监听与 8899 握手...";

    auto handle = std::make_shared<SessionHandle>();
    handle->session_id = sid;
    handle->device_id = dev_id;
    handle->local_ip = sender_ip;
    handle->remote_ip = dev_ip;
    handle->remote_port = cmd_port;
    handle->rtsp_server = std::make_unique<rtsp::WfdRtspServer>();

    rtsp::RtspServerCallbacks rcbs;
    rcbs.on_stream_connection = [this, sid](const std::string& id, const std::string& peer_ip,
                                            std::shared_ptr<net::ConnectionWriter> writer,
                                            std::function<void()> on_failure) {
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
                const auto fd = writer->socket();
                multi_sender_.add_or_update_target(id, peer_ip, fd, bias, false, sid,
                                                   std::move(writer), std::move(on_failure));
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
    rcbs.on_stop_media = [this, sid](const std::string& id) {
        multi_sender_.remove_target(id, sid);
    };
    rcbs.on_session_closed = [this, sid](const std::string& id) {
        {
            std::lock_guard lk(mu_);
            auto it = devices_.find(id);
            if (it == devices_.end() || it->second.active_session_id != sid) return;
            std::lock_guard failed_lock(failed_mu_);
            detach_device_stream_locked(it->second, failed_sessions_);
            it->second.status_text = "RTSP 音频会话已断开";
        }
        failed_cv_.notify_one();
        emit_state_changed();
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
        test_rtsp_bind_port_.load(),
        sid);
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

    auto cmd_sess = std::make_shared<cmd::CmdSession>(dev_ip, cmd_port, sender_ip);
    handle->cmd_session = cmd_sess;
    auto init_media = build_effective_media_info_locked();

    // SS-01：使用可取消、可等待的托管连接线程（绝不 detach）
    handle->connect_thread = std::thread([this, handle, cmd_sess, dev_id, sender_ip, rtsp_port, init_vol, init_media, sid]() {
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
        ccbs.on_remote_control = [this, dev_id, sid](const cmd::RemoteControlEvent& ev) {
            handle_remote_control(dev_id, sid, ev);
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
            cmd::MediaInfoPayload latest_media = init_media;
            if (!handle->cancelled.load()) {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = devices_.find(dev_id);
                still_active = (multiroom_running_ && it != devices_.end() &&
                                it->second.in_group && it->second.active_session_id == sid);
                if (still_active) latest_media = build_effective_media_info_locked();
            }
            if (still_active) {
                cmd_sess->send_open_device(sender_ip, rtsp_port, 1);
                cmd_sess->send_set_media_info(latest_media);
                if (latest_media.position_ms > 0) {
                    cmd_sess->send_set_position_ms(latest_media.position_ms);
                }
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

void MultiRoomEngine::cleanup_failed_sessions() {
    while (true) {
        std::vector<std::shared_ptr<SessionHandle>> failed;
        {
            std::unique_lock<std::mutex> lock(failed_mu_);
            failed_cv_.wait(lock, [&] { return stop_failed_worker_ || !failed_sessions_.empty(); });
            if (failed_sessions_.empty()) {
                if (stop_failed_worker_) return;
                continue;
            }
            failed.swap(failed_sessions_);
        }
        cleanup_detached(failed, false);
    }
}

void MultiRoomEngine::cleanup_detached(std::vector<std::shared_ptr<SessionHandle>>& detached, bool graceful) {
    auto cleanup = [this, graceful](const std::shared_ptr<SessionHandle>& h) {
        if (!h) return;
        h->cancelled.store(true);
        // 按唯一 session_id 移除音频目标，绝不误删同一 device_id 的新会话
        multi_sender_.remove_target(h->device_id, h->session_id);
        // On interleaved connections, send RTSP teardown after the RTP writer stops.
        if (h->rtsp_server) {
            h->rtsp_server->stop();
        }
        if (h->cmd_session) {
            if (graceful) h->cmd_session->send_close_and_abort();
            else h->cmd_session->abort();
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
    cmd::MediaInfoPayload info;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode != media::AudioSourceMode::SystemLoopback &&
            mode != media::AudioSourceMode::ExternalPcm) {
            has_external_media_ = false;
        }
        info = build_effective_media_info_locked();
        last_broadcast_media_sig_.clear();
    }
    broadcast_media_info(info);
    emit_state_changed();
    return true;
}

void MultiRoomEngine::push_external_pcm(const int16_t* stereo_pcm, size_t frame_samples) {
    audio_pipeline_.push_external_pcm(stereo_pcm, frame_samples);
}

cmd::MediaInfoPayload MultiRoomEngine::build_effective_media_info_locked() const {
    const auto base = audio_pipeline_.current_meta();
    const auto cur_mode = audio_pipeline_.source_mode();
    cmd::MediaInfoPayload out;
    if (has_external_media_ &&
        (cur_mode == media::AudioSourceMode::SystemLoopback ||
         cur_mode == media::AudioSourceMode::ExternalPcm)) {
        out = external_media_;
        if (out.title.empty()) out.title = base.title;
        if (out.artist.empty()) out.artist = base.artist;
        if (out.album.empty()) out.album = base.album;
        if (!external_lrc_.empty()) {
            if (out.lyrics.empty()) out.lyrics = external_lrc_.raw_text();
            if (out.lyric_line.empty()) out.lyric_line = external_lrc_.line_at(out.position_ms);
        }
        return out;
    }
    out.title = base.title;
    out.artist = base.artist;
    out.album = base.album;
    out.duration_ms = base.duration_ms;
    out.position_ms = base.position_ms;
    out.lyrics = base.lyrics;
    out.lyric_line = base.lyric_line;
    if (out.lyrics.empty() && !external_lrc_.empty()) {
        out.lyrics = external_lrc_.raw_text();
        out.lyric_line = external_lrc_.line_at(out.position_ms);
    }
    return out;
}

void MultiRoomEngine::broadcast_media_info(const cmd::MediaInfoPayload& info) {
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
        s->send_set_media_info(info);
    }
}

void MultiRoomEngine::broadcast_position_ms(uint64_t position_ms) {
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
        s->send_set_position_ms(position_ms);
    }
}

void MultiRoomEngine::set_media_metadata(const cmd::MediaInfoPayload& info, bool broadcast) {
    if (!info.lyrics.empty() && audio_pipeline_.source_mode() == media::AudioSourceMode::WavFile) {
        audio_pipeline_.set_wav_lrc_text(info.lyrics);
    }
    cmd::MediaInfoPayload effective;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const bool same_track = has_external_media_ &&
                                external_media_.title == info.title &&
                                external_media_.artist == info.artist;
        external_media_ = info;
        if (!info.lyrics.empty()) {
            external_lrc_ = media::LrcDocument::parse(info.lyrics);
        } else if (!same_track) {
            external_lrc_ = media::LrcDocument{};
        }
        if (!external_lrc_.empty() && external_media_.lyric_line.empty()) {
            external_media_.lyric_line = external_lrc_.line_at(external_media_.position_ms);
        }
        has_external_media_ = !external_media_.title.empty() || !external_media_.lyrics.empty() ||
                              !external_media_.lyric_line.empty();
        effective = build_effective_media_info_locked();
        last_broadcast_media_sig_ = effective.title + "|" + effective.artist + "|" + effective.album + "|" +
                                    effective.lyric_line + "|" + std::to_string(effective.lyrics.size()) + "|" +
                                    std::to_string(effective.duration_ms);
    }
    if (broadcast) {
        broadcast_media_info(effective);
        if (effective.position_ms > 0) broadcast_position_ms(effective.position_ms);
    }
    emit_state_changed();
}

void MultiRoomEngine::set_media_position_ms(uint64_t position_ms, bool broadcast) {
    bool line_changed = false;
    cmd::MediaInfoPayload effective;
    {
        std::lock_guard<std::mutex> lock(mu_);
        external_media_.position_ms = position_ms;
        if (!external_lrc_.empty()) {
            const std::string line = external_lrc_.line_at(position_ms);
            if (line != external_media_.lyric_line) {
                external_media_.lyric_line = line;
                line_changed = true;
            }
        }
        effective = build_effective_media_info_locked();
        if (line_changed) {
            effective.meta_change_type = 1;
            last_broadcast_media_sig_ = effective.title + "|" + effective.artist + "|" + effective.album + "|" +
                                        effective.lyric_line + "|" + std::to_string(effective.lyrics.size()) + "|" +
                                        std::to_string(effective.duration_ms);
        }
    }
    if (broadcast) {
        if (line_changed) broadcast_media_info(effective);
        broadcast_position_ms(position_ms);
    }
    if (line_changed) emit_state_changed();
}

bool MultiRoomEngine::load_lrc_file(const std::string& path, std::string* err_out) {
    const std::string raw = media::LrcDocument::load_text_file(path);
    if (raw.empty()) {
        std::string err = "无法读取 LRC 歌词文件或文件为空";
        if (err_out) *err_out = err;
        set_last_error(err);
        return false;
    }
    if (audio_pipeline_.source_mode() == media::AudioSourceMode::WavFile) {
        audio_pipeline_.set_wav_lrc_text(raw);
    }
    cmd::MediaInfoPayload effective;
    {
        std::lock_guard<std::mutex> lock(mu_);
        external_lrc_ = media::LrcDocument::parse(raw);
        external_media_.lyrics = external_lrc_.raw_text();
        external_media_.lyric_line = external_lrc_.line_at(external_media_.position_ms);
        effective = build_effective_media_info_locked();
        last_broadcast_media_sig_.clear();
    }
    set_last_error("");
    broadcast_media_info(effective);
    emit_state_changed();
    return true;
}

void MultiRoomEngine::set_remote_control_handled_by_host(bool handled) {
    remote_control_handled_by_host_.store(handled);
}

void MultiRoomEngine::tick_media_sync() {
    bool running = false;
    bool has_connected = false;
    cmd::MediaInfoPayload effective;
    bool should_broadcast_meta = false;
    uint64_t pos_to_sync = 0;
    bool should_broadcast_pos = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running = multiroom_running_;
        for (const auto& [_, dev] : devices_) {
            if (dev.session && dev.session->cmd_session && dev.session->cmd_session->is_connected()) {
                has_connected = true;
                break;
            }
        }
        if (running && has_connected) {
            effective = build_effective_media_info_locked();
            const std::string sig = effective.title + "|" + effective.artist + "|" + effective.album + "|" +
                                    effective.lyric_line + "|" + std::to_string(effective.lyrics.size()) + "|" +
                                    std::to_string(effective.duration_ms);
            if (sig != last_broadcast_media_sig_) {
                if (!last_broadcast_media_sig_.empty()) effective.meta_change_type = 1;
                last_broadcast_media_sig_ = sig;
                should_broadcast_meta = true;
            }
            if (effective.duration_ms > 0 || effective.position_ms > 0) {
                pos_to_sync = effective.position_ms;
                should_broadcast_pos = true;
            }
        }
    }
    if (should_broadcast_meta) {
        broadcast_media_info(effective);
    }
    if (should_broadcast_pos) {
        broadcast_position_ms(pos_to_sync);
    }
}

void MultiRoomEngine::handle_remote_control(const std::string& device_id,
                                            uint64_t session_id,
                                            const cmd::RemoteControlEvent& ev) {
    std::string dev_name = device_id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = devices_.find(device_id);
        if (it == devices_.end() || it->second.active_session_id != session_id) return;
        dev_name = it->second.info.name.empty() ? device_id : it->second.info.name;
        if (ev.action == cmd::RemoteControlAction::SetVolume && ev.volume >= 0 && ev.volume <= 100) {
            it->second.volume = ev.volume;
        }
    }

    if (ev.action == cmd::RemoteControlAction::SetVolume) {
        emit_state_changed();
        return;
    }

    const auto src_mode = audio_pipeline_.source_mode();
    bool local_handled = false;
    if (src_mode == media::AudioSourceMode::WavFile) {
        if (ev.action == cmd::RemoteControlAction::Pause) {
            audio_pipeline_.set_wav_paused(true);
            local_handled = true;
        } else if (ev.action == cmd::RemoteControlAction::Resume) {
            audio_pipeline_.set_wav_paused(false);
            local_handled = true;
        } else if (ev.action == cmd::RemoteControlAction::Seek) {
            audio_pipeline_.seek_wav_ms(ev.position_ms);
            broadcast_position_ms(ev.position_ms);
            local_handled = true;
        }
        std::vector<std::shared_ptr<cmd::CmdSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [_, d] : devices_) {
                if (d.session && d.session->cmd_session && d.session->cmd_session->is_connected()) {
                    sessions.push_back(d.session->cmd_session);
                }
            }
        }
        for (auto& s : sessions) {
            if (ev.action == cmd::RemoteControlAction::Pause) s->send_pause();
            else if (ev.action == cmd::RemoteControlAction::Resume) s->send_resume();
        }
    } else if (!remote_control_handled_by_host_.load()) {
        local_handled = media::SystemMediaController::dispatch_action(ev.action, ev.position_ms);
    }

    const std::string evt = json::dump(json::Value::Object{
        {"type", "remoteControl"},
        {"deviceId", device_id},
        {"deviceName", dev_name},
        {"action", media::SystemMediaController::action_name(ev.action)},
        {"positionMs", ev.position_ms},
        {"handledLocally", local_handled},
    });
    emit_event_json(evt);
    if (local_handled) emit_state_changed();
}

EngineStateSnapshot MultiRoomEngine::snapshot_state() const {
    EngineStateSnapshot snap;
    auto timer_map = timer_service_.snapshot_telemetry();
    auto rtp_nodes = multi_sender_.snapshot_nodes();
    std::map<std::string, uint64_t> sent_pkts;
    for (const auto& n : rtp_nodes) sent_pkts[n.device_id] = n.packets_sent;

    std::lock_guard<std::mutex> lock(mu_);
    snap.bind_ip = bind_ip_;
    snap.auto_bind = auto_bind_;
    snap.discovery_running = discovery_running_;
    snap.multiroom_running = multiroom_running_;
    snap.loopback_supported = media::AudioPipeline::is_loopback_supported();
    snap.process_loopback_fallback = audio_pipeline_.is_process_loopback_fallback();
    snap.timer_service_port = timer_service_.port();
    snap.session_time_offset_us = master_clock_.session_origin_us();
    snap.sample_rate = active_audio_cfg_.sample_rate;
    snap.base_play_delay_ms = master_clock_.base_play_delay_ms();
    snap.prefer_lpcm = active_audio_cfg_.lpcm;
    snap.master_volume = master_volume_;
    snap.source_mode = audio_pipeline_.source_mode();
    snap.source_param = audio_pipeline_.source_param();
    snap.last_error = last_error_.empty() ? audio_pipeline_.last_error() : last_error_;
    const auto effective_media = build_effective_media_info_locked();
    snap.track_meta = audio_pipeline_.current_meta();
    snap.track_meta.title = effective_media.title;
    snap.track_meta.artist = effective_media.artist;
    snap.track_meta.album = effective_media.album;
    snap.track_meta.duration_ms = effective_media.duration_ms;
    snap.track_meta.position_ms = effective_media.position_ms;
    snap.track_meta.lyrics = effective_media.lyrics;
    snap.track_meta.lyric_line = effective_media.lyric_line;

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

json::Value MultiRoomEngine::snapshot_state_value() const {
    const auto snap = snapshot_state();
    json::Value::Array devices;
    for (const auto& d : snap.devices) {
        devices.emplace_back(json::Value::Object{
            {"id", d.info.id}, {"name", d.info.name}, {"ip", d.info.ip},
            {"cmdPort", d.info.cmd_port}, {"mac", d.info.mac}, {"deviceType", d.info.device_type},
            {"roomName", d.info.room_name}, {"online", d.info.online}, {"isManual", d.info.is_manual},
            {"inGroup", d.in_group}, {"cmdConnected", d.cmd_connected}, {"rtspStreaming", d.rtsp_streaming},
            {"volume", d.volume}, {"delayBiasMs", d.delay_bias_ms}, {"reportedLatencyMs", d.reported_latency_ms},
            {"syncResidualUs", d.sync_residual_us}, {"syncPackets", d.sync_packets},
            {"audioPacketsSent", d.audio_packets_sent}, {"remoteVersion", d.remote_version},
            {"statusText", d.status_text},
        });
    }
    return json::Value::Object{
        {"bindIp", snap.bind_ip}, {"autoBind", snap.auto_bind},
        {"discoveryRunning", snap.discovery_running}, {"multiroomRunning", snap.multiroom_running},
        {"loopbackSupported", snap.loopback_supported}, {"processLoopbackFallback", snap.process_loopback_fallback},
        {"timerServicePort", snap.timer_service_port},
        {"sessionTimeOffsetUs", snap.session_time_offset_us}, {"sampleRate", snap.sample_rate},
        {"basePlayDelayMs", snap.base_play_delay_ms}, {"codec", snap.prefer_lpcm ? "LPCM" : "AAC_LC"},
        {"masterVolume", snap.master_volume}, {"sourceMode", static_cast<int>(snap.source_mode)},
        {"sourceParam", snap.source_param}, {"lastError", snap.last_error},
        {"track", json::Value::Object{
            {"title", snap.track_meta.title}, {"artist", snap.track_meta.artist}, {"album", snap.track_meta.album},
            {"durationMs", snap.track_meta.duration_ms}, {"positionMs", snap.track_meta.position_ms},
            {"lyricLine", snap.track_meta.lyric_line}, {"hasLyrics", !snap.track_meta.lyrics.empty() || !snap.track_meta.lyric_line.empty()},
            {"paused", snap.track_meta.paused},
        }},
        {"devices", std::move(devices)},
    };
}

std::string MultiRoomEngine::snapshot_state_json() const {
    return json::dump(snapshot_state_value());
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
