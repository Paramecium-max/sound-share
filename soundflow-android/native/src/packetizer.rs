use jni::{
    JNIEnv,
    objects::{JByteArray, JClass},
    sys::{jboolean, jbyteArray, jint, jlong},
};
use std::ffi::c_void;

unsafe extern "C" {
    fn soundflow_packetizer_create(stream: u8) -> *mut c_void;
    fn soundflow_packetizer_destroy(packer: *mut c_void);
    fn soundflow_packetizer_frame(
        packer: *mut c_void,
        audio: *const u8,
        size: usize,
        pts: u64,
        seq: u16,
        psi: bool,
        wire_size: *mut usize,
    ) -> *const u8;
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativePacketizer_create(
    _: JNIEnv,
    _: JClass,
    stream: jint,
) -> jlong {
    unsafe { soundflow_packetizer_create(stream as u8) as jlong }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativePacketizer_destroy(
    _: JNIEnv,
    _: JClass,
    packer: jlong,
) {
    unsafe { soundflow_packetizer_destroy(packer as *mut c_void) }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_soundflow_sender_NativePacketizer_frame(
    env: JNIEnv,
    _: JClass,
    packer: jlong,
    audio: JByteArray,
    pts: jlong,
    sequence: jint,
    force: jboolean,
) -> jbyteArray {
    let Ok(audio) = env.convert_byte_array(audio) else {
        return std::ptr::null_mut();
    };
    let mut len = 0;
    let wire = unsafe {
        soundflow_packetizer_frame(
            packer as *mut c_void,
            audio.as_ptr(),
            audio.len(),
            pts as u64,
            sequence as u16,
            force != 0,
            &mut len,
        )
    };
    if wire.is_null() {
        return std::ptr::null_mut();
    }
    // The Java writer thread owns the packetizer. JNI copies the borrowed result.
    let bytes = unsafe { std::slice::from_raw_parts(wire, len) };
    env.byte_array_from_slice(bytes)
        .map(|array| array.into_raw())
        .unwrap_or(std::ptr::null_mut())
}
