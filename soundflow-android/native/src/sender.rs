// SPDX-License-Identifier: AGPL-3.0-only
use jni::{
    JNIEnv,
    objects::{JByteArray, JClass, JString},
    sys::{jint, jlong, jstring},
};
use std::{
    collections::HashMap,
    ffi::{CStr, CString, c_char, c_void},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{
        Arc, LazyLock, Mutex, RwLock,
        atomic::{AtomicI64, Ordering},
    },
};

unsafe extern "C" {
    fn soundflow_sender_create() -> *mut c_void;
    fn soundflow_sender_request(context: *mut c_void, json: *const c_char) -> *const c_char;
    fn soundflow_sender_push_pcm(context: *mut c_void, pcm: *const i16, frames: usize);
    fn soundflow_sender_destroy(context: *mut c_void);
}

struct SenderContext {
    // Request buffer inside C++ Sender is single-caller; serialize request/poll under rpc_lock,
    // while allowing real-time push_pcm under RwLock read guard with zero RPC contention.
    lifecycle: RwLock<Option<usize>>,
    rpc_lock: Mutex<()>,
}

impl SenderContext {
    fn new() -> Option<Self> {
        let raw = unsafe { soundflow_sender_create() };
        if raw.is_null() {
            return None;
        }
        Some(Self {
            lifecycle: RwLock::new(Some(raw as usize)),
            rpc_lock: Mutex::new(()),
        })
    }

    fn request(&self, json: Option<&str>) -> Option<String> {
        let _rpc_guard = self.rpc_lock.lock().ok()?;
        let ptr_guard = self.lifecycle.read().ok()?;
        let raw = (*ptr_guard)? as *mut c_void;
        let c_req = match json {
            Some(s) if !s.is_empty() => Some(CString::new(s).ok()?),
            _ => None,
        };
        let req_ptr = c_req.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        let resp = unsafe { soundflow_sender_request(raw, req_ptr) };
        if resp.is_null() {
            return None;
        }
        Some(
            unsafe { CStr::from_ptr(resp) }
                .to_string_lossy()
                .into_owned(),
        )
    }

    fn push_pcm_bytes(&self, pcm_le: &[u8], frames: usize) {
        let needed_bytes = frames.saturating_mul(4);
        if frames == 0 || pcm_le.len() < needed_bytes {
            return;
        }
        let Ok(ptr_guard) = self.lifecycle.read() else {
            return;
        };
        let Some(addr) = *ptr_guard else {
            return;
        };
        // Copy into properly aligned i16 stack/heap buffer to guarantee strict alignment on ARM64.
        let sample_count = frames * 2;
        let mut aligned = Vec::with_capacity(sample_count);
        for chunk in pcm_le[..needed_bytes].chunks_exact(2) {
            aligned.push(i16::from_le_bytes([chunk[0], chunk[1]]));
        }
        unsafe {
            soundflow_sender_push_pcm(addr as *mut c_void, aligned.as_ptr(), frames);
        }
    }

    fn shutdown(&self) {
        let _rpc_guard = self.rpc_lock.lock().ok();
        if let Ok(mut guard) = self.lifecycle.write()
            && let Some(addr) = guard.take()
        {
            unsafe { soundflow_sender_destroy(addr as *mut c_void) };
        }
    }
}

impl Drop for SenderContext {
    fn drop(&mut self) {
        self.shutdown();
    }
}

static NEXT_ID: AtomicI64 = AtomicI64::new(1);
static REGISTRY: LazyLock<Mutex<HashMap<jlong, Arc<SenderContext>>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

fn lookup_sender(id: jlong) -> Option<Arc<SenderContext>> {
    if id <= 0 {
        return None;
    }
    REGISTRY.lock().ok()?.get(&id).cloned()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativeSender_nativeCreate(
    _: JNIEnv,
    _: JClass,
) -> jlong {
    catch_unwind(|| {
        let Some(ctx) = SenderContext::new() else {
            return 0;
        };
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        let Ok(mut map) = REGISTRY.lock() else {
            return 0;
        };
        map.insert(id, Arc::new(ctx));
        id
    })
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativeSender_nativeDestroy(
    _: JNIEnv,
    _: JClass,
    handle: jlong,
) {
    let _ = catch_unwind(|| {
        let removed = REGISTRY.lock().ok().and_then(|mut map| map.remove(&handle));
        if let Some(ctx) = removed {
            ctx.shutdown();
        }
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativeSender_nativeRequest(
    mut env: JNIEnv,
    _: JClass,
    handle: jlong,
    json: JString,
) -> jstring {
    let result = catch_unwind(AssertUnwindSafe(|| -> Option<String> {
        let ctx = lookup_sender(handle)?;
        let req: Option<String> = if json.is_null() {
            None
        } else {
            Some(env.get_string(&json).ok()?.into())
        };
        ctx.request(req.as_deref())
    }))
    .ok()
    .flatten();

    match result {
        Some(s) => env
            .new_string(s)
            .map(|v| v.into_raw())
            .unwrap_or(std::ptr::null_mut()),
        None => std::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativeSender_nativePushPcm(
    env: JNIEnv,
    _: JClass,
    handle: jlong,
    pcm: JByteArray,
    frames: jint,
) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if pcm.is_null() || frames <= 0 {
            return;
        }
        let Some(ctx) = lookup_sender(handle) else {
            return;
        };
        let max_frames = (frames as usize).min(48_000);
        let needed = max_frames * 4;
        let len = env.get_array_length(&pcm).unwrap_or(0) as usize;
        if len < needed {
            return;
        }
        let mut bytes = vec![0i8; needed];
        if env.get_byte_array_region(&pcm, 0, &mut bytes).is_ok() {
            let u8_slice =
                unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const u8, bytes.len()) };
            ctx.push_pcm_bytes(u8_slice, max_frames);
        }
    }));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sender_registry_lifecycle_and_self_rejection() {
        let ctx = Arc::new(SenderContext::new().expect("create native sender"));
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        REGISTRY.lock().unwrap().insert(id, Arc::clone(&ctx));

        let state_raw = ctx
            .request(Some(r#"{"id":"1","method":"state.get"}"#))
            .expect("state.get");
        let parsed: serde_json::Value = serde_json::from_str(&state_raw).unwrap();
        assert_eq!(parsed["type"], "rpc");
        assert!(parsed["result"].is_object());

        let self_add = ctx
            .request(Some(
                r#"{"id":"2","method":"device.addManual","name":"Self","ip":"127.0.0.1","cmdPort":8899}"#,
            ))
            .expect("addManual self");
        let self_val: serde_json::Value = serde_json::from_str(&self_add).unwrap();
        assert!(self_val["error"].as_str().unwrap_or_default().contains("本机"));

        let pcm = vec![0u8; 1024 * 4];
        ctx.push_pcm_bytes(&pcm, 1024);

        let poll_raw = ctx.request(None).expect("poll");
        let poll_val: serde_json::Value = serde_json::from_str(&poll_raw).unwrap();
        assert!(poll_val["data"].is_object());
        assert!(poll_val["events"].is_array());

        let removed = REGISTRY.lock().unwrap().remove(&id).unwrap();
        removed.shutdown();
        assert!(removed.request(None).is_none());
    }
}
