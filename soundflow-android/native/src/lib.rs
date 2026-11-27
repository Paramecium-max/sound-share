// SPDX-License-Identifier: AGPL-3.0-only
use fusionplay_miplay_sdk::{
    MediaAction, MiPlayDeviceType, MiPlayReceiver, ReceiverConfig, ReceiverIdentity,
    live_output_peak, live_output_spectrum_take, reset_live_output_peak,
};
use jni::JNIEnv;
use jni::objects::{JClass, JString};
use jni::sys::{jfloat, jint, jstring};
use serde_json::Value;
use std::collections::VecDeque;
use std::net::Ipv4Addr;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, LazyLock, Mutex};

static RECEIVER: Mutex<Option<MiPlayReceiver>> = Mutex::new(None);
static EVENTS: LazyLock<Mutex<PendingEvents>> = LazyLock::new(|| Mutex::new(PendingEvents::default()));

#[derive(Default)]
struct PendingEvents {
    receiver_epoch: u64,
    media_generation: u64,
    queue: VecDeque<Value>,
}

impl PendingEvents {
    fn reset(&mut self) -> u64 {
        self.receiver_epoch = self.receiver_epoch.wrapping_add(1);
        self.media_generation = 0;
        self.queue.clear();
        self.receiver_epoch
    }

    fn push(&mut self, receiver_epoch: u64, value: Value) {
        if receiver_epoch != self.receiver_epoch {
            return;
        }
        if let Some(generation) = value["media_generation"].as_u64() {
            if generation < self.media_generation {
                return;
            }
            if generation > self.media_generation {
                self.media_generation = generation;
                self.queue.retain(|event| {
                    event["media_generation"].as_u64().is_none_or(|old| old >= generation)
                });
            }
        }
        if self.queue.len() >= 192 {
            self.queue.pop_front();
        }
        self.queue.push_back(value);
    }
}

fn is_sensitive_key(key: &str) -> bool {
    let lower_full = key.to_ascii_lowercase();
    if lower_full.contains("auth_message")
        || lower_full.contains("auth_ack")
        || lower_full.contains("auth_msg")
        || lower_full.contains("authmessage")
        || lower_full.contains("authack")
        || lower_full.contains("authmsg")
    {
        return true;
    }
    // Split on non-alphanumeric separators and camelCase boundaries so short tokens
    // like "iv" match "aes_iv" / "streamIv" without stripping "session_active",
    // "receiver_name", or "arrival_residual_ms".
    let check = |slice: &str| {
        matches!(
            slice.to_ascii_lowercase().as_str(),
            "key" | "iv" | "hex" | "payload" | "packet" | "challenge" | "token" | "secret" | "digest" | "hmac"
        )
    };
    for part in key.split(|c: char| !c.is_ascii_alphanumeric()) {
        if part.is_empty() {
            continue;
        }
        let bytes = part.as_bytes();
        let mut start = 0usize;
        for i in 1..bytes.len() {
            if bytes[i].is_ascii_uppercase() && bytes[i - 1].is_ascii_lowercase() {
                if check(&part[start..i]) {
                    return true;
                }
                start = i;
            }
        }
        if check(&part[start..]) {
            return true;
        }
    }
    false
}

fn sanitize_with_key(v: &mut Value, key_hint: Option<&str>) {
    match v {
        Value::Object(map) => {
            map.retain(|k, _| !is_sensitive_key(k));
            for (k, value) in map.iter_mut() {
                sanitize_with_key(value, Some(k.as_str()));
            }
        }
        Value::Array(arr) => {
            for value in arr {
                sanitize_with_key(value, None);
            }
        }
        Value::String(s) => match key_hint {
            Some("lyrics") => {
                if s.len() > 65_536 {
                    *s = s.chars().take(16_384).collect();
                }
            }
            Some("artwork") => {
                if s.len() > 262_144 {
                    s.clear();
                }
            }
            _ => {
                if s.len() > 2048 {
                    *s = s.chars().take(512).collect();
                }
            }
        },
        _ => {}
    }
}

fn sanitize(v: &mut Value) {
    sanitize_with_key(v, None);
}

fn is_high_freq_trace_event(v: &Value) -> bool {
    matches!(
        v["event"].as_str(),
        Some(
            "protocol_trace"
                | "discovery_packet_received"
                | "lyra_packet_received"
                | "lyra_transport_frame_trace"
        )
    )
}

