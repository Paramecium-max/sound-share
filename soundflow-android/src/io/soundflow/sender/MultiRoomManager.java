package io.soundflow.sender;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.media.projection.MediaProjection;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.text.Collator;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Lightweight Android platform bridge over the shared native MultiRoomEngine (NativeSender):
 * - All protocol state machines (mDNS, 8899 ECDH/AES/TLV, WFD RTSP M1-M7, NDK MediaCodec AAC / LPCM,
 *   MPEG-TS/RTP packetizer, and UDP TimerService) execute inside libsoundflow.so.
 * - Java owns only Android platform integrations: AudioPlaybackCaptureConfiguration (per-app UID
 *   isolation or global self-UID exclusion), SAF WAV/LRC import, MediaSessionMonitorService, and
 *   Android 14/15 foreground service promotion.
 */
public final class MultiRoomManager {
    public interface UiActionDelegate {
        void onRequestSystemAudioCapture();
        void onSenderWillStartStreaming();
    }

    public interface EventSink {
        void onSenderEvent(String eventJson);
    }

    private static final long MAX_WAV_IMPORT_BYTES = 128L * 1024L * 1024L;
    private static volatile MultiRoomManager INSTANCE;

    public static MultiRoomManager getInstance() {
        if (INSTANCE == null) {
            synchronized (MultiRoomManager.class) {
                if (INSTANCE == null) INSTANCE = new MultiRoomManager();
            }
        }
        return INSTANCE;
    }

    private final NativeSender nativeSender = new NativeSender();
    private final AudioCaptureEngine audioEngine = new AudioCaptureEngine();
    private final ShizukuCaptureBridge shizukuBridge = new ShizukuCaptureBridge();
    private final WirelessAdbCaptureBridge adbBridge = new WirelessAdbCaptureBridge();
    private final ArrayDeque<String> logBuffer = new ArrayDeque<>(160);

