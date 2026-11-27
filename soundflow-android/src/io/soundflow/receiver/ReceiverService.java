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
import java.io.BufferedOutputStream;
import java.io.IOException;
import java.net.Inet4Address;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Isolated background process (:receiver) hosting Xiaomi MiPlay clean-room
 * and standard DLNA/UPnP audio renderers, MediaSession, and anti-kill guards.
 */
public final class ReceiverService extends Service {
    public static final String TAG = "SoundFlowSvc";
    public static final String PREF = "soundflow_cfg";
    private static final String PREF_AUDIO_SYNC_OFFSET = "audio_sync_offset_ms";
    private static final String PREF_ONSET_SENSITIVITY = "onset_sensitivity";
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
    public static final int MSG_SET_ONSET_SENSITIVITY = 10;
    public static final int MSG_UPDATE_CONFIG = 11;
    public static final int MSG_SET_AUDIO_SYNC_OFFSET = 12;
    public static final int MSG_CONTROL_ACTION = 13;
    public static final int MSG_SET_VOLUME = 14;

    private static final int NOTICE_ID = 2026;
    private static final Object WORKER_LOCK = new Object();
    private static HandlerThread sharedWorkerThread;
    private static Handler sharedWork;
    private static final AtomicLong ENGINE_OWNER = new AtomicLong(0);

    private static Handler getSharedWorkerHandler() {
        synchronized (WORKER_LOCK) {
            if (sharedWorkerThread == null || !sharedWorkerThread.isAlive()) {
                sharedWorkerThread = new HandlerThread("soundflow-control");
                sharedWorkerThread.start();
                sharedWork = new Handler(sharedWorkerThread.getLooper());
            }
            return sharedWork;
        }
    }

    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Messenger> clients = new ArrayList<>();
    private final Messenger incomingMessenger = new Messenger(new IncomingHandler());

