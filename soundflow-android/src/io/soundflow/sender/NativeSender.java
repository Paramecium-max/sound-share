package io.soundflow.sender;

/**
 * Thread-safe JNI handle to the shared native MultiRoomEngine (libsoundflow.so).
 * Owns mDNS discovery, 8899 ECDH/AES/TLV control sessions, WFD RTSP M1-M7 server,
 * NDK MediaCodec AAC-LC / LPCM MPEG-TS + RTP framing, and microsecond UDP TimerService.
 */
public final class NativeSender implements AutoCloseable {
    static {
        System.loadLibrary("soundflow");
    }

    private long handle;

    public NativeSender() {
        this.handle = nativeCreate();
        if (this.handle == 0L) {
            throw new IllegalStateException("初始化原生全屋发送核心失败");
        }
    }

    public synchronized boolean isOpen() {
        return handle != 0L;
    }

    public String request(String json) {
        long h;
        synchronized (this) {
            h = this.handle;
        }
        if (h == 0L) return null;
        return nativeRequest(h, json);
    }

    public String poll() {
        long h;
        synchronized (this) {
            h = this.handle;
        }
        if (h == 0L) return null;
        return nativeRequest(h, null);
    }

    public void pushPcm(byte[] stereoPcmLe, int sampleFrames) {
        long h;
        synchronized (this) {
            h = this.handle;
        }
        if (h == 0L || stereoPcmLe == null || sampleFrames <= 0) return;
        nativePushPcm(h, stereoPcmLe, sampleFrames);
    }

    @Override
    public void close() {
        long h;
        synchronized (this) {
            h = this.handle;
            this.handle = 0L;
        }
        if (h != 0L) {
            nativeDestroy(h);
        }
    }

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native String nativeRequest(long handle, String json);
    private static native void nativePushPcm(long handle, byte[] stereoPcmLe, int sampleFrames);
}