fn emit(receiver_epoch: u64, mut value: Value) {
    if is_high_freq_trace_event(&value) {
        return;
    }
    sanitize(&mut value);
    if let Ok(mut pending) = EVENTS.lock() {
        pending.push(receiver_epoch, value);
    }
}

fn friendly_bind_error(raw: String) -> String {
    if raw.contains("Address already in use") || raw.contains("os error 98") {
        return "端口 8899/55982 已被占用：若本机运行着其他投屏/妙播接收器，请先关闭后再试".into();
    }
    raw
}

pub fn start_receiver(config: &str) -> Result<(), String> {
    let v: Value = serde_json::from_str(config).map_err(|e| e.to_string())?;
    let str_field = |k: &str| v[k].as_str().ok_or_else(|| format!("missing {k}"));
    let ip: Ipv4Addr = str_field("ip")?
        .parse()
        .map_err(|_| "invalid IPv4 address".to_string())?;
    if ip.is_unspecified() || ip.is_loopback() || ip.is_multicast() {
        return Err("局域网 IPv4 地址无效".into());
    }
    let identity = ReceiverIdentity::new(str_field("uuid")?, str_field("lyra")?, str_field("media")?);
    let name = str_field("name")?;
    if name.is_empty() || name.chars().count() > 32 {
        return Err("设备名称长度需在 1–32 个字符之间".into());
    }
    let kind = MiPlayDeviceType::try_from(v["device_type"].as_i64().unwrap_or(4) as i32)
        .map_err(str::to_owned)?;
    let cfg = ReceiverConfig::new(name, identity, ip, str_field("interface")?)
        .with_device_type(kind)
        .with_initial_volume_percent(v["volume"].as_u64().unwrap_or(45).min(100) as u32);
    let mut guard = RECEIVER.lock().map_err(|_| "receiver lock poisoned")?;
    if guard.is_some() {
        return Err("接收内核已在运行".into());
    }
    reset_live_output_peak();
    let receiver_epoch = EVENTS
        .lock()
        .map_err(|_| "event queue poisoned")?
        .reset();
    let receiver = MiPlayReceiver::start(cfg, Arc::new(move |event| emit(receiver_epoch, event)))
        .map_err(|e| friendly_bind_error(format!("{e:#}")))?;
    *guard = Some(receiver);
    Ok(())
}

