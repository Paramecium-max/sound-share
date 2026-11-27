package io.soundflow.sender;

import android.os.SystemClock;
import java.util.HashMap;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** JNI wrapper for the shared 40-byte UDP clock service. */
public final class TimerService {
    static { System.loadLibrary("soundflow"); }

    public static class ClientTelemetry {
        public String clientIp;
        public int clientPort;
        public long lastSeq;
        public long lastSeenUs;
        public long residualErrorUs;
        public long packetCount;
    }

    private long handle;

    public static long monotonicNowUs() {
        return SystemClock.elapsedRealtimeNanos() / 1000L;
    }

    public synchronized boolean start() {
        if (handle == 0) handle = nativeStart();
        return handle != 0;
    }

    public synchronized void stop() {
        if (handle != 0) nativeStop(handle);
        handle = 0;
    }

    public synchronized int getPort() {
        return nativePort(handle);
    }

    public synchronized Map<String, ClientTelemetry> snapshotTelemetry() {
        Map<String, ClientTelemetry> result = new HashMap<>();
        String raw = nativeSnapshot(handle);
        if (raw == null) return result;
        try {
            JSONArray list = new JSONArray(raw);
            for (int i = 0; i < list.length(); ++i) {
                JSONObject row = list.getJSONObject(i);
                ClientTelemetry data = new ClientTelemetry();
                data.clientIp = row.getString("ip");
                data.clientPort = row.getInt("port");
                data.lastSeq = row.getLong("seq");
                data.lastSeenUs = row.getLong("seen");
                data.residualErrorUs = row.getLong("residual");
                data.packetCount = row.getLong("count");
                result.put(data.clientIp, data);
            }
        } catch (JSONException e) {
            throw new IllegalStateException("原生校时状态格式无效", e);
        }
        return result;
    }

    private static native long nativeStart();
    private static native void nativeStop(long timer);
    private static native int nativePort(long timer);
    private static native String nativeSnapshot(long timer);
}
