package com.mishare.miplay;

import android.content.Context;
import android.media.projection.MediaProjection;
import android.net.Uri;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Android 端全屋妙播核心调度单例：
 * - SS-03: 全局唯一 sessionId 校验全部回调与音频目标注册/删除，杜绝旧会话覆盖或误删新目标
 * - SS-06: RTSP 监听启动失败或端口无效时绝不发送 8899 Open
 * - SS-07 & SS-16: 由 TsAndRtpSender.configure() 产出单一实际 AudioConfig 供 RTSP/TS/采集共用
 * - SS-09 & SS-11: WAV 加载失败保留原状态并回传错误；MediaProjection 撤销立即释放并同步 UI
 */
public class MultiRoomManager {
    public static class ManagedDevice {
        public MdnsDiscovery.DiscoveredDevice info;
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
    private final LocalWsServer wsServer = new LocalWsServer();
    private final ConcurrentHashMap<String, ManagedDevice> devices = new ConcurrentHashMap<>();

    private final AtomicBoolean startDispatched = new AtomicBoolean(false);
    private final AtomicBoolean initialized = new AtomicBoolean(false);
    private final CountDownLatch wsReadyLatch = new CountDownLatch(1);
    private final AtomicBoolean multiroomRunning = new AtomicBoolean(false);
    private final AtomicLong totalSamples = new AtomicLong(0);
    private final AtomicLong lastBroadcastUs = new AtomicLong(0);
    private final AtomicLong nextSessionId = new AtomicLong(1);

    private volatile String bindIp = "127.0.0.1";
    private volatile String wsToken = "";
    private volatile long sessionOriginUs = 0;
    private volatile int sampleRate = 48000;
    private volatile int basePlayDelayMs = 350;
    private volatile boolean preferLpcm = false;
    private volatile int masterVolume = 70;
    private volatile String lastError = "";
    private volatile UiActionDelegate uiDelegate;
    private Thread telemetryTh;

    public void setUiActionDelegate(UiActionDelegate delegate) {
        this.uiDelegate = delegate;
    }

    public void ensureStartedAsync() {
        if (!startDispatched.compareAndSet(false, true)) return;
        new Thread(this::initOnBackground, "MiPlay-CoreInit").start();
    }

    private void initOnBackground() {
        try {
            bindIp = MdnsDiscovery.pickWifiIpv4();
            wsToken = UUID.randomUUID().toString().replace("-", "");
            wsServer.start(wsToken, this::handleRpcRequest);
            wsReadyLatch.countDown();

            discovery.start(dev -> {
                if (dev.ip != null && dev.ip.equals(bindIp)) return;
                ManagedDevice md = devices.get(dev.id);
                boolean changed = false;
                if (md == null) {
                    md = new ManagedDevice();
                    md.info = dev;
                    devices.put(dev.id, md);
                    changed = true;
                } else {
                    if (!safeEq(md.info.ip, dev.ip) || md.info.cmdPort != dev.cmdPort) changed = true;
                    md.info.ip = dev.ip;
                    md.info.cmdPort = dev.cmdPort;
                    if (dev.name != null && !dev.name.isEmpty() && !dev.name.equals(md.info.name)) {
                        md.info.name = dev.name;
                        changed = true;
                    }
                    md.info.lastSeenUs = dev.lastSeenUs;
                }
                if (changed) broadcastStateThrottled(false);
            });

            initialized.set(true);
            telemetryTh = new Thread(() -> {
                while (initialized.get()) {
                    try { Thread.sleep(1000); } catch (InterruptedException ignored) { break; }
                    broadcastStateThrottled(true);
                }
            }, "MiPlay-Telemetry");
            telemetryTh.start();
        } catch (Throwable ignored) {
            wsReadyLatch.countDown();
        }
    }

    private static boolean safeEq(String a, String b) {
        return a == null ? b == null : a.equals(b);
    }

    public int awaitWsPort(long timeoutMs) {
        try { wsReadyLatch.await(timeoutMs, TimeUnit.MILLISECONDS); } catch (InterruptedException ignored) {}
        return wsServer.getPort();
    }

    public int getWsPort() { return wsServer.getPort(); }
    public String getWsToken() { return wsToken; }

    public void onMediaProjectionGranted(MediaProjection mp) {
        audioEngine.setMediaProjection(mp);
        lastError = "";
        broadcastStateThrottled(true);
    }

    public void onMediaProjectionDenied() {
        audioEngine.revokeMediaProjection("未授予手机全局抓音权限，已保持校准测试音");
        lastError = audioEngine.getLastError();
        broadcastStateThrottled(true);
    }

    public void onMediaProjectionRevoked(String reason) {
        audioEngine.revokeMediaProjection(reason);
        lastError = reason != null ? reason : "";
        broadcastStateThrottled(true);
    }

    public void onWavFileSelected(Context ctx, Uri uri, String displayName) {
        String err = audioEngine.loadWavFromUri(ctx, uri, displayName);
        lastError = (err != null) ? err : "";
        broadcastStateThrottled(true);
    }