fn java_str(env: &mut JNIEnv, s: String) -> jstring {
    env.new_string(s)
        .map(|j| j.into_raw())
        .unwrap_or(std::ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_setAudioSyncOffset(
    _: JNIEnv,
    _: JClass,
    value: jint,
) {
    #[cfg(target_os = "android")]
    fusionplay_miplay_sdk::set_audio_sync_offset_ms(value);
    #[cfg(not(target_os = "android"))]
    let _ = value;
}

fn read_jstr(env: &mut JNIEnv, s: &JString) -> Result<String, String> {
    env.get_string(s).map(Into::into).map_err(|e| e.to_string())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_start(
    mut env: JNIEnv,
    _: JClass,
    config: JString,
) -> jstring {
    let res = catch_unwind(AssertUnwindSafe(|| {
        read_jstr(&mut env, &config).and_then(|s| start_receiver(&s))
    }));
    java_str(
        &mut env,
        match res {
            Ok(Ok(())) => String::new(),
            Ok(Err(e)) => e,
            Err(_) => "接收内核初始化异常".into(),
        },
    )
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_stop(_: JNIEnv, _: JClass) {
    let _ = catch_unwind(stop_receiver);
}

pub fn stop_receiver() {
    reset_live_output_peak();
    if let Ok(mut guard) = RECEIVER.lock() {
        if let Ok(mut pending) = EVENTS.lock() {
            pending.reset();
        }
        *guard = None;
    }
}

pub fn poll_receiver() -> Vec<Value> {
    EVENTS.lock()
        .map(|mut pending| pending.queue.drain(..).collect())
        .unwrap_or_default()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_poll(
    mut env: JNIEnv,
    _: JClass,
) -> jstring {
    let events = poll_receiver();
    if events.is_empty() {
        return std::ptr::null_mut();
    }
    java_str(
        &mut env,
        serde_json::to_string(&events).unwrap_or_else(|_| "[]".into()),
    )
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_action(
    mut env: JNIEnv,
    _: JClass,
    action: JString,
    value: jint,
) -> jstring {
    let res = catch_unwind(AssertUnwindSafe(|| -> Result<(), String> {
        let cmd = read_jstr(&mut env, &action)?;
        receiver_action(&cmd, value)
    }));
    java_str(
        &mut env,
        match res {
            Ok(Ok(())) => String::new(),
            Ok(Err(e)) => e,
            Err(_) => "控制命令执行异常".into(),
        },
    )
}

pub fn receiver_action(cmd: &str, value: i32) -> Result<(), String> {
    let guard = RECEIVER.lock().map_err(|_| "receiver lock poisoned")?;
    let ctrl = guard.as_ref().ok_or("接收内核尚未启动")?.controller();
    match cmd {
        "pause" => ctrl.send(MediaAction::Pause),
        "resume" => ctrl.send(MediaAction::Resume),
        "prev" => ctrl.send(MediaAction::Previous),
        "next" => ctrl.send(MediaAction::Next),
        "volume" => ctrl.set_volume(value.clamp(0, 100) as u8),
        "suspend" => {
            reset_live_output_peak();
            ctrl.suspend_output();
            return Ok(());
        }
        "output_resume" => {
            ctrl.resume_output();
            return Ok(());
        }
        _ => return Err("不支持的控制命令".into()),
    }
    .map_err(|e| format!("{e:#}"))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_audioLevel(
    _: JNIEnv,
    _: JClass,
) -> jfloat {
    live_output_peak()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_audioSpectrum(
    _: JNIEnv,
    _: JClass,
) -> jint {
    live_output_spectrum_take() as jint
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn sanitize_strips_sensitive_fields() {
        let mut payload = json!({
            "event": "auth",
            "streamKey": "secret123",
            "aes_iv": "deadbeef",
            "nested": { "iv": "abc", "token": "xyz", "stage": "ok" },
            "title": "Song"
        });
        sanitize(&mut payload);
        assert!(payload.get("streamKey").is_none());
        assert!(payload.get("aes_iv").is_none());
        assert!(payload["nested"].get("iv").is_none());
        assert!(payload["nested"].get("token").is_none());
        assert_eq!(payload["nested"]["stage"], "ok");
        assert_eq!(payload["title"], "Song");
    }

    #[test]
    fn sanitize_preserves_session_active_lyrics_and_artwork() {
        let long_lrc = "[00:01.00]这是一段测试完整歌词内容\n".repeat(120);
        let long_art = "A".repeat(8192);
        let long_misc = "X".repeat(3000);
        let mut payload = json!({
            "event": "media_info",
            "session_active": false,
            "receiver_name": "SoundFlow-Pad",
            "arrival_residual_ms": 12.5,
            "lyrics": long_lrc,
            "artwork": long_art,
            "misc": long_misc
        });
        sanitize(&mut payload);
        assert_eq!(payload["session_active"], false);
        assert_eq!(payload["receiver_name"], "SoundFlow-Pad");
        assert_eq!(payload["arrival_residual_ms"], 12.5);
        assert_eq!(payload["lyrics"].as_str().unwrap(), long_lrc);
        assert_eq!(payload["artwork"].as_str().unwrap(), long_art);
        assert_eq!(payload["misc"].as_str().unwrap().chars().count(), 512);
    }

    #[test]
    fn friendly_bind_error_detects_port_conflict() {
        let msg = friendly_bind_error("bind TCP 8899: Address already in use (os error 98)".into());
        assert!(msg.contains("8899/55982"));
    }

    #[test]
    fn run_start_rejects_loopback() {
        let err = start_receiver(r#"{"ip":"127.0.0.1","interface":"lo","name":"Pad","uuid":"u","lyra":"12345678","media":"12345678"}"#).unwrap_err();
        assert!(err.contains("IPv4"));
    }

    #[test]
    fn pending_events_discard_delayed_closes_from_replaced_media_and_receivers() {
        let mut pending = PendingEvents::default();
        let first = pending.reset();
        let close = json!({"event": "media_channel_closed", "media_generation": 1});
        pending.push(first, close.clone());
        pending.push(first, json!({"event": "clock_synchronized"}));
        pending.push(first, json!({"event": "stream_started", "media_generation": 2}));
        pending.push(first, close.clone());
        assert_eq!(pending.queue.len(), 2);
        assert_eq!(pending.queue[0]["event"], "clock_synchronized");
        assert_eq!(pending.queue[1]["event"], "stream_started");

        let second = pending.reset();
        pending.push(second, json!({"event": "stream_started", "media_generation": 1}));
        pending.push(first, close);
        assert_eq!(pending.queue.len(), 1);
        assert_eq!(pending.queue[0]["event"], "stream_started");
    }

    #[test]
    fn high_frequency_trace_events_do_not_evict_control_events() {
        assert!(is_high_freq_trace_event(&json!({"event": "protocol_trace"})));
        assert!(is_high_freq_trace_event(&json!({"event": "lyra_packet_received"})));
        assert!(!is_high_freq_trace_event(&json!({"event": "progress", "position_ms": 1200})));
        assert!(!is_high_freq_trace_event(&json!({"event": "media_info"})));
    }
}

use fusionplay_miplay_sdk::onset::{self, OnsetEvent};
use std::cell::RefCell;
use std::sync::atomic::{AtomicU64, Ordering};

thread_local! {
    static ONSETS: RefCell<Vec<OnsetEvent>> = RefCell::new(Vec::with_capacity(48));
    static SPECTRUM: RefCell<[f32; 4096]> = const { RefCell::new([0.0; 4096]) };
}

fn send_onsets(env: &mut JNIEnv, events: &[OnsetEvent]) -> jstring {
    use std::fmt::Write;
    static SEQUENCE: AtomicU64 = AtomicU64::new(1);
    #[cfg(target_os = "android")]
    let bias = onset::monotonic_bias();
    #[cfg(not(target_os = "android"))]
    let bias = 0.0;
    let now = onset::now_ms();
    let mut json = String::new();
    for event in events.iter().filter(|e| e.time_ms.is_finite() && e.strength.is_finite() && now - e.time_ms < 100.0) {
        let prefix = if json.is_empty() { '[' } else { ',' };
        let id = SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let band = ["bass", "mid", "high", "drum"][event.band];
        let strength = event.strength.clamp(0.0, 1.0);
        let percussion = event.percussion;
        let target = event.time_ms + bias;
        let _ = write!(json, r#"{prefix}{{"id":{id},"band":"{band}","strength":{strength:.4},"percussion":{percussion},"targetMs":{target:.3}}}"#);
    }
    if json.is_empty() {
        return std::ptr::null_mut();
    }
    json.push(']');
    java_str(env, json)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_setAudioMeterEnabled(
    _: JNIEnv, _: JClass, enabled: jni::sys::jboolean,
) {
    #[cfg(target_os = "android")]
    fusionplay_miplay_sdk::set_audio_meter_enabled(enabled != 0);
    #[cfg(not(target_os = "android"))]
    let _ = enabled;
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_setOnsetCapture(
    _: JNIEnv, _: JClass, enabled: jni::sys::jboolean, monotonic_nanos: jni::sys::jlong,
) {
    #[cfg(target_os = "android")]
    onset::align_monotonic_clock(monotonic_nanos);
    #[cfg(not(target_os = "android"))]
    let _ = monotonic_nanos;
    onset::set_capture(enabled != 0);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_audioOnsets(
    mut env: JNIEnv, _: JClass,
) -> jstring {
    ONSETS.with(|scratch| {
        let mut events = scratch.borrow_mut();
        onset::poll_into(true, &mut events);
        send_onsets(&mut env, &events)
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_spectrumOnsets(
    mut env: JNIEnv, _: JClass, bins: jni::objects::JFloatArray, rate: jint, stamp_nanos: jni::sys::jlong,
) -> jstring {
    let count = env.get_array_length(&bins).unwrap_or(0).clamp(0, 4096) as usize;
    if count == 0 { return std::ptr::null_mut(); }
    let _ = stamp_nanos;
    #[cfg(target_os = "android")]
    let stamp = stamp_nanos as f64 / 1e6 - onset::monotonic_bias();
    #[cfg(not(target_os = "android"))]
    let stamp = onset::now_ms();
    SPECTRUM.with(|scratch| {
        let mut values = scratch.borrow_mut();
        if env.get_float_array_region(&bins, 0, &mut values[..count]).is_err() {
            return std::ptr::null_mut();
        }
        ONSETS.with(|scratch| {
            let mut events = scratch.borrow_mut();
            onset::external_spectrum_into(&values[..count], rate.max(1) as u32, stamp, &mut events);
            send_onsets(&mut env, &events)
        })
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_setOnsetSensitivity(
    _: JNIEnv, _: JClass, value: jint,
) {
    fusionplay_miplay_sdk::onset::set_sensitivity(value.clamp(0, 100) as u32);
}

#[cfg(any(target_os = "linux", target_os = "android"))]
mod sender;
