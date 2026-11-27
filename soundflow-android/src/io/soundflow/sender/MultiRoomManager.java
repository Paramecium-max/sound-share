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

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Multi-Room MiPlay Sender Coordinator integrated into SoundFlow:
 * - Zero-port direct JSBridge RPC & EventSink (no LocalWsServer TCP port, CSP 'connect-src none' compliant)
 * - Self-echo guard: automatically excludes local host IPs from mDNS discovery and manual device targets
 * - On-demand mDNS discovery: runs only while Sender tab is active or Multi-Room streaming is live
 * - Harmonized with ReceiverService via UiActionDelegate (auto-pauses active local receiver session on stream start)
 */
public final class MultiRoomManager {
    public static final class ManagedDevice {
        public volatile MdnsDiscovery.DiscoveredDevice info;
        String discoveryKey = "";
        String manualName = "";
        String sessionLocalIp = "";
        String sessionRemoteIp = "";
        int sessionRemotePort;
        CompletableFuture<Void> closing = CompletableFuture.completedFuture(null);
        boolean attachPending;
        public volatile boolean inGroup = false;
        public volatile boolean cmdConnected = false;
        public volatile boolean rtspStreaming = false;
        public volatile int volume = 65;
        public volatile int delayBiasMs = 0;
        public volatile int reportedLatencyMs = 0;
        public volatile String remoteVersion = "";
        public volatile String statusText = "待机";
        public volatile long activeSessionId = 0L;
        public CmdSession cmdSession;
        public WfdRtspServer rtspServer;
    }

    public interface UiActionDelegate {
        void onRequestSystemAudioCapture();
        void onSenderWillStartStreaming();
    }

    public interface EventSink {
        void onSenderEvent(String eventJson);
    }

    private static volatile MultiRoomManager INSTANCE;

    public static MultiRoomManager getInstance() {
        if (INSTANCE == null) {
            synchronized (MultiRoomManager.class) {
                if (INSTANCE == null) INSTANCE = new MultiRoomManager();
            }
        }
        return INSTANCE;
    }

    private final MdnsDiscovery discovery = new MdnsDiscovery();
    private final TimerService timerService = new TimerService();
    private final TsAndRtpSender multiSender = new TsAndRtpSender();
    private final AudioCaptureEngine audioEngine = new AudioCaptureEngine();
    {
        audioEngine.setMode(AudioCaptureEngine.MODE_SYSTEM_LOOPBACK, "");
    }
    private final ConcurrentHashMap<String, ManagedDevice> devices = new ConcurrentHashMap<>();
    private final ArrayDeque<String> logBuffer = new ArrayDeque<>(160);

    private final AtomicBoolean initialized = new AtomicBoolean(false);
    private final AtomicBoolean discoveryActive = new AtomicBoolean(false);
    private final AtomicBoolean multiroomRunning = new AtomicBoolean(false);
    private volatile boolean uiVisible = false;
    private volatile boolean senderTabActive = false;

    private long audioSessionId;
    private final AtomicLong lastBroadcastUs = new AtomicLong(0);
    private final AtomicLong nextSessionId = new AtomicLong(1);

    private volatile String bindIp = "127.0.0.1";
    private String explicitBindIp = "";
    private volatile long sessionOriginUs = 0;
    private volatile int sampleRate = 48000;
    private volatile int basePlayDelayMs = 350;
    private volatile boolean preferLpcm = false;
    private volatile int masterVolume = 70;
    private volatile int targetAppUid = -1;
    private volatile String targetAppPkg = "";
    private volatile String targetAppName = "";
    private volatile String lastError = "";
    private volatile Context appContext;
    private volatile UiActionDelegate uiDelegate;
    private volatile EventSink eventSink;
    private Thread telemetryTh;

    private volatile String extTitle = "";
    private volatile String extArtist = "";
    private volatile String extAlbum = "";
    private volatile long extDurationMs = 0L;
    private volatile long extPositionMs = 0L;
    private volatile boolean extPaused = false;
    private volatile String extLyricsRaw = "";
    private volatile String extLyricLine = "";
    private volatile List<AudioCaptureEngine.LrcLine> extLrcLines = Collections.emptyList();
    private volatile String lastSentMediaSig = "";