    private Handler work;
    private long engineToken;
    private HandlerThread analysisThread;
    private Handler analysis;
    private HandlerThread eventThread;
    private Handler eventWorker;
    private volatile VisualRequest visualRequest;
    private VisualRequest samplingRequest;
    private final DlnaRendererServer.Spectrum spectrumScratch = new DlnaRendererServer.Spectrum();
    private long lastMeterAt;
    private long visualGeneration;
    private long dlnaSession;
    private long nativeMediaGeneration;
    private boolean screenInteractive = true;
    private boolean motionAllowed;
    private boolean rippleEnabled;
    private BufferedOutputStream logOutput;
    private long logBytes;
    private final ThreadPoolExecutor logWriter = new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS,
            new ArrayBlockingQueue<>(128), r -> new Thread(r, "soundflow-log"),
            new ThreadPoolExecutor.DiscardOldestPolicy());

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
    private volatile boolean sessionActive = false;
    private boolean desired = false;
    private volatile boolean destroyed = false;
    private boolean focusHeld = false;
    private boolean uiVisible = false;
    private float lastSentPeak = -1f;
    private int lastSentSpectrum = -1;
    private long lastSentPosMs = -1L;
    private volatile boolean miplayEngineStarted = false;
    private int lastVolumeIndex = -1;
    private int currentVolumePercent = 45;
    private long generation = 0;
    private int foregroundType;
    private String publishedNoticeTitle;
    private String publishedNoticeStatus;
    private int publishedNoticeAction = -1;
    private boolean nativeSessionActive;
    private boolean nativePlaying;
    private long publishedMediaPosition;
    private long publishedMediaAt;

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
    private long mediaPositionAnchorElapsed = 0;
    private long suppressVolumeEchoUntil = 0;
    private long lastMediaSeq = 0;

    private long currentLivePositionMs() {
        if (!playing || mediaPositionAnchorElapsed <= 0) return mediaPositionMs;
        long pos = mediaPositionMs + Math.max(0L, SystemClock.elapsedRealtime() - mediaPositionAnchorElapsed);
        return mediaDurationMs > 0 ? Math.min(mediaDurationMs, pos) : pos;
    }

    private final class IncomingHandler extends Handler {
        IncomingHandler() {
            super(Looper.getMainLooper());
        }

        @Override
        public void handleMessage(Message msg) {
            if (msg.what == MSG_REGISTER && msg.replyTo != null) {
                if (!clients.contains(msg.replyTo)) clients.add(msg.replyTo);
                sendSnapshotTo(msg.replyTo);
                updateVisualRequest();
            } else if (msg.what == MSG_UNREGISTER && msg.replyTo != null) {
                clients.remove(msg.replyTo);
                updateVisualRequest();
            } else if (msg.what == MSG_UI_VISIBILITY) {
                uiVisible = msg.arg1 != 0;
                motionAllowed = (msg.arg2 & 1) != 0;
                rippleEnabled = (msg.arg2 & 2) != 0;
                updateVisualRequest();
            } else if (msg.what == MSG_FETCH_LOGS && msg.replyTo != null) {
                final Messenger client = msg.replyTo;
                logWriter.execute(() -> {
                    Message reply = Message.obtain(null, MSG_LOG_TEXT);
                    Bundle b = new Bundle();
                    b.putString("logs", readLogTail());
                    reply.setData(b);
                    try { client.send(reply); } catch (RemoteException ignored) {}
                });
            } else if (msg.what == MSG_SET_ONSET_SENSITIVITY) {
                int sens = Math.max(0, Math.min(100, msg.arg1));
                getSharedPreferences(PREF, MODE_PRIVATE).edit().putInt(PREF_ONSET_SENSITIVITY, sens).apply();
                NativeBridge.setOnsetSensitivity(sens);
            } else if (msg.what == MSG_SET_AUDIO_SYNC_OFFSET) {
                int offset = Math.max(-400, Math.min(400, msg.arg1));
                getSharedPreferences(PREF, MODE_PRIVATE).edit().putInt(PREF_AUDIO_SYNC_OFFSET, offset).apply();
                NativeBridge.setAudioSyncOffset(offset);
                broadcastSnapshot();
            } else if (msg.what == MSG_SET_VOLUME) {
                applySystemVolume(msg.arg1);
            } else if (msg.what == MSG_CONTROL_ACTION) {
                Bundle b = msg.getData();
                String cmd = b != null ? b.getString("cmd", "") : "";
                if ("pause".equals(cmd) || "resume".equals(cmd) || "prev".equals(cmd) || "next".equals(cmd)) {
                    dispatchAction(cmd, 0);
                }
            } else if (msg.what == MSG_UPDATE_CONFIG) {
                Bundle b = msg.getData();
                if (b != null) {
                    Intent synthetic = new Intent();
                    if (b.containsKey("name")) synthetic.putExtra("name", b.getString("name"));
                    if (b.containsKey("device_type")) synthetic.putExtra("device_type", b.getInt("device_type", 4));
                    if (b.containsKey("enable_dlna")) synthetic.putExtra("enable_dlna", b.getBoolean("enable_dlna", true));
                    if (b.containsKey("auto_online_lyrics")) synthetic.putExtra("auto_online_lyrics", b.getBoolean("auto_online_lyrics", false));
                    boolean networkChanged = applyConfigExtras(getSharedPreferences(PREF, MODE_PRIVATE), synthetic);
                    if (desired && networkChanged) {
                        boundEndpoint = "";
                        reconnect();
                    } else {
                        broadcastSnapshot();
                    }
                }
            }
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        work = getSharedWorkerHandler();
        engineToken = ENGINE_OWNER.incrementAndGet();
        analysisThread = new HandlerThread("soundflow-onset");
        analysisThread.start();
        analysis = new Handler(analysisThread.getLooper());
        eventThread = new HandlerThread("soundflow-events");
        eventThread.start();
        eventWorker = new Handler(eventThread.getLooper());
        audio = (AudioManager) getSystemService(AUDIO_SERVICE);
        connectivity = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        logFile = new File(getFilesDir(), "receiver.log");
        SharedPreferences prefs = getSharedPreferences(PREF, MODE_PRIVATE);
        NativeBridge.setAudioSyncOffset(prefs.getInt(PREF_AUDIO_SYNC_OFFSET, 0));
        NativeBridge.setOnsetSensitivity(prefs.getInt(PREF_ONSET_SENSITIVITY, 55));

        initMediaSession();
        initVolumeReceiver();
        initNetworkCallback();
        eventWorker.post(eventPoller);
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
                String action = i.getAction();
                if (Intent.ACTION_SCREEN_OFF.equals(action) || Intent.ACTION_SCREEN_ON.equals(action)) {
                    screenInteractive = Intent.ACTION_SCREEN_ON.equals(action);
                    updateVisualRequest();
                } else {
                    main.removeCallbacks(volumeDebouncer);
                    main.postDelayed(volumeDebouncer, 90);
                }
            }
        };
        IntentFilter filter = new IntentFilter("android.media.VOLUME_CHANGED_ACTION");
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        PowerManager power = getSystemService(PowerManager.class);
        screenInteractive = power == null || power.isInteractive();
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
        if (foregroundType == type) return;
        try {
            startForeground(NOTICE_ID, buildNotification(), type);
            foregroundType = type;
            rememberNotification();
        } catch (Throwable ex) {
            try {
                startForeground(NOTICE_ID, buildNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
                foregroundType = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
                rememberNotification();
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
        boolean allowPublish = publishToRemote && SystemClock.elapsedRealtime() >= suppressVolumeEchoUntil;
        if (allowPublish && changed && miplayRunning) {
            dispatchAction("volume", percent);
        }
        broadcastSnapshot();
    }

    private void applySystemVolume(int percent) {
        applySystemVolume(percent, true);
    }

    private void applySystemVolume(int percent, boolean publishToRemote) {
        if (audio == null) return;
        int clamped = Math.max(0, Math.min(100, percent));
        int max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
        int targetIndex = Math.round(clamped * max / 100f);
        if (!publishToRemote) {
            suppressVolumeEchoUntil = SystemClock.elapsedRealtime() + 350L;
        }
        if (audio.getStreamVolume(AudioManager.STREAM_MUSIC) != targetIndex) {
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, targetIndex, 0);
        }
        updateSystemVolume(publishToRemote);
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
        @Override public void onDlnaPreparing(long session, String title, String artist, String album) {
            main.post(() -> {
                if (destroyed || !desired) return;
                // SetURI is an explicit choice of transport. Its progress and
                // state own this session until Stop, not concurrent MiPlay polls.
                boolean changedTrack = !mediaTitle.equals(title == null ? "" : title)
                        || !mediaArtist.equals(artist == null ? "" : artist);
                activeProtocol = "dlna";
                dlnaSession = session;
                visualGeneration++;
                playing = false;
                sessionActive = true;
                if (changedTrack) { mediaLyricsLrc = ""; mediaLyricLine = ""; mediaArtwork = ""; }
                mediaTitle = title != null ? title : "";
                mediaArtist = artist != null ? artist : "";
                mediaAlbum = album != null ? album : "";
                updateMediaSessionMetadata();
                if (work != null) work.post(() -> NativeBridge.action("suspend", 0));
                broadcastSnapshot();
            });
        }

        @Override
        public void onDlnaState(long session, boolean isPlaying, String transportState, String title, String artist, String album, long durationMs, long positionMs) {
            main.post(() -> {
                if (destroyed || !desired || session != dlnaSession || !"dlna".equals(activeProtocol)) return;
                boolean nextActive = isPlaying || "PAUSED_PLAYBACK".equals(transportState);
                long previousPosition = currentLivePositionMs();
                boolean changed = playing != isPlaying || sessionActive != nextActive
                        || mediaDurationMs != durationMs || Math.abs(positionMs - previousPosition) >= 450L
                        || !mediaTitle.equals(title) || !mediaArtist.equals(artist) || !mediaAlbum.equals(album);
                playing = isPlaying;
                sessionActive = nextActive;
                mediaTitle = title != null ? title : "";
                mediaArtist = artist != null ? artist : "";
                mediaAlbum = album != null ? album : "";
                mediaDurationMs = durationMs;
                mediaPositionMs = positionMs;
                mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
                if (!changed) return;
                lastMediaSeq++;
                formatText = "DLNA Audio · 48 kHz";
                sourceName = "DLNA / UPnP 发送端";
                statusText = isPlaying ? "正在播放 DLNA 投送" : (sessionActive ? "DLNA 已暂停" : idleStatus());
                if (isPlaying) acquirePlaybackLocks();
                else if (!sessionActive) releasePlaybackLocks();
                updateMediaSessionMetadata();
                updateMediaSessionState();
                broadcastMediaEvent();
                broadcastSnapshot();
                updateNotification(false);
            });
        }

        @Override public void onDlnaVolume(int percent) { main.post(() -> applySystemVolume(percent, false)); }
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
        main.removeCallbacks(renewPlaybackWakeLock);
        main.postDelayed(renewPlaybackWakeLock, 5 * 60 * 1000L);
        if (mediaSession != null) mediaSession.setActive(true);
        promoteForeground(true);
    }

    private final Runnable renewPlaybackWakeLock = new Runnable() {
        @Override public void run() {
            if (!playing || destroyed || wakeLock == null || !wakeLock.isHeld()) return;
            wakeLock.acquire(10 * 60 * 1000L);
            main.postDelayed(this, 5 * 60 * 1000L);
        }
    };

    private void releasePlaybackLocks() {
        main.removeCallbacks(renewPlaybackWakeLock);
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        if (focusHeld && audio != null) {
            audio.abandonAudioFocusRequest(focusRequest);
            focusHeld = false;
        }
        if (mediaSession != null) mediaSession.setActive(false);
        if (desired && !destroyed) promoteForeground(false);
    }

    private final Runnable eventPoller = new Runnable() {
        @Override public void run() {
            if (destroyed) return;
            try {
                String raw = NativeBridge.poll();
                if (raw != null && !"[]".equals(raw)) {
                    JSONArray events = new JSONArray(raw);
                    main.post(() -> {
                        if (destroyed || !desired || ENGINE_OWNER.get() != engineToken) return;
                        JSONObject lastVolume = null;
                        for (int i = 0; i < events.length(); i++) {
                            JSONObject event = events.optJSONObject(i);
                            if (event == null) continue;
                            if ("volume".equals(event.optString("event"))) lastVolume = event;
                            else handleNativeEvent(event);
                        }
                        if (lastVolume != null) handleNativeEvent(lastVolume);
                    });
                }
            } catch (Exception ex) {
                Log.e(TAG, "poll error", ex);
            }
            eventWorker.postDelayed(this, sessionActive ? 90 : 500);
        }
    };

    private static final class VisualRequest {
        final long generation;
        final boolean xiaomi;
        final boolean onsets;
        final DlnaRendererServer dlna;

        VisualRequest(long generation, boolean xiaomi, boolean onsets, DlnaRendererServer dlna) {
            this.generation = generation;
            this.xiaomi = xiaomi;
            this.onsets = onsets;
            this.dlna = dlna;
        }
    }

    private void updateVisualRequest() {
        boolean wanted = !destroyed && playing && uiVisible && screenInteractive && motionAllowed && !clients.isEmpty();
        boolean xiaomi = "xiaomi".equals(activeProtocol);
        DlnaRendererServer dlna = xiaomi ? null : dlnaServer;
        VisualRequest current = visualRequest;
        if ((!wanted && current == null) || (wanted && current != null
                && current.generation == visualGeneration && current.xiaomi == xiaomi
                && current.onsets == rippleEnabled && current.dlna == dlna)) return;
        visualRequest = wanted ? new VisualRequest(visualGeneration, xiaomi, rippleEnabled, dlna) : null;
        analysis.post(() -> {
            analysis.removeCallbacks(audioLevelTicker);
            audioLevelTicker.run();
        });
    }

    private final Runnable audioLevelTicker = new Runnable() {
        @Override public void run() {
            VisualRequest request = visualRequest;
            if (samplingRequest != request) {
                if (samplingRequest != null && samplingRequest.dlna != null) {
                    samplingRequest.dlna.releaseVisualizer();
                }
                if (ENGINE_OWNER.get() == engineToken) {
                    NativeBridge.setAudioMeterEnabled(request != null && request.xiaomi);
                    NativeBridge.setOnsetCapture(false, System.nanoTime());
                    if (request != null && request.xiaomi && request.onsets) {
                        NativeBridge.setOnsetCapture(true, System.nanoTime());
                    }
                }
                samplingRequest = request;
                lastMeterAt = 0;
            }
            if (request == null || destroyed || ENGINE_OWNER.get() != engineToken) return;
            try {
                long now = SystemClock.elapsedRealtime();
                boolean meterDue = now - lastMeterAt >= 85L;
                String onsets = null;
                int spectrum = 0;
                if (request.xiaomi) {
                    if (request.onsets) onsets = NativeBridge.audioOnsets();
                    if (meterDue) spectrum = NativeBridge.audioSpectrum();
                } else if (request.dlna != null
                        && request.dlna.captureSpectrum(spectrumScratch, request.onsets, meterDue)) {
                    if (request.onsets) onsets = NativeBridge.spectrumOnsets(
                            spectrumScratch.bins, spectrumScratch.sampleRate, spectrumScratch.capturedNanos);
                    if (meterDue) spectrum = spectrumScratch.packed;
                }
                if (meterDue) lastMeterAt = now;
                if (onsets != null || meterDue) {
                    final String payload = onsets;
                    final int packed = spectrum;
                    main.post(() -> publishVisualSample(request, payload, meterDue, packed));
                }
            } catch (RuntimeException e) {
                Log.w(TAG, "visual analysis", e);
            } finally {
                if (visualRequest == request && !destroyed) {
                    analysis.postDelayed(this, request.onsets ? 12L : 85L);
                }
            }
        }
    };

    private void publishVisualSample(VisualRequest request, String onsets, boolean meterDue, int packed) {
        if (visualRequest != request || destroyed || !playing) return;
        long pos = meterDue ? currentLivePositionMs() : 0;
        float peak = ((packed >>> 24) & 0xff) / 255f;
        boolean meterChanged = meterDue && (Math.abs(peak - lastSentPeak) >= 0.025f
                || packed != lastSentSpectrum || Math.abs(pos - lastSentPosMs) >= 240L);
        if (onsets == null && !meterChanged) return;
        Bundle data = new Bundle();
        if (onsets != null) data.putString("events", onsets);
        if (meterChanged) {
            lastSentPeak = peak;
            lastSentSpectrum = packed;
            lastSentPosMs = pos;
            data.putInt("spectrum", packed);
            data.putLong("position_ms", pos);
        }
        Message msg = Message.obtain(null, MSG_AUDIO_ONSETS);
        data.putLong("visual_generation", request.generation);
        msg.setData(data);
        sendToAllClients(msg);
        msg.recycle();
    }

    private static String optCleanString(JSONObject obj, String key) {
        if (obj == null || obj.isNull(key)) return "";
        String val = obj.optString(key, "");
        return "null".equals(val) ? "" : val;
    }

    private void activateXiaomiTransport() {
        boolean wasDlna = "dlna".equals(activeProtocol);
        activeProtocol = "xiaomi";
        if (wasDlna) {
            visualGeneration++;
            mediaTitle = "";
            mediaArtist = "";
            mediaAlbum = "";
            mediaArtwork = "";
            mediaLyricsLrc = "";
            mediaLyricLine = "";
            mediaDurationMs = 0;
            mediaPositionMs = 0;
            mediaPositionAnchorElapsed = 0;
            sourceName = "小米妙播发送端";
            formatText = "";
            updateMediaSessionMetadata();
            if (dlnaServer != null) dlnaServer.stopPlayback();
        }
        if (focusHeld) {
            dispatchAction("output_resume", 0);
        }
    }

    private void handleNativeEvent(JSONObject e) {
        String event = optCleanString(e, "event");
        if (!"progress".equals(event) && !"protocol_trace".equals(event) && !"discovery_packet_received".equals(event)) {
            if (!event.startsWith("discovery_query")) logWriter.execute(() -> writeLog("native", e.toString()));
        }
        String stage = "interconnect_stage".equals(event) ? e.optString("stage") : "";
        switch (event) {
            case "interconnect_stage":
                if ("media_session_established".equals(stage)) nativeSessionActive = true;
                break;
            case "stream_started":
            case "audio_output_signal_started":
                nativeSessionActive = true;
                nativePlaying = true;
                break;
            case "playback_state":
                nativePlaying = e.optBoolean("playing");
                nativeSessionActive = e.optBoolean("session_active", nativeSessionActive);
                break;
            case "media_channel_closed":
            case "stream_stopped":
                nativeSessionActive = false;
                nativePlaying = false;
                break;
        }
        boolean reclaimFromMetadata = false;
        if ("dlna".equals(activeProtocol)) {
            boolean explicitResume = "playback_state".equals(event) && e.optBoolean("playing");
            reclaimFromMetadata = !sessionActive && nativeSessionActive && "media_info".equals(event);
            boolean reclaimIdleDlna = (!playing && explicitResume) || reclaimFromMetadata;
            boolean newNativeSession = reclaimIdleDlna
                    || ("transport_connected".equals(stage) && !sessionActive)
                    || "media_session_established".equals(stage)
                    || "stream_started".equals(event)
                    || "audio_output_signal_started".equals(event);
            if (newNativeSession) {
                activateXiaomiTransport();
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
        long incomingGeneration = e.optLong("media_generation", 0);
        if (incomingGeneration != 0 && incomingGeneration != nativeMediaGeneration) {
            nativeMediaGeneration = incomingGeneration;
            visualGeneration++;
        }
        boolean snapshotDirty = false;
        boolean mediaDirty = false;

        switch (event) {
            case "interconnect_stage": {
                if ("transport_connected".equals(stage) && !sessionActive) {
                    statusText = "设备已握手，正在协商";
                    snapshotDirty = true;
                } else if ("media_session_established".equals(stage)) {
                    activateXiaomiTransport();
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
                activateXiaomiTransport();
                sessionActive = true;
                playing = true;
                mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
                statusText = "正在接收投送音频";
                acquirePlaybackLocks();
                snapshotDirty = true;
                break;
            case "audio_output_signal_started":
                activateXiaomiTransport();
                playing = true;
                sessionActive = true;
                mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
                statusText = "正在播放投送音频";
                acquirePlaybackLocks();
                snapshotDirty = true;
                mediaDirty = true;
                break;
            case "playback_state": {
                boolean p = e.optBoolean("playing", false);
                sessionActive = e.optBoolean("session_active", sessionActive);
                if (playing != p) {
                    mediaPositionMs = currentLivePositionMs();
                    mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
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
                mediaPositionAnchorElapsed = 0;
                statusText = idleStatus();
                releasePlaybackLocks();
                snapshotDirty = true;
                mediaDirty = true;
                break;
            case "media_info": {
                if (reclaimFromMetadata) {
                    sessionActive = nativeSessionActive;
                    playing = nativePlaying;
                    sourceName = "小米妙播发送端";
                    statusText = playing ? "正在播放投送音频" : "播放已暂停";
                    if (playing) acquirePlaybackLocks();
                }
                if (!mediaAlbum.equals(optCleanString(e, "album"))
                        || !mediaArtist.equals(optCleanString(e, "artist"))) visualGeneration++;
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
                if (!e.isNull("position_ms")) {
                    mediaPositionMs = e.optLong("position_ms", 0);
                    mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
                }
                lastMediaSeq++;
                updateMediaSessionMetadata();
                mediaDirty = true;
                snapshotDirty = true;
                break;
            }
            case "progress": {
                long nextPos = Math.max(0L, e.optLong("position_ms", 0L));
                long now = SystemClock.elapsedRealtime();
                long expected = publishedMediaPosition + (playing && publishedMediaAt != 0
                        ? Math.max(0L, now - publishedMediaAt) : 0L);
                mediaPositionMs = nextPos;
                mediaPositionAnchorElapsed = SystemClock.elapsedRealtime();
                if (Math.abs(nextPos - expected) >= 450L) {
                    mediaDirty = true;
                }
                break;
            }
            case "volume":
                applySystemVolume(e.optInt("percent", currentVolumePercent), false);
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

        updateVisualRequest();
        if (snapshotDirty || mediaDirty) updateMediaSessionState();
        if (mediaDirty) broadcastMediaEvent();
        if (snapshotDirty) broadcastSnapshot();
        if (mediaDirty || snapshotDirty) {
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
        publishedMediaPosition = currentLivePositionMs();
        publishedMediaAt = SystemClock.elapsedRealtime();
        mediaSession.setPlaybackState(new PlaybackState.Builder()
                .setState(playing ? PlaybackState.STATE_PLAYING : (sessionActive ? PlaybackState.STATE_PAUSED : PlaybackState.STATE_STOPPED),
                        publishedMediaPosition, playing ? 1f : 0f, publishedMediaAt)
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_STOP)
                .build());
    }

    private void dispatchAction(String action, int value) {
        if ("dlna".equals(activeProtocol)) {
            if (dlnaServer != null && ("output_resume".equals(action) || "suspend".equals(action))) {
                // Audio focus applies to the selected player only. Keep the parallel
                // native transport silent while DLNA is rendering audio.
                dlnaServer.togglePauseResume("suspend".equals(action));
                return;
            }
            if (dlnaServer != null && ("pause".equals(action) || "resume".equals(action))) {
                dlnaServer.togglePauseResume("pause".equals(action));
                return;
            }
            if ("prev".equals(action) || "next".equals(action)) {
                // Standard DLNA MediaRenderer does not support reverse skip to the controller;
                // never leak skip actions into the idle MiPlay transport.
                return;
            }
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

    private Bundle buildSnapshotBundle(boolean includeMedia) {
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
        if (includeMedia) {
            b.putString("lyrics_lrc", mediaLyricsLrc);
            b.putString("lyric_line", mediaLyricLine);
            b.putString("artwork", mediaArtwork);
        }
        b.putLong("duration_ms", mediaDurationMs);
        b.putLong("position_ms", currentLivePositionMs());
        b.putLong("media_seq", lastMediaSeq);
        b.putLong("visual_generation", visualGeneration);
        return b;
    }

    private void sendSnapshotTo(Messenger client) {
        Message msg = Message.obtain(null, MSG_SNAPSHOT);
        msg.setData(buildSnapshotBundle(true));
        try { client.send(msg); } catch (RemoteException ignored) {}
    }

    private void broadcastSnapshot() {
        updateVisualRequest();
        if (clients.isEmpty()) return;
        Bundle b = buildSnapshotBundle(false);
        Iterator<Messenger> it = clients.iterator();
        while (it.hasNext()) {
            Messenger m = it.next();
            Message msg = Message.obtain(null, MSG_SNAPSHOT);
            msg.setData(new Bundle(b));
            try { m.send(msg); } catch (RemoteException e) { it.remove(); }
        }
    }

    private void broadcastMediaEvent() {
        updateVisualRequest();
        if (clients.isEmpty()) return;
        Bundle b = buildSnapshotBundle(true);
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

    private int notificationAction() {
        return sessionActive ? (playing ? 2 : 1) : 0;
    }

    private void rememberNotification() {
        publishedNoticeTitle = mediaTitle;
        publishedNoticeStatus = statusText;
        publishedNoticeAction = notificationAction();
    }

    private void updateNotification(boolean force) {
        if (!force && mediaTitle.equals(publishedNoticeTitle) && statusText.equals(publishedNoticeStatus)
                && notificationAction() == publishedNoticeAction) return;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.notify(NOTICE_ID, buildNotification());
            rememberNotification();
        }
    }

    private void record(String kind, String text) {
        logWriter.execute(() -> writeLog(kind, text));
    }

    private void closeLog() {
        if (logOutput == null) return;
        try { logOutput.close(); } catch (IOException ignored) {}
        logOutput = null;
    }

    private void writeLog(String kind, String text) {
        Log.i(TAG, kind + ": " + text);
        byte[] line = (System.currentTimeMillis() + " [" + kind + "] " + text + "\n")
                .getBytes(StandardCharsets.UTF_8);
        try {
            if (logOutput == null) {
                logBytes = logFile.length();
                logOutput = new BufferedOutputStream(new FileOutputStream(logFile, true));
            }
            if (logBytes + line.length > 512 * 1024) {
                closeLog();
                File prev = new File(getFilesDir(), "receiver.previous.log");
                prev.delete();
                if (!logFile.renameTo(prev)) return;
                logOutput = new BufferedOutputStream(new FileOutputStream(logFile, true));
                logBytes = 0;
            }
            logOutput.write(line);
            logOutput.flush();
            logBytes += line.length;
        } catch (IOException ignored) { closeLog(); }
    }

    private String readLogTail() {
        try { if (logOutput != null) logOutput.flush(); } catch (IOException ignored) { closeLog(); }
        try (FileInputStream in = new FileInputStream(logFile)) {
            long fileLen = logFile.length();
            long skip = Math.max(0, fileLen - 48 * 1024);
            long skippedTotal = 0;
            while (skippedTotal < skip) {
                long n = in.skip(skip - skippedTotal);
                if (n <= 0) break;
                skippedTotal += n;
            }
            byte[] buf = new byte[49152];
            int total = 0;
            while (total < buf.length) {
                int n = in.read(buf, total, buf.length - total);
                if (n <= 0) break;
                total += n;
            }
            if (total <= 0) return "暂无日志";
            int offset = 0;
            if (skippedTotal > 0) {
                // Drop leading partial line to avoid splitting a multi-byte UTF-8 sequence.
                for (int i = 0; i < total; i++) {
                    if (buf[i] == '\n') {
                        offset = i + 1;
                        break;
                    }
                }
                if (offset >= total) offset = 0;
            }
            return new String(buf, offset, total - offset, StandardCharsets.UTF_8);
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
        mediaPositionAnchorElapsed = 0;
        lastError = "";
        getSharedPreferences(PREF, MODE_PRIVATE).edit().putBoolean("enabled", false).apply();
        main.removeCallbacks(reconnectTask);
        KeepAliveJobService.cancel(this);
        statusText = "接收已停止";
        releasePlaybackLocks();
        updateMediaSessionState();
        stopForeground(STOP_FOREGROUND_REMOVE);
        foregroundType = 0;
        broadcastMediaEvent();
        broadcastSnapshot();
        if (work != null && !destroyed) {
            final long ownerToken = engineToken;
            work.post(() -> {
                if (ENGINE_OWNER.get() != ownerToken) return;
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
        visualRequest = null;
        analysis.removeCallbacksAndMessages(null);
        analysis.post(audioLevelTicker);
        analysisThread.quitSafely();
        eventWorker.removeCallbacksAndMessages(null);
        eventThread.quitSafely();
        logWriter.execute(this::closeLog);
        logWriter.shutdown();
        main.removeCallbacksAndMessages(null);
        try { if (networkCallback != null) connectivity.unregisterNetworkCallback(networkCallback); } catch (Throwable ignored) {}
        try { if (volumeReceiver != null) unregisterReceiver(volumeReceiver); } catch (Throwable ignored) {}
        releasePlaybackLocks();
        if (mediaSession != null) mediaSession.release();
        miplayRunning = false;
        dlnaRunning = false;
        playing = false;
        sessionActive = false;
        final long ownerToken = engineToken;
        work.post(() -> {
            if (ENGINE_OWNER.get() != ownerToken) return;
            NativeBridge.stop();
            miplayEngineStarted = false;
            if (dlnaServer != null) {
                dlnaServer.stop();
                dlnaServer = null;
            }
            releaseNetworkLocks();
        });
        stopForeground(STOP_FOREGROUND_REMOVE);
        foregroundType = 0;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return incomingMessenger.getBinder();
    }
}
