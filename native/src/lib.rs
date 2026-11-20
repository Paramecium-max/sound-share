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
static EVENTS: LazyLock<Mutex<VecDeque<Value>>> = LazyLock::new(|| Mutex::new(VecDeque::new()));

fn sanitize(v: &mut Value) {
    match v {
        Value::Object(map) => {
            map.retain(|k, _| {
                !["key", "iv", "hex", "payload", "packet", "challenge", "token", "secret"]
                    .iter()
                    .any(|s| k.to_ascii_lowercase().contains(s))
            });
            for value in map.values_mut() {
                sanitize(value);
            }
        }
        Value::Array(arr) => {
            for value in arr {
                sanitize(value);
            }
        }
        Value::String(s) if s.len() > 2048 => {
            *s = s.chars().take(512).collect();
        }
        _ => {}
    }
}

fn emit(mut value: Value) {
    sanitize(&mut value);
    if let Ok(mut queue) = EVENTS.lock() {
        if queue.len() >= 192 {
            queue.pop_front();
        }
        queue.push_back(value);
    }
}

fn friendly_bind_error(raw: String) -> String {
    if raw.contains("Address already in use") || raw.contains("os error 98") {
        return "端口 8899/55982 已被占用：若本机运行着其他投屏/妙播接收器，请先关闭后再试".into();
    }
    raw
}

fn run_start(config: &str) -> Result<(), String> {
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
    EVENTS
        .lock()
        .map_err(|_| "event queue poisoned")?
        .clear();
    let receiver = MiPlayReceiver::start(cfg, Arc::new(emit))
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
        read_jstr(&mut env, &config).and_then(|s| run_start(&s))
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
    let _ = catch_unwind(|| {
        reset_live_output_peak();
        if let Ok(mut guard) = RECEIVER.lock() {
            *guard = None;
        }
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_poll(
    mut env: JNIEnv,
    _: JClass,
) -> jstring {
    let events = EVENTS
        .lock()
        .map(|mut q| q.drain(..).collect::<Vec<_>>())
        .unwrap_or_default();
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
        let guard = RECEIVER.lock().map_err(|_| "receiver lock poisoned")?;
        let ctrl = guard.as_ref().ok_or("接收内核尚未启动")?.controller();
        match cmd.as_str() {
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
            "nested": { "iv": "abc", "token": "xyz", "stage": "ok" },
            "title": "Song"
        });
        sanitize(&mut payload);
        assert!(payload.get("streamKey").is_none());
        assert!(payload["nested"].get("iv").is_none());
        assert!(payload["nested"].get("token").is_none());
        assert_eq!(payload["nested"]["stage"], "ok");
        assert_eq!(payload["title"], "Song");
    }

    #[test]
    fn friendly_bind_error_detects_port_conflict() {
        let msg = friendly_bind_error("bind TCP 8899: Address already in use (os error 98)".into());
        assert!(msg.contains("8899/55982"));
    }

    #[test]
    fn run_start_rejects_loopback() {
        let err = run_start(r#"{"ip":"127.0.0.1","interface":"lo","name":"Pad","uuid":"u","lyra":"12345678","media":"12345678"}"#).unwrap_err();
        assert!(err.contains("IPv4"));
    }
}

fn onset_json(events: Vec<fusionplay_miplay_sdk::onset::OnsetEvent>) -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQUENCE: AtomicU64 = AtomicU64::new(1);
    let now = fusionplay_miplay_sdk::onset::now_ms();
    let values: Vec<_> = events.into_iter().filter(|e| now - e.time_ms < 350.0).map(|e| serde_json::json!({
        "id": SEQUENCE.fetch_add(1, Ordering::Relaxed),
        "band": (["bass", "mid", "high", "drum"][e.band]),
        "strength": e.strength, "percussion": e.percussion,
        "delay": e.time_ms - now,
    })).collect();
    serde_json::to_string(&values).unwrap_or_else(|_| "[]".to_owned())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_audioOnsets(
    mut env: JNIEnv, _: JClass, active: jni::sys::jboolean,
) -> jstring {
    java_str(&mut env, onset_json(fusionplay_miplay_sdk::onset::poll(active != 0)))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_spectrumOnsets(
    mut env: JNIEnv, _: JClass, bins: jni::objects::JFloatArray, rate: jint,
) -> jstring {
    let count = env.get_array_length(&bins).unwrap_or(0).clamp(0, 4096);
    let mut values = vec![0.0; count as usize];
    if env.get_float_array_region(&bins, 0, &mut values).is_err() {
        return java_str(&mut env, "[]".to_owned());
    }
    let events = fusionplay_miplay_sdk::onset::external_spectrum(&values, rate.max(1) as u32);
    java_str(&mut env, onset_json(events))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_receiver_NativeBridge_setOnsetSensitivity(
    _: JNIEnv, _: JClass, value: jint,
) {
    fusionplay_miplay_sdk::onset::set_sensitivity(value.clamp(0, 100) as u32);
}