    public void attachContext(Context context) {
        if (context == null) return;
        this.appContext = context.getApplicationContext();
        if (!initialized.compareAndSet(false, true)) return;
        new Thread(() -> initOnBackground(this.appContext), "SoundFlow-SenderInit").start();
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
            broadcastStateThrottled(true);
        }
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
        boolean shouldRun = multiroomRunning.get() || (uiVisible && senderTabActive);
        if (shouldRun) {
            if (discoveryActive.compareAndSet(false, true)) {
                new Thread(() -> {
                    bindIp = explicitBindIp.isEmpty() ? MdnsDiscovery.pickWifiIpv4() : explicitBindIp;
                    discovery.start(this::onDiscoveryUpdate, this::reconcileStreams);
                    logSender("mDNS 局域网音箱扫描已开启 (" + bindIp + ")");
                    broadcastStateThrottled(true);
                }, "SoundFlow-SenderMdnsStart").start();
            }
        } else if (!multiroomRunning.get()) {
            if (discoveryActive.compareAndSet(true, false)) {
                new Thread(() -> {
                    discovery.stop();
                    logSender("mDNS 局域网扫描已按需休眠");
                }, "SoundFlow-SenderMdnsStop").start();
            }
        }
    }

    private void initOnBackground(Context context) {
        try {
            bindIp = MdnsDiscovery.pickWifiIpv4();
            ConnectivityManager connectivity = context.getSystemService(ConnectivityManager.class);
            if (connectivity != null) {
                try {
                    NetworkRequest request = new NetworkRequest.Builder()
                            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                            .build();
                    connectivity.registerNetworkCallback(request, new ConnectivityManager.NetworkCallback() {
                        @Override public void onAvailable(Network network) {
                            if (discoveryActive.get()) discovery.onNetworkChanged();
                        }
                        @Override public void onLost(Network network) {
                            if (discoveryActive.get()) discovery.onNetworkChanged();
                        }
                        @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                            if (discoveryActive.get()) discovery.onNetworkChanged();
                        }
                    });
                } catch (RuntimeException ignored) {}
            }
            reconcileDiscoveryLifecycle();
            telemetryTh = new Thread(() -> {
                while (initialized.get()) {
                    try { Thread.sleep(1000); } catch (InterruptedException ignored) { break; }
                    if (multiroomRunning.get() || (uiVisible && senderTabActive)) {
                        tickMediaSync();
                        broadcastStateThrottled(true);
                    }
                }
            }, "SoundFlow-SenderTelemetry");
            telemetryTh.setDaemon(true);
            telemetryTh.start();
        } catch (Throwable ignored) {}
    }

    private ManagedDevice findEndpoint(String ip, int port) {
        for (ManagedDevice md : devices.values()) {
            if (md.info.ip.equals(ip) && md.info.cmdPort == port) return md;
        }
        return null;
    }

    private void onDiscoveryUpdate(MdnsDiscovery.DiscoveredDevice snapshot) {
        if (snapshot == null || MdnsDiscovery.isSelfLocalIp(snapshot.ip)) {
            return;
        }
        boolean changed;
        synchronized (this) {
            ManagedDevice md = null;
            for (ManagedDevice candidate : devices.values()) {
                if (candidate.discoveryKey.equals(snapshot.id)) {
                    md = candidate;
                    break;
                }
            }
            if (md == null) {
                if (!snapshot.online) return;
                md = findEndpoint(snapshot.ip, snapshot.cmdPort);
            }
            if (md == null) {
                md = new ManagedDevice();
                md.discoveryKey = snapshot.id;
                md.info = snapshot.copy();
                devices.put(snapshot.id, md);
                changed = true;
            } else {
                MdnsDiscovery.DiscoveredDevice old = md.info;
                if (md.discoveryKey.equals(snapshot.id) && snapshot.revision <= old.revision) return;
                md.discoveryKey = snapshot.id;
                MdnsDiscovery.DiscoveredDevice updated = snapshot.copy();
                updated.id = old.id;
                updated.isManual = old.isManual;
                if (old.isManual) {
                    updated.online = true;
                    if (!md.manualName.isEmpty()) updated.name = md.manualName;
                }
                md.info = updated;
                boolean endpointChanged = !old.ip.equals(updated.ip) || old.cmdPort != updated.cmdPort
                        || old.online != updated.online;
                if (endpointChanged) reconcileDeviceLocked(md);
                changed = endpointChanged || !old.name.equals(updated.name) || !old.mac.equals(updated.mac)
                        || !old.roomName.equals(updated.roomName) || old.deviceType != updated.deviceType;
            }
        }
        if (changed) broadcastStateThrottled(true);
    }

    private synchronized void reconcileStreams() {
        bindIp = explicitBindIp.isEmpty() ? MdnsDiscovery.pickWifiIpv4() : explicitBindIp;
        // Remove any previously discovered entry if its IP now matches our own interface
        devices.entrySet().removeIf(e -> MdnsDiscovery.isSelfLocalIp(e.getValue().info.ip));
        if (multiroomRunning.get()) {
            for (ManagedDevice md : devices.values()) reconcileDeviceLocked(md);
        }
        broadcastStateThrottled(true);
    }

    private void reconcileDeviceLocked(ManagedDevice md) {
        if (!md.inGroup || !multiroomRunning.get()) return;
        if (md.activeSessionId != 0 && (!md.info.online
                || !md.sessionLocalIp.equals(senderIpFor(md.info))
                || !md.sessionRemoteIp.equals(md.info.ip)
                || md.sessionRemotePort != md.info.cmdPort)) {
            detachDeviceLocked(md);
        }
        if (md.activeSessionId == 0 && md.info.online) attachDeviceLocked(md);
    }

    private String senderIpFor(MdnsDiscovery.DiscoveredDevice device) {
        if (!explicitBindIp.isEmpty()) {
            for (Inet4Address ip : MdnsDiscovery.ipv4Interfaces().keySet()) {
                if (explicitBindIp.equals(ip.getHostAddress())) return explicitBindIp;
            }
            return "";
        }
        try (DatagramSocket route = new DatagramSocket()) {
            route.connect(InetAddress.getByName(device.ip), device.cmdPort);
            InetAddress local = route.getLocalAddress();
            return local instanceof Inet4Address && !local.isAnyLocalAddress() ? local.getHostAddress() : "";
        } catch (Exception e) {
            return "";
        }
    }

    private static boolean validIPv4(String ip) {
        String[] parts = ip.split("\\.", -1);
        if (parts.length != 4) return false;
        for (String part : parts) {
            if (part.isEmpty() || part.length() > 3
                    || !part.chars().allMatch(c -> c >= '0' && c <= '9')
                    || Integer.parseInt(part) > 255) return false;
        }
        return !ip.equals("0.0.0.0");
    }

    private synchronized String addManualDevice(String name, String ip, int port) {
        ManagedDevice md = findEndpoint(ip, port);
        if (md == null) md = new ManagedDevice();
        if (!name.isEmpty()) md.manualName = name;
        MdnsDiscovery.DiscoveredDevice info = md.info == null ? new MdnsDiscovery.DiscoveredDevice() : md.info.copy();
        if (info.id == null) info.id = "manual_" + ip + "_" + port;
        if (!md.manualName.isEmpty()) info.name = md.manualName;
        else if (info.name == null) info.name = "MiPlay-" + ip;
        info.ip = ip;
        info.cmdPort = port;
        info.portSource = MdnsDiscovery.PortSource.MANUAL;
        info.isManual = true;
        info.online = true;
        md.info = info;
        devices.put(info.id, md);
        logSender("手动添加目标设备: " + info.name + " (" + ip + ":" + port + ")");
        return info.id;
    }

    public void onMediaProjectionGranted(MediaProjection mp) {
        audioEngine.setMediaProjection(mp);
        lastError = "";
        logSender("已获得系统音频内录授权 (已排除自身 UID 回环)");
        broadcastStateThrottled(true);
    }

    public void onMediaProjectionDenied() {
        audioEngine.revokeMediaProjection("未授予手机全局抓音权限，已切回校准测试音");
        lastError = audioEngine.getLastError();
        broadcastStateThrottled(true);
    }

    public void onMediaProjectionRevoked(String reason) {
        audioEngine.revokeMediaProjection(reason);
        lastError = reason != null ? reason : "";
        logSender("系统内录权限已结束: " + lastError);
        broadcastStateThrottled(true);
    }

    public void onWavFileSelected(Context ctx, Uri uri, String displayName) {
        String err = audioEngine.loadWavFromUri(ctx, uri, displayName);
        lastError = (err != null) ? err : "";
        if (err == null) {
            audioEngine.setMode(AudioCaptureEngine.MODE_WAV_FILE, displayName);
            lastSentMediaSig = "";
            logSender("已加载本地无损 WAV: " + displayName);
            broadcastMediaInfoAsync(buildEffectiveMediaInfo(), true);
        }
        broadcastStateThrottled(true);
    }

    public void onLrcFileSelected(Context ctx, Uri uri) {
        if (ctx == null || uri == null) return;
        try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
            if (in == null) {
                lastError = "无法打开 LRC 歌词文件";
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
                lastError = "LRC 歌词文件内容为空";
                broadcastStateThrottled(true);
                return;
            }
            audioEngine.setWavLrcText(text);
            synchronized (this) {
                extLyricsRaw = text.startsWith("\uFEFF") ? text.substring(1) : text;
                extLrcLines = AudioCaptureEngine.parseLrcLines(extLyricsRaw);
                extLyricLine = AudioCaptureEngine.lrcLineAt(extLrcLines, extPositionMs);
                lastSentMediaSig = "";
            }
            lastError = "";
            logSender("已绑定发送端同步 LRC 歌词 (" + extLrcLines.size() + " 行)");
            broadcastMediaInfoAsync(buildEffectiveMediaInfo(), false);
            broadcastStateThrottled(true);
        } catch (Exception e) {
            lastError = "导入 LRC 歌词失败: " + e.getMessage();
            broadcastStateThrottled(true);
        }
    }

    public void onNotificationAccessStateChanged() {
        broadcastStateThrottled(true);
    }

    public void onSystemMediaUpdate(String title, String artist, String album,
                                    long durationMs, long positionMs, boolean paused, boolean forceMetadata) {
        boolean metaChanged;
        synchronized (this) {
            boolean trackChanged = !extTitle.equals(title) || !extArtist.equals(artist) || !extAlbum.equals(album);
            extTitle = title == null ? "" : title;
            extArtist = artist == null ? "" : artist;
            extAlbum = album == null ? "" : album;
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
            metaChanged = trackChanged || forceMetadata;
        }
        if (multiroomRunning.get() && audioEngine.getMode() == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            CmdSession.MediaInfo info = buildEffectiveMediaInfo();
            if (metaChanged) {
                broadcastMediaInfoAsync(info, true);
            }
        }
        if (metaChanged) broadcastStateThrottled(false);
    }

    public void onExternalLyricLine(String lyricLine) {
        if (lyricLine == null || lyricLine.trim().isEmpty()) return;
        String clean = lyricLine.trim();
        synchronized (this) {
            if (clean.equals(extLyricLine)) return;
            extLyricLine = clean;
        }
        if (multiroomRunning.get() && audioEngine.getMode() == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            CmdSession.MediaInfo info = buildEffectiveMediaInfo();
            info.metaChangeType = 1;
            broadcastMediaInfoAsync(info, false);
        }
        broadcastStateThrottled(false);
    }

    private synchronized CmdSession.MediaInfo buildEffectiveMediaInfo() {
        CmdSession.MediaInfo info = new CmdSession.MediaInfo();
        info.sourceName = android.os.Build.MODEL != null && !android.os.Build.MODEL.isEmpty()
                ? android.os.Build.MODEL : "SoundFlow Android";
        int m = audioEngine.getMode();
        if (m == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK
                && (!extTitle.isEmpty() || !extLyricLine.isEmpty() || !extLyricsRaw.isEmpty())) {
            info.title = !extTitle.isEmpty() ? extTitle : audioEngine.getTrackTitle();
            info.artist = !extArtist.isEmpty() ? extArtist : audioEngine.getTrackArtist();
            info.album = !extAlbum.isEmpty() ? extAlbum : "SoundFlow Multi-Room";
            info.durationMs = extDurationMs;
            info.positionMs = extPositionMs;
            info.lyrics = extLyricsRaw;
            info.lyricLine = extLyricLine;
        } else if (m == AudioCaptureEngine.MODE_WAV_FILE) {
            info.title = audioEngine.getTrackTitle();
            info.artist = audioEngine.getTrackArtist();
            info.album = "Local WAV Lossless";
            info.durationMs = audioEngine.getWavDurationMs();
            info.positionMs = audioEngine.getWavPositionMs();
            info.lyrics = audioEngine.getWavLrcRaw();
            info.lyricLine = audioEngine.getWavLyricLine();
        } else {
            info.title = audioEngine.getTrackTitle();
            info.artist = audioEngine.getTrackArtist();
            info.album = "SoundFlow Multi-Room";
            info.durationMs = 0L;
            info.positionMs = 0L;
            info.lyrics = extLyricsRaw;
            info.lyricLine = extLyricLine;
        }
        return info;
    }

    private void broadcastMediaInfoAsync(CmdSession.MediaInfo info, boolean includePosition) {
        final List<CmdSession> sessions = new ArrayList<>();
        for (ManagedDevice md : devices.values()) {
            CmdSession cs = md.cmdSession;
            if (cs != null && cs.isConnected()) sessions.add(cs);
        }
        if (sessions.isEmpty()) return;
        new Thread(() -> {
            for (CmdSession cs : sessions) {
                cs.sendSetMediaInfo(info);
                if (includePosition && info.positionMs > 0L) {
                    cs.sendSetPositionMs(info.positionMs);
                }
            }
        }, "SoundFlow-MediaSync").start();
    }

    private void tickMediaSync() {
        if (audioEngine.getMode() == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK) {
            MediaSessionMonitorService.pollCurrentPosition(appContext);
        }
        if (!multiroomRunning.get()) return;
        CmdSession.MediaInfo info = buildEffectiveMediaInfo();
        String sig = info.title + "|" + info.artist + "|" + info.album + "|" + info.lyricLine + "|" + info.lyrics.length() + "|" + info.durationMs;
        boolean metaChanged = !sig.equals(lastSentMediaSig);
        if (metaChanged) {
            if (!lastSentMediaSig.isEmpty()) info.metaChangeType = 1;
            lastSentMediaSig = sig;
        }
        final boolean sendMeta = metaChanged;
        final boolean sendPos = info.durationMs > 0L || info.positionMs > 0L;
        if (!sendMeta && !sendPos) return;
        for (ManagedDevice md : devices.values()) {
            CmdSession cs = md.cmdSession;
            if (cs != null && cs.isConnected()) {
                if (sendMeta) cs.sendSetMediaInfo(info);
                if (sendPos) cs.sendSetPositionMs(info.positionMs);
            }
        }
    }

    private void handleRemoteControl(String devId, long sid, CmdSession.RemoteControlEvent ev) {
        if (ev == null) return;
        String devName = devId;
        synchronized (this) {
            ManagedDevice cur = devices.get(devId);
            if (cur == null || cur.activeSessionId != sid) return;
            devName = (cur.info != null && cur.info.name != null && !cur.info.name.isEmpty()) ? cur.info.name : devId;
            if (ev.action == CmdSession.RemoteControlAction.SET_VOLUME && ev.volume >= 0 && ev.volume <= 100) {
                cur.volume = ev.volume;
            }
        }
        if (ev.action == CmdSession.RemoteControlAction.SET_VOLUME) {
            broadcastStateThrottled(true);
            return;
        }

        boolean handled = false;
        int m = audioEngine.getMode();
        if (m == AudioCaptureEngine.MODE_WAV_FILE) {
            if (ev.action == CmdSession.RemoteControlAction.PAUSE) {
                audioEngine.setWavPaused(true);
                handled = true;
            } else if (ev.action == CmdSession.RemoteControlAction.RESUME) {
                audioEngine.setWavPaused(false);
                handled = true;
            } else if (ev.action == CmdSession.RemoteControlAction.SEEK) {
                audioEngine.seekWavMs(ev.positionMs);
                handled = true;
            }
            for (ManagedDevice md : devices.values()) {
                CmdSession cs = md.cmdSession;
                if (cs != null && cs.isConnected()) {
                    if (ev.action == CmdSession.RemoteControlAction.PAUSE) cs.sendPause();
                    else if (ev.action == CmdSession.RemoteControlAction.RESUME) cs.sendResume();
                    else if (ev.action == CmdSession.RemoteControlAction.SEEK) cs.sendSetPositionMs(ev.positionMs);
                }
            }
        } else {
            handled = MediaSessionMonitorService.dispatchControl(appContext, ev);
        }

        String actionStr;
        switch (ev.action) {
            case PAUSE: actionStr = "pause"; break;
            case RESUME: actionStr = "resume"; break;
            case NEXT: actionStr = "next"; break;
            case PREVIOUS: actionStr = "previous"; break;
            case SEEK: actionStr = "seek"; break;
            default: actionStr = "unknown"; break;
        }
        try {
            JSONObject evt = new JSONObject();
            evt.put("type", "remoteControl");
            evt.put("deviceId", devId);
            evt.put("deviceName", devName);
            evt.put("action", actionStr);
            evt.put("positionMs", ev.positionMs);
            evt.put("handledLocally", handled);
            emitEvent(evt.toString());
        } catch (Exception ignored) {}
        if (handled) broadcastStateThrottled(true);
    }

    public synchronized boolean startMultiroom(boolean lpcm, int sr, int delayMs) {
        if (multiroomRunning.get()) return true;
        int reqRate = (sr > 0) ? sr : 48000;
        if (lpcm) {
            if (reqRate != 48000 && reqRate != 44100) {
                lastError = "WFD LPCM 仅支持 48000 Hz 或 44100 Hz";
                broadcastStateThrottled(true);
                return false;
            }
        } else if (reqRate != 48000) {
            lastError = "WFD AAC-LC 仅支持 48000 Hz";
            broadcastStateThrottled(true);
            return false;
        }

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

        bindIp = explicitBindIp.isEmpty() ? MdnsDiscovery.pickWifiIpv4() : explicitBindIp;
        this.basePlayDelayMs = Math.max(100, Math.min(1000, delayMs));
        this.sessionOriginUs = TimerService.monotonicNowUs();
        final long audioSid = ++audioSessionId;

        if (!timerService.start()) {
            lastError = "TimerService UDP 校时服务启动失败";
            broadcastStateThrottled(true);
            return false;
        }

        TsAndRtpSender.AudioConfig actualCfg = multiSender.configure(lpcm, reqRate,
                () -> new Thread(() -> {
                    synchronized (MultiRoomManager.this) {
                        if (audioSessionId != audioSid || !multiroomRunning.get()) return;
                        lastError = "AAC 编码器持续失败，已停止推流，请重新启动全屋播放";
                        stopMultiroom();
                    }
                }, "SoundFlow-AacFailure").start());
        this.preferLpcm = actualCfg.useLpcm;
        this.sampleRate = actualCfg.sampleRate;
        this.lastError = "";

        multiroomRunning.set(true);
        reconcileDiscoveryLifecycle();
        for (ManagedDevice dev : devices.values()) {
            if (dev.inGroup) attachDeviceLocked(dev);
        }

        final int activeRate = actualCfg.sampleRate;
        audioEngine.start(activeRate, actualCfg.frameSamples, sessionOriginUs, new AudioCaptureEngine.Callbacks() {
            @Override
            public void onPcmFrame(byte[] stereoPcmLe, int samples, long samplePosition) {
                long pts90k = samplePosition * 90000L / activeRate;
                multiSender.broadcastPcmFrame(stereoPcmLe, pts90k, () -> basePlayDelayMs);
            }

            @Override
            public void onSpectrum(float[] bands16) {
                if (!uiVisible || !senderTabActive) return;
                StringBuilder sb = new StringBuilder("{\"type\":\"spectrum\",\"levels\":[");
                for (int i = 0; i < bands16.length; i++) {
                    if (i > 0) sb.append(',');
                    sb.append(String.format(Locale.US, "%.3f", bands16[i]));
                }
                sb.append("]}");
                emitEvent(sb.toString());
            }
        });

        logSender("全屋同步推流已启动 (" + (actualCfg.useLpcm ? "LPCM" : "AAC-LC") + " " + activeRate + "Hz, delay=" + basePlayDelayMs + "ms)");
        broadcastStateThrottled(true);
        return true;
    }

    public synchronized void stopMultiroom() {
        if (!multiroomRunning.getAndSet(false)) return;
        audioEngine.stop();
        for (ManagedDevice dev : devices.values()) {
            detachDeviceLocked(dev);
        }
        multiSender.clearAll();
        timerService.stop();
        logSender("全屋同步推流已停止");
        reconcileDiscoveryLifecycle();
        if (appContext != null) {
            try {
                appContext.startService(new Intent(appContext, SenderService.class)
                        .setAction(SenderService.ACTION_STOP));
            } catch (Throwable ignored) {}
        }
        broadcastStateThrottled(true);
    }

    private void attachDeviceLocked(ManagedDevice dev) {
        if (dev.activeSessionId != 0 || !dev.info.online) return;
        final String devId = dev.info.id;
        if (!dev.closing.isDone()) {
            if (!dev.attachPending) {
                dev.attachPending = true;
                dev.closing.whenCompleteAsync((unused, error) -> {
                    synchronized (MultiRoomManager.this) {
                        dev.attachPending = false;
                        if (devices.get(devId) == dev && dev.inGroup && multiroomRunning.get()) attachDeviceLocked(dev);
                    }
                    broadcastStateThrottled(true);
                });
            }
            return;
        }
        final String curBindIp = senderIpFor(dev.info);
        if (curBindIp.isEmpty()) {
            dev.statusText = explicitBindIp.isEmpty() ? "找不到到达设备的本地 IPv4 地址"
                    : "本地绑定地址不可用: " + explicitBindIp;
            return;
        }
        final long sid = nextSessionId.getAndIncrement();
        dev.activeSessionId = sid;
        dev.sessionLocalIp = curBindIp;
        dev.sessionRemoteIp = dev.info.ip;
        dev.sessionRemotePort = dev.info.cmdPort;
        dev.statusText = "正在启动 RTSP 与 8899 握手...";

        WfdRtspServer rtsp = new WfdRtspServer();
        boolean rtspOk = rtsp.start(devId, curBindIp, timerService.getPort(), sessionOriginUs, preferLpcm, sampleRate,
                new WfdRtspServer.Callbacks() {
                    @Override
                    public void onStreamReady(String id, String peerIp, ConnectionWriter writer, Runnable failure) {
                        synchronized (MultiRoomManager.this) {
                            ManagedDevice cur = devices.get(id);
                            if (cur == null || cur.activeSessionId != sid || !cur.inGroup || !multiroomRunning.get()) {
                                writer.abort();
                                return;
                            }
                            cur.rtspStreaming = true;
                            multiSender.addTarget(id, sid, peerIp, writer, cur.delayBiasMs, failure);
                        }
                        logSender("音箱反连 RTSP 音频流已就绪: " + id + " (" + peerIp + ")");
                        broadcastStateThrottled(true);
                    }

                    @Override
                    public void onStopMedia(String id) {
                        multiSender.stopTargetForTeardown(id, sid);
                    }

                    @Override
                    public void onLatencyReport(String id, int latencyMs, int bitrate, long pkts) {
                        synchronized (MultiRoomManager.this) {
                            ManagedDevice cur = devices.get(id);
                            if (cur != null && cur.activeSessionId == sid) {
                                cur.reportedLatencyMs = latencyMs;
                            }
                        }
                    }

                    @Override
                    public void onSessionClosed(String id) {
                        boolean changed = false;
                        synchronized (MultiRoomManager.this) {
                            ManagedDevice cur = devices.get(id);
                            if (cur != null && cur.activeSessionId == sid) {
                                detachDeviceLocked(cur);
                                cur.statusText = "RTSP 音频会话已断开";
                                changed = true;
                            }
                        }
                        multiSender.removeTarget(id, sid);
                        if (changed) broadcastStateThrottled(true);
                    }

                    @Override
                    public void onStatus(String id, String status) {
                        boolean changed = false;
                        synchronized (MultiRoomManager.this) {
                            ManagedDevice cur = devices.get(id);
                            if (cur != null && cur.activeSessionId == sid) {
                                cur.statusText = status;
                                changed = true;
                            }
                        }
                        if (changed) broadcastStateThrottled(false);
                    }
                });
        final int rtspPort = rtspOk ? rtsp.getListenPort() : 0;

        if (!rtspOk || rtspPort <= 0) {
            rtsp.stop();
            dev.activeSessionId = 0L;
            dev.cmdConnected = false;
            dev.rtspStreaming = false;
            dev.statusText = "RTSP 监听启动失败，未发送 8899 Open";
            return;
        }

        dev.rtspServer = rtsp;
        final CmdSession cs = new CmdSession(dev.info.ip, dev.info.cmdPort, curBindIp);
        dev.cmdSession = cs;

        new Thread(() -> {
            if (dev.activeSessionId != sid) return;
            boolean ok = cs.connect(new CmdSession.Callbacks() {
                @Override
                public void onVersion(String v) {
                    boolean changed = false;
                    synchronized (MultiRoomManager.this) {
                        ManagedDevice cur = devices.get(devId);
                        if (cur != null && cur.activeSessionId == sid) {
                            cur.remoteVersion = v;
                            changed = true;
                        }
                    }
                    if (changed) broadcastStateThrottled(false);
                }

                @Override
                public void onStateChanged(boolean conn) {
                    boolean changed = false;
                    synchronized (MultiRoomManager.this) {
                        ManagedDevice cur = devices.get(devId);
                        if (cur != null && cur.activeSessionId == sid) {
                            cur.cmdConnected = conn;
                            changed = true;
                        }
                    }
                    if (changed) broadcastStateThrottled(true);
                }

                @Override
                public void onStatus(String status) {
                    boolean changed = false;
                    synchronized (MultiRoomManager.this) {
                        ManagedDevice cur = devices.get(devId);
                        if (cur != null && cur.activeSessionId == sid) {
                            cur.statusText = status;
                            changed = true;
                        }
                    }
                    if (changed) broadcastStateThrottled(false);
                }

                @Override
                public void onRemoteControl(CmdSession.RemoteControlEvent ev) {
                    handleRemoteControl(devId, sid, ev);
                }
            }, () -> dev.activeSessionId != sid);
            boolean stillActive;
            int initVol = 65;
            synchronized (MultiRoomManager.this) {
                ManagedDevice cur = devices.get(devId);
                stillActive = (ok && multiroomRunning.get() && cur != null && cur.inGroup && cur.activeSessionId == sid);
                if (stillActive) initVol = cur.volume;
            }
            if (stillActive) {
                cs.sendOpenDevice(curBindIp, rtspPort);
                CmdSession.MediaInfo info = buildEffectiveMediaInfo();
                cs.sendSetMediaInfo(info);
                if (info.positionMs > 0L) cs.sendSetPositionMs(info.positionMs);
                cs.sendSetVolume(initVol);
            } else if (ok) {
                cs.closeGracefully();
            }
        }, "SoundFlow-Attach-" + devId + "-" + sid).start();
    }

    private void detachDeviceLocked(ManagedDevice dev) {
        final long oldSid = dev.activeSessionId;
        dev.activeSessionId = 0L;
        if (oldSid > 0) {
            multiSender.stopTargetForTeardown(dev.info.id, oldSid);
        } else {
            multiSender.removeTarget(dev.info.id);
        }
        final CmdSession oldCs = dev.cmdSession;
        dev.cmdSession = null;
        final WfdRtspServer oldRtsp = dev.rtspServer;
        dev.rtspServer = null;
        dev.cmdConnected = false;
        dev.rtspStreaming = false;
        dev.statusText = "待机";
        if (oldCs != null || oldRtsp != null) {
            dev.closing = dev.closing.handle((unused, error) -> null).thenRunAsync(() -> {
                if (oldRtsp != null) oldRtsp.stop();
                if (oldCs != null) oldCs.closeGracefully();
            });
        }
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
                    result = buildStateJson();
                    break;
                case "net.listInterfaces": {
                    JSONArray arr = new JSONArray();
                    for (Map.Entry<Inet4Address, NetworkInterface> entry : MdnsDiscovery.ipv4Interfaces().entrySet()) {
                        if (entry.getKey().isLoopbackAddress()) continue;
                        JSONObject iface = new JSONObject();
                        iface.put("name", entry.getValue().getName());
                        iface.put("ipv4", entry.getKey().getHostAddress());
                        iface.put("isLoopback", false);
                        arr.put(iface);
                    }
                    result = arr;
                    break;
                }
                case "net.setBindIp": {
                    String ip = req.optString("ip", "").trim();
                    if (!ip.isEmpty() && !validIPv4(ip)) {
                        error = "无效的本地 IPv4 地址";
                        break;
                    }
                    synchronized (this) {
                        explicitBindIp = ip;
                        reconcileStreams();
                        result = buildStateJson();
                    }
                    break;
                }
                case "discovery.refresh":
                    reconcileDiscoveryLifecycle();
                    discovery.triggerQuery(req.optBoolean("automatic", false));
                    break;
                case "device.addManual": {
                    String name = req.optString("name", "").trim();
                    String ip = req.optString("ip", "").trim();
                    int port = req.optInt("cmdPort", 8899);
                    if (!validIPv4(ip) || port < 1 || port > 65535) {
                        error = "无效的设备 IPv4 地址或控制端口";
                        break;
                    }
                    if (MdnsDiscovery.isSelfLocalIp(ip)) {
                        error = "不能将本机自身添加为投送目标，以免产生音频回环";
                        break;
                    }
                    result = addManualDevice(name, ip, port);
                    broadcastStateThrottled(true);
                    break;
                }
                case "device.remove": {
                    String devId = req.optString("deviceId", "");
                    synchronized (this) {
                        ManagedDevice md = devices.remove(devId);
                        if (md != null) {
                            md.inGroup = false;
                            detachDeviceLocked(md);
                            result = Boolean.TRUE;
                        } else {
                            result = Boolean.FALSE;
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "multiroom.addDevice": {
                    synchronized (this) {
                        ManagedDevice md = devices.get(req.optString("deviceId", ""));
                        if (md != null) {
                            md.inGroup = true;
                            if (multiroomRunning.get()) attachDeviceLocked(md);
                            result = Boolean.TRUE;
                        } else {
                            result = Boolean.FALSE;
                            error = "设备不存在";
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "multiroom.removeDevice": {
                    synchronized (this) {
                        ManagedDevice md = devices.get(req.optString("deviceId", ""));
                        if (md != null) {
                            md.inGroup = false;
                            detachDeviceLocked(md);
                            result = Boolean.TRUE;
                        } else {
                            result = Boolean.FALSE;
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "multiroom.start": {
                    boolean lpcm = "LPCM".equals(req.optString("codec", preferLpcm ? "LPCM" : "AAC_LC"));
                    int sr = req.optInt("sampleRate", sampleRate);
                    int delay = req.optInt("basePlayDelayMs", basePlayDelayMs);
                    boolean ok = startMultiroom(lpcm, sr, delay);
                    if (!ok) error = lastError.isEmpty() ? "启动全屋播放失败" : lastError;
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
                case "sync.setBaseDelay":
                    basePlayDelayMs = Math.max(100, Math.min(1000, req.optInt("delayMs", 350)));
                    broadcastStateThrottled(true);
                    break;
                case "sync.setDeviceBias": {
                    String devId = req.optString("deviceId", "");
                    int bias = Math.max(-300, Math.min(300, req.optInt("delayBiasMs", 0)));
                    ManagedDevice md = devices.get(devId);
                    if (md != null) {
                        md.delayBiasMs = bias;
                        multiSender.setDelayBias(devId, bias);
                        broadcastStateThrottled(true);
                    }
                    break;
                }
                case "volume.setDevice": {
                    ManagedDevice md = devices.get(req.optString("deviceId", ""));
                    int vol = Math.max(0, Math.min(100, req.optInt("volume", 65)));
                    if (md != null) {
                        md.volume = vol;
                        final CmdSession cs = md.cmdSession;
                        if (cs != null && cs.isConnected()) {
                            new Thread(() -> cs.sendSetVolume(vol), "SoundFlow-Vol").start();
                        }
                        broadcastStateThrottled(true);
                    }
                    break;
                }
                case "volume.setMaster": {
                    int vol = Math.max(0, Math.min(100, req.optInt("volume", 70)));
                    masterVolume = vol;
                    for (ManagedDevice md : devices.values()) {
                        if (!md.inGroup) continue;
                        md.volume = vol;
                        final CmdSession cs = md.cmdSession;
                        if (cs != null && cs.isConnected()) {
                            new Thread(() -> cs.sendSetVolume(vol), "SoundFlow-MVol").start();
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "source.setMode": {
                    int mode = req.optInt("mode", AudioCaptureEngine.MODE_SYSTEM_LOOPBACK);
                    String param = req.optString("param", "");
                    if (mode == AudioCaptureEngine.MODE_SYSTEM_LOOPBACK && !audioEngine.hasMediaProjection()) {
                        audioEngine.setMode(mode, param);
                        if (uiDelegate != null) uiDelegate.onRequestSystemAudioCapture();
                    } else {
                        audioEngine.setMode(mode, param);
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "app.listAudioApps": {
                    result = listInstalledAudioApps();
                    break;
                }
                case "source.setTargetApp": {
                    int uid = req.optInt("uid", -1);
                    String pkg = req.optString("packageName", "").trim();
                    String lbl = req.optString("label", "").trim();
                    if (uid <= 0 || pkg.isEmpty() || (appContext != null && pkg.equals(appContext.getPackageName()))) {
                        this.targetAppUid = -1;
                        this.targetAppPkg = "";
                        this.targetAppName = "";
                        audioEngine.setTargetAppUid(-1, "");
                        MediaSessionMonitorService.setPreferredPackage("");
                        logSender("已切换为全局媒体音频捕获");
                    } else {
                        Context ctx = appContext;
                        if (ctx == null || uid == android.os.Process.myUid()
                                || ctx.getPackageManager().getApplicationInfo(pkg, 0).uid != uid) {
                            throw new IllegalArgumentException("音源应用已变化，请刷新列表");
                        }
                        this.targetAppUid = uid;
                        this.targetAppPkg = pkg;
                        this.targetAppName = lbl.isEmpty() ? pkg : lbl;
                        audioEngine.setTargetAppUid(uid, this.targetAppName);
                        MediaSessionMonitorService.setPreferredPackage(pkg);
                        logSender("已锁定单应用独立抓音: " + this.targetAppName + " (uid=" + uid + ")");
                    }
                    lastSentMediaSig = "";
                    broadcastStateThrottled(true);
                    break;
                }
                case "wav.pause":
                    audioEngine.setWavPaused(true);
                    broadcastStateThrottled(true);
                    break;
                case "wav.resume":
                    audioEngine.setWavPaused(false);
                    broadcastStateThrottled(true);
                    break;
                case "wav.seek":
                    audioEngine.seekWavMs(Math.max(0L, req.optLong("positionMs", 0L)));
                    broadcastStateThrottled(true);
                    break;
                case "media.control": {
                    String act = req.optString("action", "");
                    if (audioEngine.getMode() == AudioCaptureEngine.MODE_WAV_FILE) {
                        if ("pause".equals(act)) audioEngine.setWavPaused(true);
                        else if ("resume".equals(act)) audioEngine.setWavPaused(false);
                        else if ("seek".equals(act)) audioEngine.seekWavMs(req.optLong("positionMs", 0L));
                    } else {
                        CmdSession.RemoteControlAction rca = null;
                        if ("pause".equals(act)) rca = CmdSession.RemoteControlAction.PAUSE;
                        else if ("resume".equals(act)) rca = CmdSession.RemoteControlAction.RESUME;
                        else if ("prev".equals(act)) rca = CmdSession.RemoteControlAction.PREVIOUS;
                        else if ("next".equals(act)) rca = CmdSession.RemoteControlAction.NEXT;
                        if (rca != null) {
                            CmdSession.RemoteControlEvent ev = new CmdSession.RemoteControlEvent(rca, req.optLong("positionMs", 0L), -1, "ui");
                            result = MediaSessionMonitorService.dispatchControl(appContext, ev);
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "lyrics.getCurrent": {
                    CmdSession.MediaInfo info = buildEffectiveMediaInfo();
                    JSONObject lrc = new JSONObject();
                    lrc.put("title", info.title);
                    lrc.put("artist", info.artist);
                    lrc.put("album", info.album);
                    lrc.put("durationMs", info.durationMs);
                    lrc.put("positionMs", info.positionMs);
                    lrc.put("lyricsRaw", info.lyrics == null ? "" : info.lyrics);
                    lrc.put("lyricLine", info.lyricLine == null ? "" : info.lyricLine);
                    lrc.put("codec", (preferLpcm ? "LPCM " : "AAC-LC ") + (sampleRate / 1000) + "kHz");
                    lrc.put("paused", audioEngine.getMode() == AudioCaptureEngine.MODE_WAV_FILE ? audioEngine.isWavPaused() : extPaused);
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
            return "{\"type\":\"rpc\",\"id\":\"0\",\"error\":\"bad_request\"}";
        }
    }

    public synchronized JSONObject buildStateJson() throws Exception {
        JSONObject root = new JSONObject();
        root.put("bindIp", bindIp);
        root.put("autoBind", explicitBindIp.isEmpty());
        root.put("discoveryRunning", discoveryActive.get());
        root.put("multiroomRunning", multiroomRunning.get());
        root.put("hasProjection", audioEngine.hasMediaProjection());
        root.put("timerServicePort", timerService.getPort());
        root.put("sessionTimeOffsetUs", sessionOriginUs);
        root.put("sampleRate", sampleRate);
        root.put("basePlayDelayMs", basePlayDelayMs);
        root.put("codec", preferLpcm ? "LPCM" : "AAC_LC");
        root.put("masterVolume", masterVolume);
        root.put("sourceMode", audioEngine.getMode());
        root.put("targetAppUid", targetAppUid);
        root.put("targetAppPkg", targetAppPkg);
        root.put("targetAppName", targetAppName);
        root.put("lastError", lastError);
        root.put("notificationAccessGranted", MediaSessionMonitorService.isNotificationAccessGranted(appContext));

        CmdSession.MediaInfo curMedia = buildEffectiveMediaInfo();
        JSONObject track = new JSONObject();
        track.put("title", curMedia.title);
        track.put("artist", curMedia.artist);
        track.put("album", curMedia.album);
        track.put("durationMs", curMedia.durationMs);
        track.put("positionMs", curMedia.positionMs);
        track.put("lyricLine", curMedia.lyricLine == null ? "" : curMedia.lyricLine);
        track.put("hasLyrics", (curMedia.lyrics != null && !curMedia.lyrics.isEmpty())
                || (curMedia.lyricLine != null && !curMedia.lyricLine.isEmpty()));
        track.put("paused", audioEngine.getMode() == AudioCaptureEngine.MODE_WAV_FILE ? audioEngine.isWavPaused() : extPaused);
        root.put("track", track);

        Map<String, TimerService.ClientTelemetry> tmap = timerService.snapshotTelemetry();
        JSONArray devArr = new JSONArray();
        List<ManagedDevice> list = new ArrayList<>(devices.values());
        for (ManagedDevice md : list) {
            JSONObject d = new JSONObject();
            d.put("id", md.info.id);
            d.put("name", md.info.name);
            d.put("ip", md.info.ip);
            d.put("cmdPort", md.info.cmdPort);
            d.put("mac", md.info.mac);
            d.put("deviceType", md.info.deviceType);
            d.put("roomName", md.info.roomName);
            d.put("isManual", md.info.isManual);
            d.put("online", md.info.online);
            d.put("inGroup", md.inGroup);
            d.put("cmdConnected", md.cmdConnected);
            d.put("rtspStreaming", md.rtspStreaming);
            d.put("volume", md.volume);
            d.put("delayBiasMs", md.delayBiasMs);
            d.put("reportedLatencyMs", md.reportedLatencyMs);
            TimerService.ClientTelemetry ct = tmap.get(md.info.ip);
            d.put("syncResidualUs", ct != null ? ct.residualErrorUs : 0L);
            d.put("syncPackets", ct != null ? ct.packetCount : 0L);
            d.put("audioPacketsSent", multiSender.getPacketsSent(md.info.id));
            d.put("remoteVersion", md.remoteVersion);
            d.put("statusText", md.statusText);
            devArr.put(d);
        }
        root.put("devices", devArr);
        return root;
    }

    private JSONArray listInstalledAudioApps() {
        JSONArray arr = new JSONArray();
        Context ctx = this.appContext;
        if (ctx == null) return arr;
        try {
            PackageManager pm = ctx.getPackageManager();
            String selfPkg = ctx.getPackageName();
            Intent mainIntent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            List<ResolveInfo> resolved = pm.queryIntentActivities(mainIntent, 0);
            java.util.Map<String, JSONObject> byPkg = new java.util.LinkedHashMap<>();
            if (resolved != null) {
                for (ResolveInfo ri : resolved) {
                    if (ri == null || ri.activityInfo == null || ri.activityInfo.applicationInfo == null) continue;
                    ApplicationInfo ai = ri.activityInfo.applicationInfo;
                    String pkg = ai.packageName;
                    if (pkg == null || pkg.equals(selfPkg) || byPkg.containsKey(pkg)) continue;
                    if (ai.uid < 10000) continue; // skip core system UIDs
                    CharSequence cs = ai.loadLabel(pm);
                    String label = (cs != null && cs.length() > 0) ? cs.toString().trim() : pkg;
                    JSONObject item = new JSONObject();
                    item.put("packageName", pkg);
                    item.put("label", label);
                    item.put("uid", ai.uid);
                    byPkg.put(pkg, item);
                }
            }
            List<JSONObject> sorted = new ArrayList<>(byPkg.values());
            java.text.Collator collator = java.text.Collator.getInstance(Locale.CHINA);
            sorted.sort((a, b) -> collator.compare(a.optString("label", ""), b.optString("label", "")));
            for (JSONObject item : sorted) arr.put(item);
        } catch (Throwable ignored) {}
        return arr;
    }

    public void broadcastStateThrottled(boolean force) {
        if (!uiVisible) return;
        long now = TimerService.monotonicNowUs();
        long prev = lastBroadcastUs.get();
        if (!force && (now - prev) < 250_000L) return;
        lastBroadcastUs.set(now);
        try {
            JSONObject evt = new JSONObject();
            evt.put("type", "state");
            evt.put("data", buildStateJson());
            emitEvent(evt.toString());
        } catch (Exception ignored) {}
    }
}