    public synchronized boolean startMultiroom(boolean lpcm, int sr, int delayMs) {
        if (multiroomRunning.get()) return true;
        int reqRate = (sr > 0) ? sr : 48000;
        // SS-16: 校验采样率与编码组合，禁止向 RTSP 通告不匹配参数
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

        bindIp = MdnsDiscovery.pickWifiIpv4();
        this.basePlayDelayMs = Math.max(100, Math.min(1000, delayMs));
        this.sessionOriginUs = TimerService.monotonicNowUs();
        this.totalSamples.set(0);

        if (!timerService.start()) {
            lastError = "TimerService UDP 校时服务启动失败";
            broadcastStateThrottled(true);
            return false;
        }

        // SS-07: 先配置编码器并取回实际生效配置（如 AAC 初始化失败自动回退 LPCM），统一驱动 RTSP/TS/采集
        TsAndRtpSender.AudioConfig actualCfg = multiSender.configure(lpcm, reqRate);
        this.preferLpcm = actualCfg.useLpcm;
        this.sampleRate = actualCfg.sampleRate;
        this.lastError = "";

        multiroomRunning.set(true);
        for (ManagedDevice dev : devices.values()) {
            if (dev.inGroup) attachDeviceLocked(dev);
        }

        final int activeRate = actualCfg.sampleRate;
        audioEngine.start(activeRate, actualCfg.frameSamples, new AudioCaptureEngine.Callbacks() {
            @Override
            public void onPcmFrame(byte[] stereoPcmLe, int samples) {
                // 若因等待系统抓音授权、切源或后台调度阻塞导致样本计数落后实时墙钟 >3 帧，立即按整帧边界追齐
                long nowUs = TimerService.monotonicNowUs();
                long elapsedUs = Math.max(0L, nowUs - sessionOriginUs);
                long expectedSamples = (elapsedUs * activeRate) / 1_000_000L;
                long curSamples = totalSamples.get();
                long tolerance = (long) samples * 3L;
                if (expectedSamples > curSamples + tolerance) {
                    long aligned = (expectedSamples / samples) * samples;
                    if (totalSamples.compareAndSet(curSamples, aligned)) {
                        curSamples = aligned;
                    } else {
                        curSamples = totalSamples.get();
                    }
                }
                totalSamples.addAndGet(samples);
                long pts90k = ((curSamples * 90000L) / activeRate) + (basePlayDelayMs * 90L);
                multiSender.broadcastPcmFrame(stereoPcmLe, pts90k);
            }

            @Override
            public void onSpectrum(float[] bands16) {
                StringBuilder sb = new StringBuilder("{\"type\":\"spectrum\",\"levels\":[");
                for (int i = 0; i < bands16.length; i++) {
                    if (i > 0) sb.append(',');
                    sb.append(String.format(Locale.US, "%.3f", bands16[i]));
                }
                sb.append("]}");
                wsServer.broadcastText(sb.toString());
            }
        });

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
        broadcastStateThrottled(true);
    }

    private void attachDeviceLocked(ManagedDevice dev) {
        detachDeviceLocked(dev);
        final String devId = dev.info.id;
        final long sid = nextSessionId.getAndIncrement();
        dev.activeSessionId = sid;
        dev.statusText = "正在启动 RTSP 与 8899 握手...";

        WfdRtspServer rtsp = new WfdRtspServer();
        boolean rtspOk = rtsp.start(devId, bindIp, timerService.getPort(), sessionOriginUs, preferLpcm, sampleRate,
                new WfdRtspServer.Callbacks() {
                    @Override
                    public void onStreamReady(String id, String peerIp, java.net.Socket dataSock) {
                        synchronized (MultiRoomManager.this) {
                            ManagedDevice cur = devices.get(id);
                            if (cur == null || cur.activeSessionId != sid || !cur.inGroup || !multiroomRunning.get()) {
                                try { dataSock.close(); } catch (Exception ignored) {}
                                return;
                            }
                            cur.rtspStreaming = true;
                            multiSender.addTarget(id, sid, peerIp, dataSock, cur.delayBiasMs);
                        }
                        broadcastStateThrottled(true);
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
                                cur.rtspStreaming = false;
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

        // SS-06: RTSP 监听失败或端口无效时绝不继续建连或发 Open
        if (!rtspOk || rtspPort <= 0) {
            rtsp.stop();
            dev.activeSessionId = 0L;
            dev.cmdConnected = false;
            dev.rtspStreaming = false;
            dev.statusText = "RTSP 监听启动失败，未发送 8899 Open";
            return;
        }

        dev.rtspServer = rtsp;
        final String curBindIp = bindIp;
        final CmdSession cs = new CmdSession(dev.info.ip, dev.info.cmdPort);
        dev.cmdSession = cs;

        new Thread(() -> {
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
            });
            boolean stillActive;
            int initVol = 65;
            synchronized (MultiRoomManager.this) {
                ManagedDevice cur = devices.get(devId);
                stillActive = (ok && multiroomRunning.get() && cur != null && cur.inGroup && cur.activeSessionId == sid);
                if (stillActive) initVol = cur.volume;
            }
            if (stillActive) {
                cs.sendOpenDevice(curBindIp, rtspPort);
                cs.sendSetMediaInfo(audioEngine.getTrackTitle(), audioEngine.getTrackArtist(), "MiPlay MultiRoom", 0);
                cs.sendSetVolume(initVol);
            } else if (ok) {
                cs.closeGracefully();
            }
        }, "MiPlay-Attach-" + devId + "-" + sid).start();
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
            new Thread(() -> {
                if (oldRtsp != null) oldRtsp.stop();
                if (oldCs != null) oldCs.closeGracefully();
            }, "MiPlay-Detach-" + dev.info.id + "-" + oldSid).start();
        }
    }

