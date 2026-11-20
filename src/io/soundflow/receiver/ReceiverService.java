package io.soundflow.receiver;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.Inet4Address;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.UUID;

/**
 * Isolated background process (:receiver) hosting Xiaomi MiPlay clean-room
 * and standard DLNA/UPnP audio renderers, MediaSession, and anti-kill guards.
 */
public final class ReceiverService extends Service {
    public static final String TAG = "SoundFlowSvc";
    public static final String PREF = "soundflow_cfg";
    private static final String PREF_AUDIO_SYNC_OFFSET = "audio_sync_offset_ms";
    public static final String ACTION_START = "io.soundflow.receiver.ACTION_START_RECEIVER";
    public static final String ACTION_STOP = "io.soundflow.receiver.ACTION_STOP_RECEIVER";
    public static final String ACTION_PAUSE = "io.soundflow.receiver.ACTION_PAUSE";
    public static final String ACTION_RESUME = "io.soundflow.receiver.ACTION_RESUME";
    public static final String ACTION_PREV = "io.soundflow.receiver.ACTION_PREV";
    public static final String ACTION_NEXT = "io.soundflow.receiver.ACTION_NEXT";
    public static final String ACTION_VOLUME = "io.soundflow.receiver.ACTION_VOLUME";
    public static final String ACTION_AUDIO_SYNC_OFFSET = "io.soundflow.receiver.ACTION_AUDIO_SYNC_OFFSET";
    public static final String ACTION_UPDATE_CONFIG = "io.soundflow.receiver.ACTION_UPDATE_CONFIG";

    public static final int MSG_REGISTER = 1;
    public static final int MSG_UNREGISTER = 2;
    public static final int MSG_SNAPSHOT = 3;
    public static final int MSG_MEDIA_EVENT = 4;
    public static final int MSG_AUDIO_LEVEL = 5;
    public static final int MSG_LOG_TEXT = 6;
    public static final int MSG_FETCH_LOGS = 7;
    public static final int MSG_UI_VISIBILITY = 8;
    public static final int MSG_AUDIO_ONSETS = 9;

    private static final int NOTICE_ID = 2026;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Messenger> clients = new ArrayList<>();
    private final Messenger incomingMessenger = new Messenger(new IncomingHandler());

    private HandlerThread workerThread;
    private Handler work;
    private HandlerThread analysisThread;
    private Handler analysis;
    private boolean onsetBusy;
    private long lastMeterAt;

    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback networkCallback;
    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;
    private AudioManager audio;
    private AudioFocusRequest focusRequest;
    private BroadcastReceiver volumeReceiver;
    private MediaSession mediaSession;
    private DlnaRendererServer dlnaServer;
    private File logFile;

    private boolean miplayRunning = false;
    private boolean dlnaRunning = false;
    private boolean playing = false;
    private boolean sessionActive = false;
    private boolean desired = false;
    private boolean destroyed = false;
    private boolean focusHeld = false;
    private boolean uiVisible = true;
    private float lastSentPeak = -1f;
    private int lastSentSpectrum = -1;
    private long lastSentPosMs = -1L;
    private volatile boolean miplayEngineStarted = false;
    private int lastVolumeIndex = -1;
    private int currentVolumePercent = 45;
    private long generation = 0;
    private long lastNoticeTime = 0;
    private long lastWakeRenew = 0;

    private String boundEndpoint = "";
    private String activeProtocol = "xiaomi";
    private String statusText = "接收服务未启动";
    private String endpointText = "";
    private String formatText = "";
    private String sourceName = "";
    private String lastError = "";

    // Latest media/lyric snapshot
    private String mediaTitle = "";
    private String mediaArtist = "";
    private String mediaAlbum = "";
    private String mediaLyricsLrc = "";
    private String mediaLyricLine = "";
    private String mediaArtwork = "";
    private long mediaDurationMs = 0;
    private long mediaPositionMs = 0;
    private long lastMediaSeq = 0;

    private final class IncomingHandler extends Handler {
        IncomingHandler() {
            super(Looper.getMainLooper());
        }

        @Override
        public void handleMessage(Message msg) {
            if (msg.what == MSG_REGISTER && msg.replyTo != null) {
                if (!clients.contains(msg.replyTo)) clients.add(msg.replyTo);
                sendSnapshotTo(msg.replyTo);
                startHighFreqMeterIfNeeded();
            } else if (msg.what == MSG_UNREGISTER && msg.replyTo != null) {
                clients.remove(msg.replyTo);
            } else if (msg.what == MSG_UI_VISIBILITY) {
                uiVisible = msg.arg1 != 0;
                if (!uiVisible && dlnaServer != null) dlnaServer.releaseVisualizer();
                if (uiVisible) startHighFreqMeterIfNeeded();
            } else if (msg.what == MSG_FETCH_LOGS && msg.replyTo != null) {
                Message reply = Message.obtain(null, MSG_LOG_TEXT);
                Bundle b = new Bundle();
                b.putString("logs", readLogTail());
                reply.setData(b);
                try { msg.replyTo.send(reply); } catch (RemoteException ignored) {}
            }
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        workerThread = new HandlerThread("soundflow-control");
        workerThread.start();
        work = new Handler(workerThread.getLooper());
        analysisThread = new HandlerThread("soundflow-onset");
        analysisThread.start();
        analysis = new Handler(analysisThread.getLooper());
        audio = (AudioManager) getSystemService(AUDIO_SERVICE);
        connectivity = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        logFile = new File(getFilesDir(), "receiver.log");
        NativeBridge.setAudioSyncOffset(getSharedPreferences(PREF, MODE_PRIVATE)
                .getInt(PREF_AUDIO_SYNC_OFFSET, 0));

        initMediaSession();
        initVolumeReceiver();
        initNetworkCallback();
        main.post(eventPoller);
    }