    private final AtomicBoolean initialized = new AtomicBoolean(false);
    private final ExecutorService discoveryWorker = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "SoundFlow-SenderDiscovery");
        thread.setDaemon(true);
        return thread;
    });
    private final AtomicBoolean discoveryActive = new AtomicBoolean(false);
    private final AtomicBoolean multiroomRunning = new AtomicBoolean(false);
    private final AtomicLong lastBroadcastUs = new AtomicLong(0L);

    private volatile boolean uiVisible = false;
    private volatile boolean senderTabActive = false;
    private volatile boolean senderStageActive = false;
    private volatile int uiSourceMode = AudioCaptureEngine.MODE_SYSTEM_LOOPBACK;
    private volatile boolean preferLpcm = false;
    private volatile int sampleRate = 48000;
    private volatile int basePlayDelayMs = 300;
    private volatile boolean userExplicitGlobalApp = false;
    private volatile int targetAppUid = -1;
    private volatile String targetAppPkg = "";
    private volatile String targetAppName = "";
    private volatile String captureBackend = "media_projection";
    private volatile boolean muteLocalApp = false;
    private volatile String platformError = "";
    private volatile String activeWavPath = "";

    private volatile Context appContext;
    private volatile UiActionDelegate uiDelegate;
    private volatile EventSink eventSink;
    private volatile JSONObject cachedNativeState = new JSONObject();

    private volatile String extTitle = "";
    private volatile String extArtist = "";
    private volatile String extAlbum = "";
    private volatile long extDurationMs = 0L;
    private volatile long extPositionMs = 0L;
    private volatile boolean extPaused = false;
    private volatile String extLyricsRaw = "";
    private volatile String extLyricLine = "";
    private volatile List<AudioCaptureEngine.LrcLine> extLrcLines = Collections.emptyList();

    private MultiRoomManager() {
        audioEngine.setMode(AudioCaptureEngine.MODE_SYSTEM_LOOPBACK, "");
        callNativeQuiet("media.setRemoteControlHandledByHost", new JSONObjectMap().put("handled", true));
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
            autoBindCurrentlyPlayingAppIfUnset();
        }
        syncNativeSourceMode();
    }

    private static final class JSONObjectMap {
        final JSONObject obj = new JSONObject();
        JSONObjectMap put(String k, Object v) {
            try { obj.put(k, v); } catch (Exception ignored) {}
            return this;
        }
    }

    private String sourceDeviceName() {
        return (Build.MODEL != null && !Build.MODEL.isEmpty()) ? Build.MODEL : "SoundFlow Android";
    }

    private JSONObject callNative(String method, JSONObject params) {
        try {
            JSONObject req = new JSONObject();
            req.put("id", "jni");
            req.put("method", method);
            if (params != null) req.put("params", params);
            String raw = nativeSender.request(req.toString());
            return raw != null ? new JSONObject(raw) : new JSONObject().put("error", "native_unavailable");
        } catch (Exception e) {
            try {
                return new JSONObject().put("error", e.getMessage());
            } catch (Exception ignored) {
                return new JSONObject();
            }
        }
    }

    private void callNativeQuiet(String method, JSONObjectMap map) {
        callNative(method, map != null ? map.obj : null);
    }

    public void attachContext(Context context) {
        if (context == null) return;
        this.appContext = context.getApplicationContext();
        if (!initialized.compareAndSet(false, true)) return;
        adbBridge.init(this.appContext);
        shizukuBridge.register(() -> {
            refreshCaptureBackend();
            broadcastStateThrottled(true);
        });
        refreshCaptureBackend();
        new Thread(() -> initOnBackground(this.appContext), "SoundFlow-SenderInit").start();
    }

    public synchronized void refreshCaptureBackend() {
        Context ctx = this.appContext;
        if (ctx == null) return;
        if (PrivilegedAudioPolicyHelper.hasDirectPrivilegedRouting(ctx)) {
            if (!"privileged_policy".equals(captureBackend) || !audioEngine.hasMediaProjection()) {
                captureBackend = "privileged_policy";
                audioEngine.setPrivilegedPolicyContext(ctx);
                platformError = "";
                logSender("已启用系统特权免投屏直抓通道 (AudioPolicy)");
            }
            return;
        }
        if (adbBridge.isDaemonAlive()) {
            if (!"wireless_adb".equals(captureBackend) || !audioEngine.hasMediaProjection()) {
                captureBackend = "wireless_adb";
                audioEngine.setWirelessAdbBridge(ctx, adbBridge);
                platformError = "";
                logSender("已启用无线 ADB 免投屏直抓通道 (UID 2000)");
            }
            return;
        }
        if (shizukuBridge.isAvailable() && shizukuBridge.isGranted()) {
            if (!"shizuku".equals(captureBackend) || !audioEngine.hasMediaProjection()) {
                captureBackend = "shizuku";
                audioEngine.setShizukuBridge(ctx, shizukuBridge);
                platformError = "";
                logSender("已启用 Shizuku 免投屏直抓通道 (UID 2000)");
            }
            return;
        }
        if (!"media_projection".equals(captureBackend)) {
            captureBackend = "media_projection";
            audioEngine.revokeMediaProjection(null);
        }
    }

    public synchronized void setMuteLocalApp(boolean mute) throws Exception {
        if (mute && targetAppUid <= 0 && !userExplicitGlobalApp) {
            autoBindCurrentlyPlayingAppIfUnset();
        }
        if (this.muteLocalApp == mute) return;
        boolean restart = multiroomRunning.get() && uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK;
        boolean lpcm = preferLpcm;
        int rate = sampleRate;
        int delay = basePlayDelayMs;
        if (restart) {
            audioEngine.stop();
            JSONObject stopped = callNative("multiroom.stop", null);
            multiroomRunning.set(false);
            if (!stopped.optBoolean("result", false)) {
                throw new IllegalStateException(stopped.optString("error", "无法确认旧音频流已停止"));
            }
        }
        this.muteLocalApp = mute;
        audioEngine.setMuteLocalApp(mute);
        if (restart) {
            if (!startMultiroom(lpcm, rate, delay)) {
                throw new IllegalStateException(platformError);
            }
        } else {
            reconcileAudioCaptureWorker();
        }
        broadcastStateThrottled(true);
    }

    public void setUiActionDelegate(UiActionDelegate delegate) {
        this.uiDelegate = delegate;
    }

    public void setEventSink(EventSink sink) {
        this.eventSink = sink;
    }

    public void setUiVisibility(boolean visible, boolean onSenderTab) {
        this.uiVisible = visible;
        this.senderTabActive = onSenderTab;
        reconcileDiscoveryLifecycle();
        if (visible && onSenderTab) {
            pollNativeOnce(true);
        }
    }

    public void setSenderStageActive(boolean active) {
        this.senderStageActive = active;
        if (active) pollNativeOnce(true);
    }

    public boolean isMultiroomRunning() {
        return multiroomRunning.get();
    }

    private synchronized void logSender(String text) {
        String entry = System.currentTimeMillis() + " [sender] " + text;
        if (logBuffer.size() >= 150) logBuffer.pollFirst();
        logBuffer.addLast(entry);
    }

    public synchronized String readLogs() {
        if (logBuffer.isEmpty()) return "";
        StringBuilder sb = new StringBuilder();
        for (String line : logBuffer) sb.append(line).append('\n');
        return sb.toString();
    }

    private void emitEvent(String json) {
        EventSink sink = this.eventSink;
        if (sink != null && uiVisible) {
            sink.onSenderEvent(json);
        }
    }

    private void reconcileDiscoveryLifecycle() {
        discoveryWorker.execute(() -> {
            boolean desired = multiroomRunning.get() || (uiVisible && senderTabActive);
            if (desired == discoveryActive.get()) return;
            JSONObject reply = callNative(desired ? "discovery.start" : "discovery.stop", null);
            if (!reply.optBoolean("result", false)) {
                logSender(desired ? "原生 mDNS 启动失败" : "原生 mDNS 停止失败");
                return;
            }
            discoveryActive.set(desired);
            pollNativeOnce(true);
        });
    }

    private void initOnBackground(Context context) {
        try {
            ConnectivityManager connectivity = context.getSystemService(ConnectivityManager.class);
            if (connectivity != null) {
                try {
                    NetworkRequest request = new NetworkRequest.Builder()
                            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                            .build();
                    connectivity.registerNetworkCallback(request, new ConnectivityManager.NetworkCallback() {
                        @Override public void onAvailable(Network network) {
                            if (discoveryActive.get()) {
                                callNativeQuiet("discovery.refresh", new JSONObjectMap().put("automatic", true));
                            }
                        }
                        @Override public void onLost(Network network) {
                            if (discoveryActive.get()) {
                                callNativeQuiet("discovery.refresh", new JSONObjectMap().put("automatic", true));
                            }
                        }
                        @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                            if (discoveryActive.get()) {
                                callNativeQuiet("discovery.refresh", new JSONObjectMap().put("automatic", true));
                            }
                        }
                    });
                } catch (RuntimeException ignored) {}
            }
            reconcileDiscoveryLifecycle();
            Thread pollThread = new Thread(() -> {
                int posTickDiv = 0;
                while (initialized.get()) {
                    boolean fast = multiroomRunning.get() || (uiVisible && (senderTabActive || senderStageActive));
                    try {
                        Thread.sleep(fast ? 110L : 850L);
                    } catch (InterruptedException ignored) {
                        break;
                    }
                    if (!fast) continue;
                    posTickDiv++;
                    if (posTickDiv >= 8) {
                        posTickDiv = 0;
                        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
                            MediaSessionMonitorService.pollCurrentPosition(appContext);
                        }
                    }
                    pollNativeOnce(false);
                }
            }, "SoundFlow-NativeSenderPoll");
            pollThread.setDaemon(true);
            pollThread.start();
        } catch (Throwable ignored) {}
    }

    private void syncNativeSourceMode() {
        int m = this.uiSourceMode;
        if (m == AudioCaptureEngine.MODE_TEST_TONE) {
            callNativeQuiet("source.setMode", new JSONObjectMap().put("mode", 0).put("param", ""));
        } else if (m == AudioCaptureEngine.MODE_WAV_FILE && !activeWavPath.isEmpty()) {
            callNativeQuiet("source.setMode", new JSONObjectMap().put("mode", 1).put("param", activeWavPath));
        } else {
            // Android system / per-app capture feeds stereo PCM into native ExternalPcm (mode = 3).
            callNativeQuiet("source.setMode", new JSONObjectMap().put("mode", 3).put("param", "android-playback-capture"));
            pushEffectiveSystemMetadata(true);
        }
        reconcileAudioCaptureWorker();
    }

    private boolean isLocalMuteTakeoverDesired() {
        return muteLocalApp
                && uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK
                && ("wireless_adb".equals(captureBackend)
                        || "privileged_policy".equals(captureBackend)
                        || "shizuku".equals(captureBackend));
    }

    private synchronized void reconcileAudioCaptureWorker() {
        boolean needJavaCapture = uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK
                && (multiroomRunning.get() || isLocalMuteTakeoverDesired());
        if (!needJavaCapture) {
            audioEngine.stop();
            return;
        }
        int rate = this.sampleRate > 0 ? this.sampleRate : 48000;
        int frames = this.preferLpcm ? ((rate == 44100) ? 441 : 480) : 1024;
        if (audioEngine.getMode() != AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            audioEngine.setMode(AudioCaptureEngine.MODE_SYSTEM_LOOPBACK, "");
        }
        // If capture is already running (e.g. local-mute takeover before clicking Start),
        // keep the active AudioPolicy session uninterrupted instead of tearing it down.
        if (audioEngine.isRunningWith(rate, frames)) {
            return;
        }
        long originUs = System.nanoTime() / 1000L;
        audioEngine.start(rate, frames, originUs, new AudioCaptureEngine.Callbacks() {
            @Override
            public void onPcmFrame(byte[] stereoPcmLe, int sampleFrames, long samplePosition) {
                if (multiroomRunning.get()) {
                    nativeSender.pushPcm(stereoPcmLe, sampleFrames);
                }
            }

            @Override
            public void onSpectrum(float[] bands16) {
                // Native AudioPipeline computes and emits 16-band spectrum events directly.
            }
        });
    }

    private void pushEffectiveSystemMetadata(boolean broadcast) {
        if (uiSourceMode != AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) return;
        String fallbackTitle = audioEngine.hasMediaProjection()
                ? ((targetAppUid > 0 && !targetAppName.isEmpty()) ? ("单应用抓音：" + targetAppName) : "手机全局音频抓取中")
                : "等待授予音频内录权限";
        String fallbackArtist = (targetAppUid > 0 && !targetAppName.isEmpty())
                ? ("App Exclusive · " + targetAppName)
                : "AudioPlaybackCapture";
        String title = !extTitle.isEmpty() ? extTitle : fallbackTitle;
        String artist = !extArtist.isEmpty() ? extArtist : fallbackArtist;
        String album = !extAlbum.isEmpty() ? extAlbum : "SoundFlow Multi-Room";
        JSONObject params = new JSONObjectMap()
                .put("title", title)
                .put("artist", artist)
                .put("album", album)
                .put("durationMs", (int) Math.min(86_400_000L, Math.max(0L, extDurationMs)))
                .put("positionMs", (int) Math.min(86_400_000L, Math.max(0L, extPositionMs)))
                .put("lyrics", extLyricsRaw == null ? "" : extLyricsRaw)
                .put("lyricLine", extLyricLine == null ? "" : extLyricLine)
                .put("sourceName", sourceDeviceName())
                .put("broadcast", broadcast)
                .obj;
        callNative("media.setMetadata", params);
    }

    synchronized void onProjectionReleased() {
        audioEngine.stop();
        audioEngine.revokeMediaProjection(null);
    }

    public synchronized void onMediaProjectionGranted(MediaProjection mp) {
        audioEngine.setMediaProjection(mp);
        platformError = "";
        uiSourceMode = AudioCaptureEngine.MODE_SYSTEM_LOOPBACK;
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
            autoBindCurrentlyPlayingAppIfUnset();
        }
        syncNativeSourceMode();
        logSender("已获得系统音频内录授权 (已排除自身 UID 回环)");
        broadcastStateThrottled(true);
    }

    public void onMediaProjectionDenied() {
        audioEngine.revokeMediaProjection("未授予系统投屏内录权限，可直接使用「无线 ADB 免投屏」或点击「授权内录」");
        platformError = audioEngine.getLastError();
        // Stay on MODE_SYSTEM_LOOPBACK so the user can directly activate Wireless ADB without being kicked back.
        if (uiSourceMode != AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            uiSourceMode = AudioCaptureEngine.MODE_TEST_TONE;
        }
        syncNativeSourceMode();
        broadcastStateThrottled(true);
    }

    public synchronized void onMediaProjectionRevoked(String reason) {
        audioEngine.stop();
        audioEngine.revokeMediaProjection(reason);
        platformError = reason != null ? reason : "";
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            uiSourceMode = AudioCaptureEngine.MODE_TEST_TONE;
            if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
            autoBindCurrentlyPlayingAppIfUnset();
        }
        syncNativeSourceMode();
        }
        logSender("系统内录权限已结束: " + platformError);
        broadcastStateThrottled(true);
    }

    private static String sanitizeFileName(String displayName, String fallback) {
        if (displayName == null || displayName.trim().isEmpty()) return fallback;
        String clean = displayName.trim().replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_");
        return clean.isEmpty() ? fallback : clean;
    }

    public void onWavFileSelected(Context ctx, Uri uri, String displayName) {
        if (ctx == null || uri == null) return;
        File dir = new File(ctx.getFilesDir(), "sender_media");
        if (!dir.exists()) dir.mkdirs();
        String safeName = sanitizeFileName(displayName, "Local.wav");
        if (!safeName.toLowerCase(Locale.US).endsWith(".wav")) safeName += ".wav";
        File dst = new File(dir, safeName);
        File tmp = new File(dir, ".import_wav.tmp");
        long total = 0L;
        try (InputStream in = ctx.getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(tmp)) {
            if (in == null) {
                platformError = "无法打开所选 WAV 音频文件";
                broadcastStateThrottled(true);
                return;
            }
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > MAX_WAV_IMPORT_BYTES) {
                    tmp.delete();
                    platformError = "WAV 文件超过 128 MB 大小限制";
                    broadcastStateThrottled(true);
                    return;
                }
                out.write(buf, 0, n);
            }
        } catch (Exception e) {
            tmp.delete();
            platformError = "读取 WAV 文件失败: " + e.getMessage();
            broadcastStateThrottled(true);
            return;
        }

        File[] oldFiles = dir.listFiles((d, name) -> name.endsWith(".wav") && !name.equals(dst.getName()));
        if (oldFiles != null) {
            for (File f : oldFiles) f.delete();
        }
        if (dst.exists()) dst.delete();
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            platformError = "缓存本地 WAV 文件失败";
            broadcastStateThrottled(true);
            return;
        }

        JSONObject resp = callNative("source.setMode",
                new JSONObjectMap().put("mode", 1).put("param", dst.getAbsolutePath()).obj);
        String err = resp.optString("error", "");
        if (!err.isEmpty()) {
            dst.delete();
            platformError = err;
            broadcastStateThrottled(true);
            return;
        }
        this.activeWavPath = dst.getAbsolutePath();
        this.uiSourceMode = AudioCaptureEngine.MODE_WAV_FILE;
        this.platformError = "";
        reconcileAudioCaptureWorker();
        logSender("已通过原生核加载本地无损 WAV: " + safeName);
        pollNativeOnce(true);
    }

    public void onLrcFileSelected(Context ctx, Uri uri) {
        if (ctx == null || uri == null) return;
        try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
            if (in == null) {
                platformError = "无法打开 LRC 歌词文件";
                broadcastStateThrottled(true);
                return;
            }
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) {
                bos.write(buf, 0, n);
                if (bos.size() > 1024 * 1024) break;
            }
            byte[] raw = bos.toByteArray();
            String text;
            try {
                text = StandardCharsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(raw)).toString();
            } catch (Throwable notUtf8) {
                text = Charset.forName("GB18030").decode(ByteBuffer.wrap(raw)).toString();
            }
            if (text.trim().isEmpty()) {
                platformError = "LRC 歌词文件内容为空";
                broadcastStateThrottled(true);
                return;
            }
            String cleanText = text.startsWith("\uFEFF") ? text.substring(1) : text;
            File dir = new File(ctx.getFilesDir(), "sender_media");
            if (!dir.exists()) dir.mkdirs();
            File lrcFile = new File(dir, "active_bound.lrc");
            try (FileOutputStream fos = new FileOutputStream(lrcFile)) {
                fos.write(cleanText.getBytes(StandardCharsets.UTF_8));
            }
            synchronized (this) {
                extLyricsRaw = cleanText;
                extLrcLines = AudioCaptureEngine.parseLrcLines(extLyricsRaw);
                extLyricLine = AudioCaptureEngine.lrcLineAt(extLrcLines, extPositionMs);
            }
            JSONObject resp = callNative("media.loadLrcFile",
                    new JSONObjectMap().put("path", lrcFile.getAbsolutePath()).obj);
            String err = resp.optString("error", "");
            if (!err.isEmpty()) {
                platformError = err;
            } else {
                platformError = "";
                logSender("已绑定发送端同步 LRC 歌词 (" + extLrcLines.size() + " 行)");
            }
            pollNativeOnce(true);
        } catch (Exception e) {
            platformError = "导入 LRC 歌词失败: " + e.getMessage();
            broadcastStateThrottled(true);
        }
    }

    public void onNotificationAccessStateChanged() {
        broadcastStateThrottled(true);
    }

    public synchronized void onSystemMediaUpdate(String packageName, String title, String artist, String album,
                                    long durationMs, long positionMs, boolean paused, boolean forceMetadata) {
        if (!targetAppPkg.isEmpty() && !targetAppPkg.equals(packageName)) return;
        String safeTitle = title == null ? "" : title;
        String safeArtist = artist == null ? "" : artist;
        String safeAlbum = album == null ? "" : album;
        boolean trackChanged = !extTitle.equals(safeTitle)
                || !extArtist.equals(safeArtist)
                || !extAlbum.equals(safeAlbum);
        extTitle = safeTitle;
        extArtist = safeArtist;
        extAlbum = safeAlbum;
        extDurationMs = Math.max(0L, durationMs);
        extPositionMs = Math.max(0L, positionMs);
        extPaused = paused;
        if (trackChanged) {
            extLyricsRaw = "";
            extLrcLines = Collections.emptyList();
            extLyricLine = "";
        } else if (!extLrcLines.isEmpty()) {
            extLyricLine = AudioCaptureEngine.lrcLineAt(extLrcLines, extPositionMs);
        }
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            if (trackChanged || forceMetadata) {
                pushEffectiveSystemMetadata(multiroomRunning.get());
                broadcastStateThrottled(false);
            } else if (multiroomRunning.get() && (extDurationMs > 0L || extPositionMs > 0L)) {
                callNativeQuiet("media.setPosition", new JSONObjectMap()
                        .put("positionMs", (int) Math.min(86_400_000L, extPositionMs))
                        .put("broadcast", true));
            }
        }
    }

    public synchronized void onExternalLyricLine(String packageName, String lyricLine) {
        if (!targetAppPkg.isEmpty() && !targetAppPkg.equals(packageName)) return;
        if (lyricLine == null || lyricLine.trim().isEmpty()) return;
        String clean = lyricLine.trim();
        if (clean.equals(extLyricLine)) return;
        extLyricLine = clean;
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            pushEffectiveSystemMetadata(multiroomRunning.get());
            broadcastStateThrottled(false);
        }
    }

    private synchronized void pollNativeOnce(boolean forceEmitState) {
        String raw = nativeSender.poll();
        if (raw == null || raw.isEmpty()) return;
        try {
            JSONObject bundle = new JSONObject(raw);
            JSONObject data = bundle.optJSONObject("data");
            if (data != null) {
                String prevStr = cachedNativeState.toString();
                cachedNativeState = data;
                boolean nativeRunning = data.optBoolean("multiroomRunning", false);
                multiroomRunning.set(nativeRunning);
                if (nativeRunning) {
                    preferLpcm = "LPCM".equals(data.optString("codec", preferLpcm ? "LPCM" : "AAC_LC"));
                    sampleRate = data.optInt("sampleRate", sampleRate);
                }
                basePlayDelayMs = data.optInt("basePlayDelayMs", basePlayDelayMs);
                if (forceEmitState || !prevStr.equals(data.toString())) {
                    broadcastStateThrottled(forceEmitState);
                }
            }
            JSONArray events = bundle.optJSONArray("events");
            if (events != null) {
                for (int i = 0; i < events.length(); i++) {
                    JSONObject ev = events.optJSONObject(i);
                    if (ev == null) continue;
                    String type = ev.optString("type", "");
                    if ("remoteControl".equals(type)) {
                        String act = ev.optString("action", "");
                        long posMs = ev.optLong("positionMs", 0L);
                        boolean handled = ev.optBoolean("handledLocally", false);
                        if (!handled && uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
                            handled = MediaSessionMonitorService.dispatchControl(appContext, act, posMs);
                            ev.put("handledLocally", handled);
                        }
                        emitEvent(ev.toString());
                        if (handled) broadcastStateThrottled(true);
                    } else if ("spectrum".equals(type)) {
                        if (uiVisible && (senderTabActive || senderStageActive)) {
                            emitEvent(ev.toString());
                        }
                    } else {
                        emitEvent(ev.toString());
                    }
                }
            }
        } catch (Exception ignored) {}
    }

    public synchronized boolean startMultiroom(boolean lpcm, int sr, int delayMs) {
        if (multiroomRunning.get()) return true;
        UiActionDelegate delegate = this.uiDelegate;
        if (delegate != null) {
            delegate.onSenderWillStartStreaming();
        }
        if (appContext != null) {
            try {
                appContext.startForegroundService(new Intent(appContext, SenderService.class)
                        .setAction(SenderService.ACTION_START));
            } catch (Throwable ignored) {}
        }

        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
            autoBindCurrentlyPlayingAppIfUnset();
        }
        syncNativeSourceMode();
        int clampedDelay = Math.max(40, Math.min(800, delayMs));
        JSONObject resp = callNative("multiroom.start", new JSONObjectMap()
                .put("codec", lpcm ? "LPCM" : "AAC_LC")
                .put("sampleRate", sr > 0 ? sr : 48000)
                .put("basePlayDelayMs", clampedDelay)
                .obj);
        String err = resp.optString("error", "");
        if (!err.isEmpty()) {
            platformError = err;
            if (appContext != null) {
                try {
                    appContext.startService(new Intent(appContext, SenderService.class)
                            .setAction(SenderService.ACTION_STOP));
                } catch (Throwable ignored) {}
            }
            pollNativeOnce(true);
            return false;
        }
        platformError = "";
        multiroomRunning.set(true);
        pollNativeOnce(false);
        reconcileDiscoveryLifecycle();
        reconcileAudioCaptureWorker();
        logSender("原生全屋同步推流已启动 (" + (preferLpcm ? "LPCM" : "AAC-LC") + " " + sampleRate + "Hz, delay=" + basePlayDelayMs + "ms)");
        broadcastStateThrottled(true);
        return true;
    }

    public synchronized void stopMultiroom() {
        stopNativeStreaming();
        if (appContext != null) appContext.stopService(new Intent(appContext, SenderService.class));
    }

    synchronized void stopNativeStreaming() {
        boolean wasRunning = multiroomRunning.getAndSet(false);
        callNative("multiroom.stop", null);
        reconcileAudioCaptureWorker();
        if (wasRunning) {
            logSender("原生全屋同步推流已停止");
        }
        reconcileDiscoveryLifecycle();
        pollNativeOnce(true);
    }

    private synchronized void changeTargetApp(int uid, String pkg, String label) throws Exception {
        if (uid <= 0) {
            uid = -1;
            pkg = "";
            label = "";
        } else {
            Context ctx = appContext;
            if (ctx == null || pkg.isEmpty() || uid == android.os.Process.myUid()
                    || ctx.getPackageManager().getApplicationInfo(pkg, 0).uid != uid) {
                throw new IllegalArgumentException("音源应用已变化，请刷新列表");
            }
            if (label.isEmpty()) label = pkg;
        }
        if (targetAppUid == uid && targetAppPkg.equals(pkg)) return;

        boolean restart = multiroomRunning.get() && uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK;
        boolean lpcm = preferLpcm;
        int rate = sampleRate;
        int delay = basePlayDelayMs;
        if (restart) {
            // Joining both capture and native senders retires the PCM, AAC, RTP and TCP
            // queues before any frame for the next UID can be accepted.
            audioEngine.stop();
            JSONObject stopped = callNative("multiroom.stop", null);
            multiroomRunning.set(false);
            if (!stopped.optBoolean("result", false)) {
                throw new IllegalStateException(stopped.optString("error", "无法确认旧音源已停止"));
            }
        }
        targetAppUid = uid;
        targetAppPkg = pkg;
        targetAppName = label;
        extTitle = extArtist = extAlbum = extLyricsRaw = extLyricLine = "";
        extLrcLines = Collections.emptyList();
        extDurationMs = extPositionMs = 0L;
        extPaused = true;
        audioEngine.setTargetAppUid(uid, label);
        MediaSessionMonitorService.setPreferredPackage(pkg);
        if (restart && !startMultiroom(lpcm, rate, delay)) {
            throw new IllegalStateException(platformError);
        }
        pushEffectiveSystemMetadata(multiroomRunning.get());
        broadcastStateThrottled(true);
    }

    public String handleRpcRequest(String reqJson) {
        try {
            JSONObject req = new JSONObject(reqJson);
            String id = req.optString("id", "0");
            String method = req.optString("method", "");
            Object result = Boolean.TRUE;
            String error = null;

            switch (method) {
                case "state.get":
                    pollNativeOnce(false);
                    result = buildStateJson();
                    break;
                case "net.listInterfaces": {
                    JSONObject r = callNative("net.listInterfaces", null);
                    if (r.has("error")) error = r.optString("error");
                    else {
                        JSONArray all = r.optJSONArray("result");
                        JSONArray filtered = new JSONArray();
                        if (all != null) {
                            for (int i = 0; i < all.length(); i++) {
                                JSONObject item = all.optJSONObject(i);
                                if (item != null && !item.optBoolean("isLoopback", false)) {
                                    filtered.put(item);
                                }
                            }
                        }
                        result = filtered;
                    }
                    break;
                }
                case "net.setBindIp": {
                    String ip = req.optString("ip", "").trim();
                    JSONObject r = callNative("net.setBindIp", new JSONObjectMap().put("ip", ip).obj);
                    if (r.has("error")) error = r.optString("error");
                    else {
                        pollNativeOnce(true);
                        result = buildStateJson();
                    }
                    break;
                }
                case "discovery.refresh": {
                    reconcileDiscoveryLifecycle();
                    callNative("discovery.refresh",
                            new JSONObjectMap().put("automatic", req.optBoolean("automatic", false)).obj);
                    break;
                }
                case "device.addManual":
                case "device.remove":
                case "multiroom.addDevice":
                case "multiroom.removeDevice":
                case "sync.setBaseDelay":
                case "sync.setDeviceBias":
                case "volume.setDevice":
                case "volume.setMaster": {
                    JSONObject r = callNative(method, req);
                    if (r.has("error")) error = r.optString("error");
                    else result = r.opt("result");
                    pollNativeOnce(true);
                    break;
                }
                case "multiroom.start": {
                    boolean lpcm = "LPCM".equals(req.optString("codec", preferLpcm ? "LPCM" : "AAC_LC"));
                    int sr = req.optInt("sampleRate", sampleRate);
                    int delay = req.optInt("basePlayDelayMs", basePlayDelayMs);
                    boolean ok = startMultiroom(lpcm, sr, delay);
                    if (!ok) error = platformError.isEmpty() ? "启动全屋播放失败" : platformError;
                    result = ok;
                    break;
                }
                case "multiroom.stop":
                    stopMultiroom();
                    break;
                case "codec.set": {
                    boolean nextLpcm = "LPCM".equals(req.optString("codec", "AAC_LC"));
                    int nextSr = req.optInt("sampleRate", 48000);
                    if (!nextLpcm) nextSr = 48000;
                    else if (nextSr != 48000 && nextSr != 44100) nextSr = 48000;
                    synchronized (this) {
                        this.preferLpcm = nextLpcm;
                        this.sampleRate = nextSr;
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "source.setMode": {
                    int mode = req.optInt("mode", AudioCaptureEngine.MODE_SYSTEM_LOOPBACK);
                    String param = req.optString("param", "");
                    boolean requestProjection = req.optBoolean("requestProjection", false);
                    if (mode == AudioCaptureEngine.MODE_WAV_FILE && activeWavPath.isEmpty() && param.isEmpty()) {
                        error = "请先选择本地 WAV 音频文件";
                        break;
                    }
                    this.uiSourceMode = mode;
                    this.platformError = "";
                    refreshCaptureBackend();
                    audioEngine.setMode(mode, param);
                    if (mode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && !audioEngine.hasMediaProjection()) {
                        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
                            autoBindCurrentlyPlayingAppIfUnset();
                        }
                        syncNativeSourceMode();
                        // Only launch the system screen-capture (MediaProjection) dialog when the user explicitly
                        // clicks "授权内录" (requestProjection=true), never when switching tabs or using Wireless ADB.
                        if (requestProjection && "media_projection".equals(captureBackend) && uiDelegate != null) {
                            uiDelegate.onRequestSystemAudioCapture();
                        }
                    } else {
                        if (mode == AudioCaptureEngine.MODE_WAV_FILE && !param.isEmpty()) {
                            activeWavPath = param;
                        }
                        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && targetAppUid <= 0 && !userExplicitGlobalApp) {
                            autoBindCurrentlyPlayingAppIfUnset();
                        }
                        syncNativeSourceMode();
                    }
                    pollNativeOnce(true);
                    break;
                }
                case "source.setMuteLocalApp": {
                    setMuteLocalApp(req.optBoolean("muteLocalApp", false));
                    result = buildStateJson();
                    break;
                }
                case "shizuku.requestPermission": {
                    boolean requested = shizukuBridge.requestPermission();
                    if (!requested) error = "未检测到运行中的 Shizuku 服务，可使用「无线 ADB 免投屏」直连本机 5555 端口";
                    result = requested;
                    refreshCaptureBackend();
                    broadcastStateThrottled(true);
                    break;
                }
                case "adb.connect": {
                    int port = req.optInt("port", adbBridge.getConfiguredPort());
                    boolean enableMute = req.optBoolean("enableMute", false);
                    boolean ok = adbBridge.connectAndStartDaemon(appContext, port);
                    refreshCaptureBackend();
                    if (!ok) {
                        error = adbBridge.getLastError();
                    } else {
                        if (targetAppUid <= 0 && !userExplicitGlobalApp) {
                            autoBindCurrentlyPlayingAppIfUnset();
                        }
                        if (enableMute) {
                            try {
                                setMuteLocalApp(true);
                            } catch (Exception muteEx) {
                                error = muteEx.getMessage();
                            }
                        }
                    }
                    result = buildStateJson();
                    broadcastStateThrottled(true);
                    break;
                }
                case "adb.disconnect": {
                    if (muteLocalApp && "wireless_adb".equals(captureBackend)) {
                        try { setMuteLocalApp(false); } catch (Exception ignored) {}
                    }
                    adbBridge.disconnect(appContext);
                    refreshCaptureBackend();
                    result = buildStateJson();
                    broadcastStateThrottled(true);
                    break;
                }
                case "app.listAudioApps":
                    result = listInstalledAudioApps();
                    break;
                case "source.setTargetApp": {
                    int reqUid = req.optInt("uid", -1);
                    userExplicitGlobalApp = (reqUid <= 0);
                    changeTargetApp(reqUid, req.optString("packageName", "").trim(),
                            req.optString("label", "").trim());
                    break;
                }
                case "wav.pause":
                    callNative("wav.pause", new JSONObjectMap().put("paused", req.optBoolean("paused", true)).obj);
                    pollNativeOnce(true);
                    break;
                case "wav.resume":
                    callNative("wav.pause", new JSONObjectMap().put("paused", false).obj);
                    pollNativeOnce(true);
                    break;
                case "wav.seek":
                    callNative("wav.seek", new JSONObjectMap()
                            .put("positionMs", (int) Math.min(86_400_000L, Math.max(0L, req.optLong("positionMs", 0L)))).obj);
                    pollNativeOnce(true);
                    break;
                case "media.control": {
                    String act = req.optString("action", "");
                    long posMs = req.optLong("positionMs", 0L);
                    if (uiSourceMode == AudioCaptureEngine.MODE_WAV_FILE) {
                        if ("pause".equals(act)) {
                            callNative("wav.pause", new JSONObjectMap().put("paused", true).obj);
                        } else if ("resume".equals(act)) {
                            callNative("wav.pause", new JSONObjectMap().put("paused", false).obj);
                        } else if ("seek".equals(act)) {
                            callNative("wav.seek", new JSONObjectMap()
                                    .put("positionMs", (int) Math.min(86_400_000L, Math.max(0L, posMs))).obj);
                        }
                    } else {
                        result = MediaSessionMonitorService.dispatchControl(appContext, act, posMs);
                    }
                    pollNativeOnce(true);
                    break;
                }
                case "lyrics.getCurrent": {
                    JSONObject state = buildStateJson();
                    JSONObject track = state.optJSONObject("track");
                    JSONObject lrc = new JSONObject();
                    lrc.put("title", track != null ? track.optString("title", "") : "");
                    lrc.put("artist", track != null ? track.optString("artist", "") : "");
                    lrc.put("album", track != null ? track.optString("album", "") : "");
                    lrc.put("durationMs", track != null ? track.optLong("durationMs", 0L) : 0L);
                    lrc.put("positionMs", track != null ? track.optLong("positionMs", 0L) : 0L);
                    lrc.put("lyricsRaw", extLyricsRaw == null ? "" : extLyricsRaw);
                    lrc.put("lyricLine", track != null ? track.optString("lyricLine", "") : "");
                    lrc.put("codec", (preferLpcm ? "LPCM " : "AAC-LC ") + (sampleRate / 1000) + "kHz");
                    lrc.put("paused", track != null && track.optBoolean("paused", false));
                    result = lrc;
                    break;
                }
                default:
                    error = "unknown method: " + method;
                    break;
            }

            JSONObject resp = new JSONObject();
            resp.put("type", "rpc");
            resp.put("id", id);
            if (error != null) resp.put("error", error);
            else resp.put("result", result);
            return resp.toString();
        } catch (Exception e) {
            try {
                return new JSONObject()
                        .put("type", "rpc")
                        .put("id", "0")
                        .put("error", e.getMessage() != null ? e.getMessage() : "bad_request")
                        .toString();
            } catch (Exception ignored) {
                return "{\"type\":\"rpc\",\"id\":\"0\",\"error\":\"bad_request\"}";
            }
        }
    }

    public synchronized JSONObject buildStateJson() throws Exception {
        JSONObject base = cachedNativeState != null ? new JSONObject(cachedNativeState.toString()) : new JSONObject();
        boolean running = base.optBoolean("multiroomRunning", multiroomRunning.get());
        base.put("codec", running ? base.optString("codec", preferLpcm ? "LPCM" : "AAC_LC")
                                  : (preferLpcm ? "LPCM" : "AAC_LC"));
        base.put("sampleRate", running ? base.optInt("sampleRate", sampleRate) : sampleRate);
        base.put("sourceMode", uiSourceMode);
        base.put("hasProjection", audioEngine.hasMediaProjection());
        base.put("captureBackend", captureBackend);
        boolean shizukuAvail = shizukuBridge.isAvailable();
        boolean shizukuGrant = shizukuBridge.isGranted();
        boolean adbActive = "wireless_adb".equals(captureBackend) || adbBridge.isDaemonAliveCached();
        boolean muteSupported = "privileged_policy".equals(captureBackend)
                || "wireless_adb".equals(captureBackend)
                || "shizuku".equals(captureBackend);
        base.put("shizukuAvailable", shizukuAvail);
        base.put("shizukuGranted", shizukuGrant);
        base.put("adbDaemonActive", adbActive);
        base.put("adbPort", adbBridge.getConfiguredPort());
        base.put("adbLastError", adbBridge.getLastError());
        base.put("muteLocalApp", muteLocalApp);
        base.put("muteLocalSupported", muteSupported);
        base.put("muteLocalHint", muteSupported ? "" : "点击开关或「无线 ADB 免投屏」连接本机调试端口即可开启单应用本机消音");
        base.put("targetAppUid", targetAppUid);
        base.put("targetAppPkg", targetAppPkg);
        base.put("targetAppName", targetAppName);
        base.put("notificationAccessGranted", MediaSessionMonitorService.isNotificationAccessGranted(appContext));
        String nativeErr = base.optString("lastError", "");
        base.put("lastError", !platformError.isEmpty() ? platformError : nativeErr);

        JSONObject track = base.optJSONObject("track");
        if (track == null) {
            track = new JSONObject();
            base.put("track", track);
        }
        if (uiSourceMode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            track.put("paused", extPaused);
            if (extLyricsRaw != null && !extLyricsRaw.isEmpty()) {
                track.put("hasLyrics", true);
            }
        }
        return base;
    }

    private synchronized void autoBindCurrentlyPlayingAppIfUnset() {
        if (targetAppUid > 0 || appContext == null) return;
        if (!"wireless_adb".equals(captureBackend)
                && !"privileged_policy".equals(captureBackend)
                && !"shizuku".equals(captureBackend)) {
            return;
        }
        try {
            PackageManager pm = appContext.getPackageManager();
            String selfPkg = appContext.getPackageName();
            int selfUid = android.os.Process.myUid();

            java.util.Set<Integer> playingUids = adbBridge.queryActivePlaybackUids();
            java.util.Set<String> playingPkgs = MediaSessionMonitorService.getActivePlayingPackages(appContext);

            int bestUid = -1;
            String bestPkg = "";
            String bestLabel = "";

            // Priority 1: app that is BOTH actively outputting media audio AND has an active PLAYING MediaSession
            for (String pkg : playingPkgs) {
                if (pkg == null || pkg.isEmpty() || pkg.equals(selfPkg)) continue;
                try {
                    ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
                    if (ai.uid >= 10000 && ai.uid != selfUid && playingUids.contains(ai.uid)) {
                        bestUid = ai.uid;
                        bestPkg = pkg;
                        CharSequence cs = ai.loadLabel(pm);
                        bestLabel = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                        break;
                    }
                } catch (Throwable ignored) {}
            }

            // Priority 2: first actively outputting media UID from AudioPlaybackConfiguration
            if (bestUid <= 0) {
                for (Integer uid : playingUids) {
                    if (uid == null || uid < 10000 || uid == selfUid) continue;
                    String[] pkgs = pm.getPackagesForUid(uid);
                    if (pkgs == null) continue;
                    for (String pkg : pkgs) {
                        if (pkg == null || pkg.isEmpty() || pkg.equals(selfPkg)) continue;
                        try {
                            ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
                            bestUid = ai.uid;
                            bestPkg = pkg;
                            CharSequence cs = ai.loadLabel(pm);
                            bestLabel = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                            break;
                        } catch (Throwable ignored) {}
                    }
                    if (bestUid > 0) break;
                }
            }

            // Priority 3: first PLAYING MediaSession package
            if (bestUid <= 0) {
                for (String pkg : playingPkgs) {
                    if (pkg == null || pkg.isEmpty() || pkg.equals(selfPkg)) continue;
                    try {
                        ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
                        if (ai.uid >= 10000 && ai.uid != selfUid) {
                            bestUid = ai.uid;
                            bestPkg = pkg;
                            CharSequence cs = ai.loadLabel(pm);
                            bestLabel = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                            break;
                        }
                    } catch (Throwable ignored) {}
                }
            }

            if (bestUid > 0 && !bestPkg.isEmpty()) {
                changeTargetApp(bestUid, bestPkg, bestLabel);
                logSender("已自动锁定当前发声应用为 ADB 指定抓流源: " + bestLabel + " (" + bestPkg + ", uid=" + bestUid + ")");
            }
        } catch (Throwable ignored) {}
    }

    private JSONArray listInstalledAudioApps() {
        JSONArray arr = new JSONArray();
        Context ctx = this.appContext;
        if (ctx == null) return arr;
        try {
            PackageManager pm = ctx.getPackageManager();
            String selfPkg = ctx.getPackageName();
            int selfUid = android.os.Process.myUid();

            java.util.Set<Integer> playingUids = adbBridge.queryActivePlaybackUids();
            java.util.Set<String> playingPkgs = MediaSessionMonitorService.getActivePlayingPackages(ctx);

            Intent mainIntent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            List<ResolveInfo> resolved = pm.queryIntentActivities(mainIntent, 0);
            Map<String, JSONObject> byPkg = new LinkedHashMap<>();
            if (resolved != null) {
                for (ResolveInfo ri : resolved) {
                    if (ri == null || ri.activityInfo == null || ri.activityInfo.applicationInfo == null) continue;
                    ApplicationInfo ai = ri.activityInfo.applicationInfo;
                    String pkg = ai.packageName;
                    if (pkg == null || pkg.equals(selfPkg) || byPkg.containsKey(pkg)) continue;
                    if (ai.uid < 10000 || ai.uid == selfUid) continue;
                    CharSequence cs = ai.loadLabel(pm);
                    String label = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                    boolean isPlaying = playingUids.contains(ai.uid) || playingPkgs.contains(pkg);
                    JSONObject item = new JSONObject();
                    item.put("packageName", pkg);
                    item.put("label", label);
                    item.put("uid", ai.uid);
                    item.put("playing", isPlaying);
                    byPkg.put(pkg, item);
                }
            }

            // Include any actively playing UID/package even if it has no launcher icon
            for (Integer uid : playingUids) {
                if (uid == null || uid < 10000 || uid == selfUid) continue;
                String[] uidPkgs = pm.getPackagesForUid(uid);
                if (uidPkgs == null) continue;
                for (String pkg : uidPkgs) {
                    if (pkg == null || pkg.equals(selfPkg) || byPkg.containsKey(pkg)) continue;
                    try {
                        ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
                        CharSequence cs = ai.loadLabel(pm);
                        String label = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                        JSONObject item = new JSONObject();
                        item.put("packageName", pkg);
                        item.put("label", label);
                        item.put("uid", ai.uid);
                        item.put("playing", true);
                        byPkg.put(pkg, item);
                    } catch (Throwable ignored) {}
                }
            }

            final int selectedUid = this.targetAppUid;
            final String selectedPkg = this.targetAppPkg;
            List<JSONObject> sorted = new ArrayList<>(byPkg.values());
            Collator collator = Collator.getInstance(Locale.CHINA);
            sorted.sort((a, b) -> {
                boolean ap = a.optBoolean("playing", false);
                boolean bp = b.optBoolean("playing", false);
                if (ap != bp) return ap ? -1 : 1;
                boolean as = selectedUid > 0 && a.optInt("uid", -1) == selectedUid
                        && selectedPkg.equals(a.optString("packageName", ""));
                boolean bs = selectedUid > 0 && b.optInt("uid", -1) == selectedUid
                        && selectedPkg.equals(b.optString("packageName", ""));
                if (as != bs) return as ? -1 : 1;
                return collator.compare(a.optString("label", ""), b.optString("label", ""));
            });
            for (JSONObject item : sorted) arr.put(item);
        } catch (Throwable ignored) {}
        return arr;
    }

    public void broadcastStateThrottled(boolean force) {
        if (!uiVisible) return;
        long now = System.nanoTime() / 1000L;
        long prev = lastBroadcastUs.get();
        if (!force && (now - prev) < 220_000L) return;
        lastBroadcastUs.set(now);
        try {
            JSONObject evt = new JSONObject();
            evt.put("type", "state");
            evt.put("data", buildStateJson());
            emitEvent(evt.toString());
        } catch (Exception ignored) {}
    }
}