    private String handleRpcRequest(String reqJson) {
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
                    JSONObject iface = new JSONObject();
                    iface.put("name", "wlan0");
                    iface.put("ipv4", bindIp);
                    iface.put("isLoopback", false);
                    arr.put(iface);
                    result = arr;
                    break;
                }
                case "source.listLoopback": {
                    JSONArray arr = new JSONArray();
                    JSONObject src = new JSONObject();
                    src.put("id", "android_playback_capture");
                    src.put("label", "Android 系统全局抓音 (AudioPlaybackCapture)");
                    arr.put(src);
                    result = arr;
                    break;
                }
                case "discovery.refresh":
                    new Thread(discovery::triggerQuery, "MiPlay-Refresh").start();
                    break;
                case "device.addManual": {
                    String name = req.optString("name", "");
                    String ip = req.optString("ip", "").trim();
                    int port = req.optInt("cmdPort", 8899);
                    String devId = "manual_" + ip + "_" + port;
                    ManagedDevice md = new ManagedDevice();
                    md.info = new MdnsDiscovery.DiscoveredDevice();
                    md.info.id = devId;
                    md.info.name = name.isEmpty() ? ("MiPlay-" + ip) : name;
                    md.info.ip = ip;
                    md.info.cmdPort = port;
                    md.info.isManual = true;
                    devices.put(devId, md);
                    broadcastStateThrottled(true);
                    result = devId;
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
                    boolean lpcm = "LPCM".equals(req.optString("codec", "AAC_LC"));
                    int sr = req.optInt("sampleRate", 48000);
                    int delay = req.optInt("basePlayDelayMs", 350);
                    boolean ok = startMultiroom(lpcm, sr, delay);
                    if (!ok) error = lastError.isEmpty() ? "启动全屋播放失败" : lastError;
                    result = ok;
                    break;
                }
                case "multiroom.stop":
                    stopMultiroom();
                    break;
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
                            new Thread(() -> cs.sendSetVolume(vol), "MiPlay-Vol").start();
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
                            new Thread(() -> cs.sendSetVolume(vol), "MiPlay-MVol").start();
                        }
                    }
                    broadcastStateThrottled(true);
                    break;
                }
                case "source.setMode": {
                    int mode = req.optInt("mode", 0);
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
                default:
                    error = "unknown method: " + method;
                    break;
            }

            JSONObject resp = new JSONObject();
            resp.put("type", "rpc");
            resp.put("id", id);
            if (error != null) {
                resp.put("error", error);
            } else {
                resp.put("result", result);
            }
            return resp.toString();
        } catch (Exception e) {
            return "{\"type\":\"rpc\",\"id\":\"0\",\"error\":\"bad_request\"}";
        }
    }

    private JSONObject buildStateJson() throws Exception {
        JSONObject root = new JSONObject();
        root.put("bindIp", bindIp);
        root.put("discoveryRunning", true);
        root.put("multiroomRunning", multiroomRunning.get());
        root.put("loopbackSupported", true);
        root.put("timerServicePort", timerService.getPort());
        root.put("sessionTimeOffsetUs", sessionOriginUs);
        root.put("sampleRate", sampleRate);
        root.put("basePlayDelayMs", basePlayDelayMs);
        root.put("codec", preferLpcm ? "LPCM" : "AAC_LC");
        root.put("masterVolume", masterVolume);
        root.put("sourceMode", audioEngine.getMode());
        root.put("sourceParam", "");
        root.put("lastError", lastError);

        JSONObject track = new JSONObject();
        track.put("title", audioEngine.getTrackTitle());
        track.put("artist", audioEngine.getTrackArtist());
        track.put("album", "MiPlay Multi-Room");
        track.put("durationMs", 0);
        track.put("positionMs", 0);
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

    private void broadcastStateThrottled(boolean force) {
        long now = TimerService.monotonicNowUs();
        long prev = lastBroadcastUs.get();
        if (!force && (now - prev) < 250_000L) return;
        lastBroadcastUs.set(now);
        try {
            JSONObject evt = new JSONObject();
            evt.put("type", "state");
            evt.put("data", buildStateJson());
            wsServer.broadcastText(evt.toString());
        } catch (Exception ignored) {}
    }
}