    private void initMediaSession() {
        mediaSession = new MediaSession(this, "SoundFlowAudioReceiver");
        mediaSession.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { dispatchAction("resume", 0); }
            @Override public void onPause() { dispatchAction("pause", 0); }
            @Override public void onSkipToPrevious() { dispatchAction("prev", 0); }
            @Override public void onSkipToNext() { dispatchAction("next", 0); }
            @Override public void onStop() { stopReceiver(); }
        }, main);
        AudioAttributes attrs = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();
        mediaSession.setPlaybackToLocal(attrs);
        focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setOnAudioFocusChangeListener(change -> {
                    if (change == AudioManager.AUDIOFOCUS_GAIN) {
                        focusHeld = true;
                        dispatchAction("output_resume", 0);
                    } else {
                        focusHeld = false;
                        dispatchAction("suspend", 0);
                        if (change == AudioManager.AUDIOFOCUS_LOSS) dispatchAction("pause", 0);
                    }
                }, main)
                .build();
    }

    private void initVolumeReceiver() {
        volumeReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                main.removeCallbacks(volumeDebouncer);
                main.postDelayed(volumeDebouncer, 90);
            }
        };
        IntentFilter filter = new IntentFilter("android.media.VOLUME_CHANGED_ACTION");
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(volumeReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(volumeReceiver, filter);
        }
        updateSystemVolume(false);
    }

    private void initNetworkCallback() {
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) { scheduleReconnect(); }
            @Override public void onLost(Network n) { scheduleReconnect(); }
            @Override public void onLinkPropertiesChanged(Network n, LinkProperties lp) { scheduleReconnect(); }
        };
        try {
            connectivity.registerNetworkCallback(new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                    .build(), networkCallback);
        } catch (Throwable t) {
            record("network", "registerNetworkCallback failed: " + t.getMessage());
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        if (action == null) action = ACTION_START;
        SharedPreferences prefs = getSharedPreferences(PREF, MODE_PRIVATE);

        if (ACTION_STOP.equals(action)) {
            stopReceiver();
            return START_NOT_STICKY;
        }
        if (ACTION_PAUSE.equals(action)) {
            dispatchAction("pause", 0);
            return START_STICKY;
        }
        if (ACTION_RESUME.equals(action)) {
            dispatchAction("resume", 0);
            return START_STICKY;
        }
        if (ACTION_PREV.equals(action)) {
            dispatchAction("prev", 0);
            return START_STICKY;
        }
        if (ACTION_NEXT.equals(action)) {
            dispatchAction("next", 0);
            return START_STICKY;
        }
        if (ACTION_VOLUME.equals(action)) {
            applySystemVolume(intent.getIntExtra("value", 45));
            return START_STICKY;
        }
        if (ACTION_AUDIO_SYNC_OFFSET.equals(action)) {
            int offset = Math.max(-400, Math.min(400, intent.getIntExtra("value", 0)));
            prefs.edit().putInt(PREF_AUDIO_SYNC_OFFSET, offset).apply();
            NativeBridge.setAudioSyncOffset(offset);
            broadcastSnapshot();
            return desired ? START_STICKY : START_NOT_STICKY;
        }
        if (ACTION_UPDATE_CONFIG.equals(action) && intent != null) {
            boolean networkChanged = applyConfigExtras(prefs, intent);
            if (desired && networkChanged) {
                boundEndpoint = "";
                reconnect();
            } else {
                broadcastSnapshot();
            }
            return START_STICKY;
        }

        if (applyConfigExtras(prefs, intent) && desired) {
            boundEndpoint = "";
        }
        desired = true;
        prefs.edit().putBoolean("enabled", true).apply();
        KeepAliveJobService.schedule(this);
        promoteForeground(false);
        reconnect();
        return START_STICKY;
    }

    private boolean applyConfigExtras(SharedPreferences prefs, Intent intent) {
        if (intent == null) return false;
        String prevName = prefs.getString("name", "SoundFlow-Pad");
        int prevType = prefs.getInt("device_type", 4);
        boolean prevDlna = prefs.getBoolean("enable_dlna", true);

        boolean networkChanged = false;
        SharedPreferences.Editor ed = prefs.edit();
        if (intent.hasExtra("name")) {
            String nextName = intent.getStringExtra("name");
            if (nextName != null && !nextName.equals(prevName)) networkChanged = true;
            ed.putString("name", nextName);
        }
        if (intent.hasExtra("device_type")) {
            int nextType = intent.getIntExtra("device_type", 4);
            if (nextType != prevType) networkChanged = true;
            ed.putInt("device_type", nextType);
        }
        if (intent.hasExtra("enable_dlna")) {
            boolean nextDlna = intent.getBooleanExtra("enable_dlna", true);
            if (nextDlna != prevDlna) networkChanged = true;
            ed.putBoolean("enable_dlna", nextDlna);
        }
        if (intent.hasExtra("auto_online_lyrics")) {
            ed.putBoolean("auto_online_lyrics", intent.getBooleanExtra("auto_online_lyrics", false));
        }
        ed.apply();
        return networkChanged;
    }

    private void promoteForeground(boolean isPlaybackActive) {
        int type = isPlaybackActive
                ? (ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
                : ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
        try {
            startForeground(NOTICE_ID, buildNotification(), type);
        } catch (Throwable ex) {
            try {
                startForeground(NOTICE_ID, buildNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } catch (Throwable ignored) {}
            record("foreground", "promoteForeground(" + isPlaybackActive + "): " + ex.getMessage());
        }
    }

    private void scheduleReconnect() {
        main.post(() -> {
            main.removeCallbacks(reconnectTask);
            main.postDelayed(reconnectTask, 1200);
        });
    }

    private final Runnable reconnectTask = () -> {
        if (desired) reconnect();
    };

    private final Runnable volumeDebouncer = () -> updateSystemVolume(true);

    private int systemVolumePercent() {
        if (audio == null) return currentVolumePercent;
        int max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
        return max <= 0 ? 0 : Math.round(100f * audio.getStreamVolume(AudioManager.STREAM_MUSIC) / max);
    }

    private void updateSystemVolume(boolean publishToRemote) {
        if (audio == null) return;
        int index = audio.getStreamVolume(AudioManager.STREAM_MUSIC);
        int percent = systemVolumePercent();
        boolean changed = index != lastVolumeIndex;
        lastVolumeIndex = index;
        currentVolumePercent = percent;
        getSharedPreferences(PREF, MODE_PRIVATE).edit().putInt("volume", percent).apply();
        if (dlnaServer != null) dlnaServer.setVolumeFromSystem(percent);
        if (publishToRemote && changed && miplayRunning) {
            dispatchAction("volume", percent);
        }
        broadcastSnapshot();
    }

    private void applySystemVolume(int percent) {
        if (audio == null) return;
        int clamped = Math.max(0, Math.min(100, percent));
        int max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
        int targetIndex = Math.round(clamped * max / 100f);
        if (audio.getStreamVolume(AudioManager.STREAM_MUSIC) != targetIndex) {
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, targetIndex, 0);
        }
        updateSystemVolume(true);
    }

    private String[] findLanInterface() {
        if (connectivity == null) return null;
        for (Network net : connectivity.getAllNetworks()) {
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(net);
            LinkProperties lp = connectivity.getLinkProperties(net);
            if (caps == null || lp == null) continue;
            if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                    && !caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
                continue;
            }
            for (LinkAddress addr : lp.getLinkAddresses()) {
                if (addr.getAddress() instanceof Inet4Address
                        && !addr.getAddress().isLoopbackAddress()
                        && !addr.getAddress().isLinkLocalAddress()) {
                    return new String[]{addr.getAddress().getHostAddress(), lp.getInterfaceName()};
                }
            }
        }
        return null;
    }

    private void reconnect() {
        if (!desired || destroyed) return;
        SharedPreferences prefs = getSharedPreferences(PREF, MODE_PRIVATE);
        String[] lan = findLanInterface();
        String next = lan == null ? "" : (lan[0] + "/" + lan[1]);
        boolean wantDlna = prefs.getBoolean("enable_dlna", true);
        boolean healthy = miplayRunning && (!wantDlna || dlnaRunning);
        if (healthy && next.equals(boundEndpoint)) return;

        long ticket = ++generation;
        miplayRunning = false;
        dlnaRunning = false;
        playing = false;
        sessionActive = false;
        boundEndpoint = next;
        endpointText = next;
        formatText = "";
        sourceName = "";
        lastError = "";
        statusText = lan == null ? "等待接入 Wi-Fi 或局域网" : "正在启动独立接收引擎…";
        releasePlaybackLocks();
        updateNotification(true);
        broadcastSnapshot();

        final String name = prefs.getString("name", "SoundFlow-Pad");
        final int devType = prefs.getInt("device_type", 4);
        final boolean enableDlna = wantDlna;
        final int vol = systemVolumePercent();

        work.post(() -> {
            if (ticket != generation || !desired || destroyed) return;
            boolean waitRustClose = miplayEngineStarted;
            NativeBridge.stop();
            miplayEngineStarted = false;
            if (dlnaServer != null) {
                dlnaServer.stop();
                dlnaServer = null;
            }
            if (lan == null) {
                main.post(() -> {
                    if (ticket == generation) releaseNetworkLocks();
                });
                return;
            }
            if (waitRustClose) {
                try { Thread.sleep(90); } catch (InterruptedException ignored) {}
            }
            if (ticket != generation || !desired || destroyed) return;

            acquireNetworkLocks();
            String uuid = prefs.getString("uuid", null);
            String lyra = prefs.getString("lyra", null);
            String mediaId = prefs.getString("media", null);
            if (uuid == null || lyra == null || mediaId == null) {
                uuid = UUID.randomUUID().toString();
                lyra = UUID.randomUUID().toString().replace("-", "").substring(0, 8).toUpperCase(Locale.ROOT);
                mediaId = UUID.randomUUID().toString().replace("-", "").substring(0, 8).toUpperCase(Locale.ROOT);
                prefs.edit().putString("uuid", uuid).putString("lyra", lyra).putString("media", mediaId).apply();
            }

            String miplayErr = "";
            String cfgJson = "";
            try {
                cfgJson = new JSONObject()
                        .put("name", name)
                        .put("uuid", uuid)
                        .put("lyra", lyra)
                        .put("media", mediaId)
                        .put("ip", lan[0])
                        .put("interface", lan[1])
                        .put("device_type", devType)
                        .put("volume", vol)
                        .toString();
            } catch (Throwable ex) {
                miplayErr = ex.toString();
            }

            if (miplayErr.isEmpty()) {
                for (int attempt = 0; attempt < 6; attempt++) {
                    if (ticket != generation || !desired || destroyed) return;
                    try {
                        miplayErr = NativeBridge.start(cfgJson);
                        if (miplayErr == null) miplayErr = "NativeBridge 未返回结果";
                    } catch (Throwable ex) {
                        miplayErr = ex.toString();
                    }
                    if (miplayErr.isEmpty()) {
                        miplayEngineStarted = true;
                        break;
                    }
                    if (!miplayErr.contains("8899/55982") || attempt == 5) break;
                    try { Thread.sleep(120); } catch (InterruptedException ignored) {}
                }
            }

            if (ticket != generation || !desired || destroyed) {
                if (miplayEngineStarted) {
                    NativeBridge.stop();
                    miplayEngineStarted = false;
                }
                return;
            }

            boolean dlnaOk = false;
            if (enableDlna) {
                DlnaRendererServer srv = new DlnaRendererServer(
                        ReceiverService.this,
                        uuid,
                        name,
                        lan[0],
                        lan[1],
                        vol,
                        dlnaListener
                );
                dlnaOk = srv.start();
                if (ticket != generation || !desired || destroyed) {
                    if (dlnaOk) srv.stop();
                    if (miplayEngineStarted) {
                        NativeBridge.stop();
                        miplayEngineStarted = false;
                    }
                    return;
                }
                dlnaServer = dlnaOk ? srv : null;
            }

            final String finalErr = miplayErr;
            final boolean finalDlna = dlnaOk;
            main.post(() -> {
                if (ticket != generation || !desired || destroyed) return;
                miplayRunning = finalErr.isEmpty();
                dlnaRunning = finalDlna;
                if (miplayRunning && dlnaRunning) {
                    statusText = "双模就绪 · 等待小米妙播 / DLNA 投送";
                } else if (miplayRunning) {
                    statusText = "小米妙播就绪 · 等待投送";
                } else if (dlnaRunning) {
                    statusText = "DLNA 就绪（小米端口被占用）";
                } else {
                    statusText = "启动失败";
                    boundEndpoint = "";
                    releaseNetworkLocks();
                }
                lastError = finalErr;
                record("lifecycle", "miplay=" + miplayRunning + " dlna=" + dlnaRunning + (finalErr.isEmpty() ? "" : " err=" + finalErr));
                updateNotification(true);
                broadcastSnapshot();
            });
        });
    }

    private final DlnaRendererServer.Listener dlnaListener = new DlnaRendererServer.Listener() {
        @Override public void onDlnaPreparing(String title, String artist, String album) {
            main.post(() -> {
                if (destroyed || !desired) return;
                // SetURI is an explicit choice of transport. Its progress and
                // state own this session until Stop, not concurrent MiPlay polls.
                boolean changedTrack = !mediaTitle.equals(title == null ? "" : title)
                        || !mediaArtist.equals(artist == null ? "" : artist);
                activeProtocol = "dlna";
                sessionActive = true;
                if (changedTrack) { mediaLyricsLrc = ""; mediaLyricLine = ""; mediaArtwork = ""; }
                mediaTitle = title != null ? title : "";
                mediaArtist = artist != null ? artist : "";
                mediaAlbum = album != null ? album : "";
                if (work != null) work.post(() -> NativeBridge.action("suspend", 0));
            });
        }

        @Override
        public void onDlnaState(boolean isPlaying, String transportState, String title, String artist, String album, long durationMs, long positionMs) {
            main.post(() -> {
                if (destroyed || !desired || !"dlna".equals(activeProtocol)) return;
                activeProtocol = "dlna";
                playing = isPlaying;
                sessionActive = isPlaying || "PAUSED_PLAYBACK".equals(transportState);
                mediaTitle = title != null ? title : "";
                mediaArtist = artist != null ? artist : "";
                mediaAlbum = album != null ? album : "";
                mediaDurationMs = durationMs;
                mediaPositionMs = positionMs;
                lastMediaSeq++;
                formatText = "DLNA Audio · 48 kHz";
                sourceName = "DLNA / UPnP 发送端";
                statusText = isPlaying ? "正在播放 DLNA 投送" : (sessionActive ? "DLNA 已暂停" : idleStatus());
                if (isPlaying) acquirePlaybackLocks();
                else if (!sessionActive) releasePlaybackLocks();
                updateMediaSessionState();
                broadcastMediaEvent();
                broadcastSnapshot();
                updateNotification(false);
            });
        }

        @Override public void onDlnaVolume(int percent) { main.post(() -> applySystemVolume(percent)); }
        @Override public void onDlnaError(String msg) { main.post(() -> { lastError = msg; broadcastSnapshot(); }); }
        @Override public void onDlnaLog(String msg) { record("dlna", msg); }
    };

    private String idleStatus() {
        if (miplayRunning && dlnaRunning) return "双模就绪 · 等待小米妙播 / DLNA 投送";
        if (miplayRunning) return "小米妙播就绪 · 等待投送";
        if (dlnaRunning) return "DLNA 就绪 · 等待投送";
        return "接收服务未启动";
    }

    private synchronized void acquireNetworkLocks() {
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        if (wifi != null) {
            if (multicastLock == null) {
                multicastLock = wifi.createMulticastLock("SoundFlow:multicast");
                multicastLock.setReferenceCounted(false);
            }
            if (!multicastLock.isHeld()) multicastLock.acquire();

            if (wifiLock == null) {
                wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "SoundFlow:lowlatency");
                wifiLock.setReferenceCounted(false);
            }
            if (!wifiLock.isHeld()) wifiLock.acquire();
        }
    }

    private synchronized void releaseNetworkLocks() {
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
    }

    private void acquirePlaybackLocks() {
        if (!focusHeld && audio != null) {
            focusHeld = audio.requestAudioFocus(focusRequest) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            dispatchAction(focusHeld ? "output_resume" : "suspend", 0);
            if (!focusHeld) lastError = "等待系统音频焦点";
        }
        if (wakeLock == null) {
            PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
            if (pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SoundFlow:playback");
                wakeLock.setReferenceCounted(false);
            }
        }
        if (wakeLock != null) wakeLock.acquire(10 * 60 * 1000L);
        lastWakeRenew = SystemClock.elapsedRealtime();
        if (mediaSession != null) mediaSession.setActive(true);
        promoteForeground(true);
    }

    private void releasePlaybackLocks() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        if (focusHeld && audio != null) {
            audio.abandonAudioFocusRequest(focusRequest);
            focusHeld = false;
        }
        if (mediaSession != null) mediaSession.setActive(false);
        if (desired && !destroyed) promoteForeground(false);
    }

    private final Runnable eventPoller = new Runnable() {
        @Override
        public void run() {
            if (destroyed) return;
            try {
                String raw = NativeBridge.poll();
                if (raw != null && !"[]".equals(raw)) {
                    JSONArray events = new JSONArray(raw);
                    JSONObject lastVol = null;
                    for (int i = 0; i < events.length(); i++) {
                        JSONObject e = events.getJSONObject(i);
                        if ("volume".equals(e.optString("event"))) lastVol = e;
                        else handleNativeEvent(e);
                    }
                    if (lastVol != null) handleNativeEvent(lastVol);
                }
                if (playing && wakeLock != null && wakeLock.isHeld()
                        && SystemClock.elapsedRealtime() - lastWakeRenew > 5 * 60 * 1000L) {
                    wakeLock.acquire(10 * 60 * 1000L);
                    lastWakeRenew = SystemClock.elapsedRealtime();
                }
            } catch (Throwable ex) {
                Log.e(TAG, "poll error", ex);
            }
            main.postDelayed(this, sessionActive ? 90 : 500);
        }
    };

    private boolean highFreqRunning = false;

    private void startHighFreqMeterIfNeeded() {
        if (highFreqRunning || destroyed) return;
        highFreqRunning = true;
        main.post(audioLevelTicker);
    }

    private final Runnable audioLevelTicker = new Runnable() {
        @Override
        public void run() {
            if (destroyed || clients.isEmpty()) {
                highFreqRunning = false;
                return;
            }
            if (playing && uiVisible) {
                if (!onsetBusy) {
                    onsetBusy = true;
                    boolean xiaomi = "xiaomi".equals(activeProtocol);
                    String protocol = activeProtocol;
                    float[] dlnaSpectrum = !xiaomi && dlnaServer != null ? dlnaServer.captureOnsetSpectrum() : null;
                    int dlnaRate = dlnaServer != null ? dlnaServer.onsetSampleRate : 48000;
                    analysis.post(() -> {
                        String events = NativeBridge.audioOnsets(xiaomi);
                        if (!xiaomi) events = dlnaSpectrum == null ? "[]" : NativeBridge.spectrumOnsets(dlnaSpectrum, dlnaRate);
                        String payload = events;
                        long capturedAt = SystemClock.uptimeMillis();
                        main.post(() -> {
                            onsetBusy = false;
                            if (!destroyed && playing && uiVisible && protocol.equals(activeProtocol) && !"[]".equals(payload)) {
                                Message event = Message.obtain(null, MSG_AUDIO_ONSETS);
                                Bundle data = new Bundle();
                                data.putString("events", payload);
                                data.putLong("captured_at", capturedAt);
                                event.setData(data);
                                sendToAllClients(event);
                            }
                        });
                    });
                }
                if (SystemClock.uptimeMillis() - lastMeterAt < 85) {
                    main.postDelayed(this, 20);
                    return;
                }
                lastMeterAt = SystemClock.uptimeMillis();
                int spec;
                float peak;
                if ("xiaomi".equals(activeProtocol)) {
                    spec = NativeBridge.audioSpectrum();
                    peak = ((spec >>> 24) & 0xff) / 255f;
                } else {
                    spec = dlnaServer != null ? dlnaServer.audioSpectrum() : 0;
                    peak = ((spec >>> 24) & 0xff) / 255f;
                }
                if (Math.abs(peak - lastSentPeak) >= 0.025f || spec != lastSentSpectrum || Math.abs(mediaPositionMs - lastSentPosMs) >= 180L) {
                    lastSentPeak = peak;
                    lastSentSpectrum = spec;
                    lastSentPosMs = mediaPositionMs;
                    Message msg = Message.obtain(null, MSG_AUDIO_LEVEL);
                    Bundle b = new Bundle();
                    b.putFloat("peak", peak);
                    b.putInt("spectrum", spec);
                    b.putLong("position_ms", mediaPositionMs);
                    msg.setData(b);
                    sendToAllClients(msg);
                }
            }
            if ((!playing || !uiVisible) && dlnaServer != null) dlnaServer.releaseVisualizer();
            if (!playing || !uiVisible) {
                if (!onsetBusy) analysis.post(() -> NativeBridge.audioOnsets(false));
            }
            main.postDelayed(this, (playing && uiVisible) ? 20 : 350);
        }
    };

    private static String optCleanString(JSONObject obj, String key) {
        if (obj == null || obj.isNull(key)) return "";
        String val = obj.optString(key, "");
        return "null".equals(val) ? "" : val;
    }

    private void handleNativeEvent(JSONObject e) {
        String event = optCleanString(e, "event");
        if (!"progress".equals(event) && !"protocol_trace".equals(event) && !"discovery_packet_received".equals(event)) {
            record("native", e.toString());
        }
        if ("dlna".equals(activeProtocol)) {
            boolean newNativeConnection = "interconnect_stage".equals(event)
                    && "transport_connected".equals(e.optString("stage")) && !sessionActive;
            if (newNativeConnection) {
                activeProtocol = "xiaomi";
            } else {
                // A matching MiPlay side channel can supply a live lyric sentence,
                // but cannot replace DLNA metadata, clock, or play/pause state.
                if ("media_info".equals(event) && sessionActive && !mediaTitle.isEmpty()) {
                    String album = optCleanString(e, "album");
                    String artist = optCleanString(e, "artist");
                    String title = optCleanString(e, "title");
                    boolean sameTrack = mediaTitle.equals(album)
                            || (artist.startsWith(mediaTitle + "-")
                            && (mediaArtist.isEmpty() || artist.endsWith("-" + mediaArtist)));
                    if (sameTrack) {
                        String line = optCleanString(e, "lyric_line");
                        if (line.isEmpty() && !title.equals(mediaTitle)) line = title;
                        String lrc = optCleanString(e, "lyrics");
                        if ((!line.isEmpty() && !line.equals(mediaLyricLine)) || (!lrc.isEmpty() && !lrc.equals(mediaLyricsLrc))) {
                            if (!line.isEmpty()) mediaLyricLine = line;
                            if (!lrc.isEmpty()) mediaLyricsLrc = lrc;
                            lastMediaSeq++;
                            broadcastMediaEvent();
                        }
                    }
                }
                return;
            }
        }
        boolean snapshotDirty = false;
        boolean mediaDirty = false;

        switch (event) {
            case "interconnect_stage": {
                String stage = e.optString("stage");
                if ("transport_connected".equals(stage) && !sessionActive) {
                    statusText = "设备已握手，正在协商";
                    snapshotDirty = true;
                } else if ("media_session_established".equals(stage)) {
                    activeProtocol = "xiaomi";
                    sessionActive = true;
                    statusText = "音频信道已就绪";
                    acquirePlaybackLocks();
                    snapshotDirty = true;
                } else if ("transport_disconnected".equals(stage) && !sessionActive) {
                    statusText = idleStatus();
                    snapshotDirty = true;
                }
                break;
            }
            case "source_info":
                sourceName = optCleanString(e, "source_name");
                snapshotDirty = true;
                break;
            case "stream_started":
                activeProtocol = "xiaomi";
                sessionActive = true;
                playing = true;
                statusText = "正在接收投送音频";
                acquirePlaybackLocks();
                snapshotDirty = true;
                break;
            case "audio_output_signal_started":
                activeProtocol = "xiaomi";
                playing = true;
                sessionActive = true;
                statusText = "正在播放投送音频";
                acquirePlaybackLocks();
                snapshotDirty = true;
                mediaDirty = true;
                break;
            case "playback_state": {
                boolean p = e.optBoolean("playing", false);
                sessionActive = e.optBoolean("session_active", sessionActive);
                if (playing != p) {
                    playing = p;
                    snapshotDirty = true;
                    mediaDirty = true;
                }
                if (sessionActive) {
                    statusText = playing ? "正在播放投送音频" : "播放已暂停";
                    if (playing) acquirePlaybackLocks();
                    else if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
                    snapshotDirty = true;
                }
                break;
            }
            case "media_channel_closed":
            case "stream_stopped":
                playing = false;
                sessionActive = false;
                mediaTitle = "";
                mediaArtist = "";
                mediaAlbum = "";
                mediaLyricsLrc = "";
                mediaLyricLine = "";
                mediaArtwork = "";
                mediaDurationMs = 0;
                mediaPositionMs = 0;
                statusText = idleStatus();
                releasePlaybackLocks();
                snapshotDirty = true;
                mediaDirty = true;
                break;
            case "media_info": {
                activeProtocol = "xiaomi";
                mediaTitle = optCleanString(e, "title");
                mediaArtist = optCleanString(e, "artist");
                mediaAlbum = optCleanString(e, "album");
                mediaLyricsLrc = optCleanString(e, "lyrics");
                mediaLyricLine = optCleanString(e, "lyric_line");
                String art = optCleanString(e, "artwork").replaceAll("\\s+", "");
                if (art.length() <= 196608) mediaArtwork = art;
                else mediaArtwork = "";
                mediaDurationMs = e.optLong("duration_ms", 0);
                if (!e.isNull("position_ms")) mediaPositionMs = e.optLong("position_ms", 0);
                lastMediaSeq++;
                updateMediaSessionMetadata();
                mediaDirty = true;
                snapshotDirty = true;
                break;
            }
            case "volume":
                applySystemVolume(e.optInt("percent", currentVolumePercent));
                break;
            case "audio_format":
                formatText = optCleanString(e, "codec") + " · " + e.optInt("sample_rate") + " Hz · " + e.optInt("channels") + "ch";
                snapshotDirty = true;
                break;
            case "error": {
                String msg = optCleanString(e, "message");
                lastError = msg.isEmpty() ? optCleanString(e, "error") : msg;
                snapshotDirty = true;
                break;
            }
        }

        if (snapshotDirty || mediaDirty) updateMediaSessionState();
        if (mediaDirty) broadcastMediaEvent();
        if (snapshotDirty) broadcastSnapshot();
        if (SystemClock.elapsedRealtime() - lastNoticeTime > 1500 || mediaDirty || snapshotDirty) {
            updateNotification(false);
        }
    }

    private void updateMediaSessionMetadata() {
        if (mediaSession == null) return;
        String displaySong = !mediaAlbum.isEmpty() ? mediaAlbum : mediaTitle;
        mediaSession.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, displaySong)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, mediaArtist)
                .build());
    }

    private void updateMediaSessionState() {
        if (mediaSession == null) return;
        mediaSession.setPlaybackState(new PlaybackState.Builder()
                .setState(playing ? PlaybackState.STATE_PLAYING : (sessionActive ? PlaybackState.STATE_PAUSED : PlaybackState.STATE_STOPPED),
                        mediaPositionMs, 1f)
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_STOP)
                .build());
    }

    private void dispatchAction(String action, int value) {
        if ("dlna".equals(activeProtocol) && dlnaServer != null
                && ("output_resume".equals(action) || "suspend".equals(action))) {
            // Audio focus applies to the selected player only. Keep the parallel
            // native transport silent while DLNA is rendering audio.
            dlnaServer.togglePauseResume("suspend".equals(action));
            return;
        }
        if ("dlna".equals(activeProtocol) && dlnaServer != null && ("pause".equals(action) || "resume".equals(action))) {
            dlnaServer.togglePauseResume("pause".equals(action));
            return;
        }
        if (work == null || destroyed) return;
        work.post(() -> {
            try {
                String err = NativeBridge.action(action, value);
                if (err != null && !err.isEmpty()) record("control", action + ": " + err);
            } catch (Throwable ex) {
                record("control", ex.toString());
            }
        });
    }

    private Bundle buildSnapshotBundle() {
        SharedPreferences prefs = getSharedPreferences(PREF, MODE_PRIVATE);
        Bundle b = new Bundle();
        b.putBoolean("enabled", desired);
        b.putBoolean("miplay_running", miplayRunning);
        b.putBoolean("dlna_running", dlnaRunning);
        b.putBoolean("playing", playing);
        b.putBoolean("session_active", sessionActive);
        b.putString("active_protocol", activeProtocol);
        b.putString("status", statusText);
        b.putString("endpoint", endpointText);
        b.putString("format", formatText);
        b.putString("source_name", sourceName);
        b.putString("last_error", lastError);
        b.putString("device_name", prefs.getString("name", "SoundFlow-Pad"));
        b.putInt("device_type", prefs.getInt("device_type", 4));
        b.putBoolean("enable_dlna", prefs.getBoolean("enable_dlna", true));
        b.putBoolean("auto_online_lyrics", prefs.getBoolean("auto_online_lyrics", false));
        b.putInt("volume", currentVolumePercent);
        b.putInt("audio_sync_offset_ms", prefs.getInt(PREF_AUDIO_SYNC_OFFSET, 0));
        b.putString("title", mediaTitle);
        b.putString("artist", mediaArtist);
        b.putString("album", mediaAlbum);
        b.putString("lyrics_lrc", mediaLyricsLrc);
        b.putString("lyric_line", mediaLyricLine);
        b.putString("artwork", mediaArtwork);
        b.putLong("duration_ms", mediaDurationMs);
        b.putLong("position_ms", mediaPositionMs);
        b.putLong("media_seq", lastMediaSeq);
        return b;
    }

    private void sendSnapshotTo(Messenger client) {
        Message msg = Message.obtain(null, MSG_SNAPSHOT);
        msg.setData(buildSnapshotBundle());
        try { client.send(msg); } catch (RemoteException ignored) {}
    }

    private void broadcastSnapshot() {
        if (clients.isEmpty()) return;
        Bundle b = buildSnapshotBundle();
        Iterator<Messenger> it = clients.iterator();
        while (it.hasNext()) {
            Messenger m = it.next();
            Message msg = Message.obtain(null, MSG_SNAPSHOT);
            msg.setData(new Bundle(b));
            try { m.send(msg); } catch (RemoteException e) { it.remove(); }
        }
    }

    private void broadcastMediaEvent() {
        if (clients.isEmpty()) return;
        Bundle b = buildSnapshotBundle();
        Iterator<Messenger> it = clients.iterator();
        while (it.hasNext()) {
            Messenger m = it.next();
            Message msg = Message.obtain(null, MSG_MEDIA_EVENT);
            msg.setData(new Bundle(b));
            try { m.send(msg); } catch (RemoteException e) { it.remove(); }
        }
    }

    private void sendToAllClients(Message template) {
        Iterator<Messenger> it = clients.iterator();
        while (it.hasNext()) {
            Messenger m = it.next();
            Message copy = Message.obtain();
            copy.copyFrom(template);
            try { m.send(copy); } catch (RemoteException e) { it.remove(); }
        }
    }

    private PendingIntent svcAction(String action) {
        return PendingIntent.getService(
                this,
                action.hashCode(),
                new Intent(this, ReceiverService.class).setAction(action),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );
    }

    private Notification buildNotification() {
        PendingIntent open = PendingIntent.getActivity(
                this, 0,
                new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );
        String line = mediaTitle.isEmpty() ? statusText : (mediaTitle + " · " + statusText);
        Notification.Builder b = new Notification.Builder(this, SoundFlowApp.CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_soundflow_notif)
                .setContentTitle("音流独立音频接收器")
                .setContentText(line)
                .setContentIntent(open)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setVisibility(Notification.VISIBILITY_PUBLIC);
        if (sessionActive) {
            b.addAction(new Notification.Action.Builder(null, playing ? "暂停" : "播放", svcAction(playing ? ACTION_PAUSE : ACTION_RESUME)).build());
        }
        b.addAction(new Notification.Action.Builder(null, "停止接收", svcAction(ACTION_STOP)).build());
        return b.build();
    }

    private void updateNotification(boolean force) {
        long now = SystemClock.elapsedRealtime();
        if (!force && now - lastNoticeTime < 1500) return;
        lastNoticeTime = now;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.notify(NOTICE_ID, buildNotification());
    }

    private synchronized void record(String kind, String text) {
        Log.i(TAG, kind + ": " + text);
        try {
            if (logFile.length() > 512 * 1024) {
                File prev = new File(getFilesDir(), "receiver.previous.log");
                prev.delete();
                logFile.renameTo(prev);
            }
            try (FileOutputStream fos = new FileOutputStream(logFile, true)) {
                fos.write((System.currentTimeMillis() + " [" + kind + "] " + text + "\n").getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException ignored) {}
    }

    private String readLogTail() {
        try (FileInputStream in = new FileInputStream(logFile)) {
            long skip = Math.max(0, logFile.length() - 48 * 1024);
            while (skip > 0) {
                long n = in.skip(skip);
                if (n <= 0) break;
                skip -= n;
            }
            byte[] buf = new byte[49152];
            int len = in.read(buf);
            return len > 0 ? new String(buf, 0, len, StandardCharsets.UTF_8) : "暂无日志";
        } catch (IOException e) {
            return "暂无诊断日志";
        }
    }

    private void stopReceiver() {
        desired = false;
        generation++;
        boundEndpoint = "";
        miplayRunning = false;
        dlnaRunning = false;
        playing = false;
        sessionActive = false;
        formatText = "";
        sourceName = "";
        mediaTitle = "";
        mediaArtist = "";
        mediaAlbum = "";
        mediaLyricsLrc = "";
        mediaLyricLine = "";
        mediaArtwork = "";
        mediaDurationMs = 0;
        mediaPositionMs = 0;
        lastError = "";
        getSharedPreferences(PREF, MODE_PRIVATE).edit().putBoolean("enabled", false).apply();
        main.removeCallbacks(reconnectTask);
        KeepAliveJobService.cancel(this);
        statusText = "接收已停止";
        releasePlaybackLocks();
        updateMediaSessionState();
        stopForeground(STOP_FOREGROUND_REMOVE);
        broadcastMediaEvent();
        broadcastSnapshot();
        if (work != null && !destroyed) {
            work.post(() -> {
                NativeBridge.stop();
                miplayEngineStarted = false;
                if (dlnaServer != null) {
                    dlnaServer.stop();
                    dlnaServer = null;
                }
                main.post(() -> {
                    if (!desired) releaseNetworkLocks();
                });
            });
        } else {
            NativeBridge.stop();
            miplayEngineStarted = false;
            releaseNetworkLocks();
        }
        record("lifecycle", "receiver stopped and ports released");
        stopSelf();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        super.onTaskRemoved(rootIntent);
        // Re-ensure service keeps alive even when UI task card is swiped away.
        if (desired && !destroyed) {
            KeepAliveJobService.schedule(this);
            try {
                startForegroundService(new Intent(this, ReceiverService.class).setAction(ACTION_START));
            } catch (Throwable ignored) {}
        }
    }

    @Override
    public void onDestroy() {
        destroyed = true;
        desired = false;
        generation++;
        analysis.removeCallbacksAndMessages(null);
        analysisThread.quitSafely();
        main.removeCallbacksAndMessages(null);
        try { if (networkCallback != null) connectivity.unregisterNetworkCallback(networkCallback); } catch (Throwable ignored) {}
        try { if (volumeReceiver != null) unregisterReceiver(volumeReceiver); } catch (Throwable ignored) {}
        releasePlaybackLocks();
        if (mediaSession != null) mediaSession.release();
        miplayRunning = false;
        dlnaRunning = false;
        playing = false;
        sessionActive = false;
        work.post(() -> {
            NativeBridge.stop();
            miplayEngineStarted = false;
            if (dlnaServer != null) {
                dlnaServer.stop();
                dlnaServer = null;
            }
            releaseNetworkLocks();
            workerThread.quitSafely();
        });
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return incomingMessenger.getBinder();
    }
}
