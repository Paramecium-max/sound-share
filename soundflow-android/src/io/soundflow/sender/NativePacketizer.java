package io.soundflow.sender;

final class NativePacketizer implements AutoCloseable {
    static { System.loadLibrary("soundflow"); }

    private long handle;

    NativePacketizer(int streamType) {
        handle = create(streamType);
        if (handle == 0) throw new IllegalStateException("无法初始化原生 MPEG-TS 打包器");
    }

    byte[] pack(byte[] payload, long pts, int sequence, boolean forcePsi) {
        byte[] data = frame(handle, payload, pts, sequence, forcePsi);
        if (data == null) throw new IllegalStateException("原生 MPEG-TS 打包失败");
        return data;
    }

    static int packetCount(byte[] wire) {
        int count = 0;
        for (int at = 0; at < wire.length; count++) {
            at += 4 + ((wire[at + 2] & 0xff) << 8) + (wire[at + 3] & 0xff);
        }
        return count;
    }

    @Override public void close() {
        if (handle != 0) destroy(handle);
        handle = 0;
    }

    private static native long create(int streamType);
    private static native void destroy(long handle);
    private static native byte[] frame(long handle, byte[] payload, long pts, int sequence, boolean forcePsi);
}
