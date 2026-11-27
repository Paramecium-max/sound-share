use jni::{
    JNIEnv,
    objects::JClass,
    sys::{jint, jlong, jstring},
};
use std::ffi::{CStr, c_char, c_void};

unsafe extern "C" {
    fn soundflow_timer_start() -> *mut c_void;
    fn soundflow_timer_stop(timer: *mut c_void);
    fn soundflow_timer_port(timer: *mut c_void) -> u16;
    fn soundflow_timer_snapshot(timer: *mut c_void) -> *const c_char;
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_TimerService_nativeStart(
    _: JNIEnv,
    _: JClass,
) -> jlong {
    unsafe { soundflow_timer_start() as jlong }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_TimerService_nativeStop(
    _: JNIEnv,
    _: JClass,
    timer: jlong,
) {
    unsafe { soundflow_timer_stop(timer as *mut c_void) }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_TimerService_nativePort(
    _: JNIEnv,
    _: JClass,
    timer: jlong,
) -> jint {
    unsafe { soundflow_timer_port(timer as *mut c_void) as jint }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_TimerService_nativeSnapshot(
    env: JNIEnv,
    _: JClass,
    timer: jlong,
) -> jstring {
    let snapshot = unsafe { soundflow_timer_snapshot(timer as *mut c_void) };
    if snapshot.is_null() {
        return std::ptr::null_mut();
    }
    // Java serializes access to the context. Its buffer stays valid through this call.
    unsafe { CStr::from_ptr(snapshot) }
        .to_str()
        .ok()
        .and_then(|json| env.new_string(json).ok())
        .map(|value| value.into_raw())
        .unwrap_or(std::ptr::null_mut())
}
