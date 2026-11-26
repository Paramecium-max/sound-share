package io.soundflow.receiver;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.database.ContentObserver;
import android.graphics.Color;
import android.graphics.Insets;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.PowerManager;
import android.os.RemoteException;
import android.provider.Settings;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.webkit.ConsoleMessage;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Main UI hosting the iOS 26/27 Liquid Glass interface and bundled AMLL
 * GPU/Spring animated lyrics stage inside a virtual local origin.
 */
public final class MainActivity extends Activity {
    private static final String TAG = "SoundFlowUI";
    private static final String APP_HOST = "app.soundflow.local";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ContentObserver motionPreferenceObserver = new ContentObserver(main) {
        @Override
        public void onChange(boolean selfChange) {
            if (jsReady) pushMotionPreference();
        }
    };
    private final LyricOrchestrator lyricOrchestrator = new LyricOrchestrator();
    private final Messenger replyMessenger = new Messenger(new IncomingUiHandler());
    private final ExecutorService bgExecutor = Executors.newSingleThreadExecutor();

    private Messenger serviceMessenger;
    private boolean boundToService = false;
    private int pendingOnsetSensitivity = -1;
    private boolean rootInspectDone = false;
    private String cachedRootSummary = "正在检测 su / Kitsune Magisk 状态…";

    private FrameLayout rootHost;
    private WebView webView;
    private boolean jsReady = false;
    private boolean uiResumed = false;
    private int visualDemand;
    private long lastVisualGeneration = -1;
    private boolean currentPlaying = false;
    private boolean isFullscreenLyrics = false;
    private boolean isModalOpen = false;
    private String pendingLogText = "";
    private Bundle latestSnapshot = new Bundle();

    private int safeTopCss = 24;
    private int safeRightCss = 0;
    private int safeBottomCss = 16;
    private int safeLeftCss = 0;

    private final class IncomingUiHandler extends Handler {
        IncomingUiHandler() {
            super(Looper.getMainLooper());
        }

        @Override
        public void handleMessage(Message msg) {
            if (msg.what == ReceiverService.MSG_SNAPSHOT) {
                applySnapshot(msg.getData());
            } else if (msg.what == ReceiverService.MSG_MEDIA_EVENT) {
                applyMediaEvent(msg.getData());
            } else if (msg.what == ReceiverService.MSG_AUDIO_LEVEL) {
                Bundle b = msg.getData();
                if (b != null) {
                    lyricOrchestrator.updateAudioEnergy(
                            b.getFloat("peak", 0f),
                            b.getLong("position_ms", 0L),
                            b.getInt("spectrum", 0)
                    );
                }
            } else if (msg.what == ReceiverService.MSG_AUDIO_ONSETS) {
                Bundle data = msg.getData();
                long epoch = data.getLong("visual_generation", 0);
                if (!uiResumed || epoch < lastVisualGeneration) return;
                lastVisualGeneration = epoch;
                lyricOrchestrator.updateAudioSample(data.getString("events"), data.getInt("spectrum", -1),
                        data.containsKey("spectrum"), data.getLong("position_ms", 0), epoch);
            } else if (msg.what == ReceiverService.MSG_LOG_TEXT) {
                Bundle b = msg.getData();
                pendingLogText = b != null ? b.getString("logs", "暂无运行日志") : "暂无运行日志";
                evalApp("showLogSheet(" + JSONObject.quote(pendingLogText) + ")");
            }
        }
    }

    private final ServiceConnection conn = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            serviceMessenger = new Messenger(binder);
            lastVisualGeneration = -1;
            eval("window.SoundFlowStage?.resetAudioGeneration()");
            boundToService = true;
            sendToService(ReceiverService.MSG_REGISTER);
            sendUiVisibilityToService(uiResumed);
            if (pendingOnsetSensitivity >= 0) {
                sendOnsetSensitivityToService(pendingOnsetSensitivity);
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            boundToService = false;
            serviceMessenger = null;
        }
    };

    private final class JsBridge {
        @JavascriptInterface
        public void onUiState(String name, boolean value) {
            if ("tuner".equals(name) || "fullscreen".equals(name) || "ripple".equals(name)) {
                Log.i("SoundFlowControls", name + "=" + value);
            }
        }

        @JavascriptInterface
        public void setVisualDemand(boolean motion, boolean ripples) {
            main.post(() -> {
                int next = (motion ? 1 : 0) | (ripples ? 2 : 0);
                if (next == visualDemand) return;
                visualDemand = next;
                sendUiVisibilityToService(uiResumed);
            });
        }

        @JavascriptInterface
        public void onRenderStats(String stats) {
            if (stats != null && stats.length() < 300) Log.i("SoundFlowFrames", stats);
        }

        @JavascriptInterface
        public void requestDlnaVisualization() {
            main.post(() -> {
                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                    showJsToast("DLNA 节奏分析已授权，仅分析本应用播放的音频");
                } else {
                    requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 102);
                }
            });
        }

        @JavascriptInterface
        public void onJsReady() {
            main.post(() -> {
                jsReady = true;
                eval("window.SoundFlowStage?.resetAudioGeneration()");
                lyricOrchestrator.markPageReady();
                pushInsetsAndTheme();
                pushMotionPreference();
                pushSnapshotToJs(latestSnapshot);
                refreshRootAndAntiKillStatus();
                handleLyricVisibility(
                        latestSnapshot.getBoolean("playing", false),
                        latestSnapshot.getBoolean("session_active", false),
                        latestSnapshot
                );
            });
        }

        @JavascriptInterface
        public void toggleReceiver(String name, int deviceType, boolean enableDlna, boolean autoOnlineLyrics) {
            main.post(() -> {
                performTickHaptic();
                boolean enabled = latestSnapshot.getBoolean("enabled", false);
                if (enabled) {
                    startServiceCompat(new Intent(MainActivity.this, ReceiverService.class)
                            .setAction(ReceiverService.ACTION_STOP));
                } else {
                    Intent it = new Intent(MainActivity.this, ReceiverService.class)
                            .setAction(ReceiverService.ACTION_START)
                            .putExtra("name", sanitizeDeviceName(name))
                            .putExtra("device_type", deviceType)
                            .putExtra("enable_dlna", enableDlna)
                            .putExtra("auto_online_lyrics", autoOnlineLyrics);
                    startServiceCompat(it);
                }
            });
        }

        @JavascriptInterface
        public void applyConfig(String name, int deviceType, boolean enableDlna, boolean autoOnlineLyrics) {
            main.post(() -> {
                String safeName = sanitizeDeviceName(name);
                lyricOrchestrator.onOnlineLookupSettingChanged(autoOnlineLyrics);
                if (boundToService && serviceMessenger != null) {
                    Message msg = Message.obtain(null, ReceiverService.MSG_UPDATE_CONFIG);
                    Bundle b = new Bundle();
                    b.putString("name", safeName);
                    b.putInt("device_type", deviceType);
                    b.putBoolean("enable_dlna", enableDlna);
                    b.putBoolean("auto_online_lyrics", autoOnlineLyrics);
                    msg.setData(b);
                    try {
                        serviceMessenger.send(msg);
                    } catch (RemoteException ignored) {
                        startServiceCompat(new Intent(MainActivity.this, ReceiverService.class)
                                .setAction(ReceiverService.ACTION_UPDATE_CONFIG)
                                .putExtra("name", safeName)
                                .putExtra("device_type", deviceType)
                                .putExtra("enable_dlna", enableDlna)
                                .putExtra("auto_online_lyrics", autoOnlineLyrics));
                    }
                } else {
                    Intent it = new Intent(MainActivity.this, ReceiverService.class)
                            .setAction(ReceiverService.ACTION_UPDATE_CONFIG)
                            .putExtra("name", safeName)
                            .putExtra("device_type", deviceType)
                            .putExtra("enable_dlna", enableDlna)
                            .putExtra("auto_online_lyrics", autoOnlineLyrics);
                    startServiceCompat(it);
                }
                refreshRootAndAntiKillStatus(false);
            });
        }

        @JavascriptInterface
        public void controlAction(String cmd) {
            main.post(() -> {
                performTickHaptic();
                String action;
                if ("prev".equals(cmd)) action = ReceiverService.ACTION_PREV;
                else if ("pause".equals(cmd)) action = ReceiverService.ACTION_PAUSE;
                else if ("resume".equals(cmd)) action = ReceiverService.ACTION_RESUME;
                else if ("next".equals(cmd)) action = ReceiverService.ACTION_NEXT;
                else return;
                if (boundToService && serviceMessenger != null) {
                    Message msg = Message.obtain(null, ReceiverService.MSG_CONTROL_ACTION);
                    Bundle b = new Bundle();
                    b.putString("cmd", cmd);
                    msg.setData(b);
                    try {
                        serviceMessenger.send(msg);
                        return;
                    } catch (RemoteException ignored) {}
                }
                startServiceCompat(new Intent(MainActivity.this, ReceiverService.class).setAction(action));
            });
        }

        @JavascriptInterface
        public void setVolume(int percent) {
            main.post(() -> {
                int clamped = Math.max(0, Math.min(100, percent));
                if (boundToService && serviceMessenger != null) {
                    Message msg = Message.obtain(null, ReceiverService.MSG_SET_VOLUME, clamped, 0);
                    try {
                        serviceMessenger.send(msg);
                        return;
                    } catch (RemoteException ignored) {}
                }
                Intent it = new Intent(MainActivity.this, ReceiverService.class)
                        .setAction(ReceiverService.ACTION_VOLUME)
                        .putExtra("value", clamped);
                startServiceCompat(it);
            });
        }

        @JavascriptInterface
        public void setAudioSyncOffset(int millis) {
            main.post(() -> {
                int clamped = Math.max(-400, Math.min(400, millis));
                if (boundToService && serviceMessenger != null) {
                    Message msg = Message.obtain(null, ReceiverService.MSG_SET_AUDIO_SYNC_OFFSET, clamped, 0);
                    try {
                        serviceMessenger.send(msg);
                        return;
                    } catch (RemoteException ignored) {}
                }
                startServiceCompat(new Intent(MainActivity.this, ReceiverService.class)
                        .setAction(ReceiverService.ACTION_AUDIO_SYNC_OFFSET)
                        .putExtra("value", clamped));
            });
        }

        @JavascriptInterface
        public void setRippleSensitivity(int value) {
            int clamped = Math.max(0, Math.min(100, value));
            main.post(() -> {
                pendingOnsetSensitivity = clamped;
                sendOnsetSensitivityToService(clamped);
            });
        }

        @JavascriptInterface
        public void releasePortConflict() {
            bgExecutor.execute(() -> {
                RootHardeningManager.stopPortConflictReceiver();
                main.postDelayed(() -> {
                    Intent it = new Intent(MainActivity.this, ReceiverService.class)
                            .setAction(ReceiverService.ACTION_START);
                    startServiceCompat(it);
                }, 420);
            });
        }

        @JavascriptInterface
        public void openBatterySettings() {
            main.post(MainActivity.this::openBatteryWhitelistUi);
        }

        @JavascriptInterface
        public void openAutoStartSettings() {
            main.post(MainActivity.this::openAutoStartUi);
        }

        @JavascriptInterface
        public void runRootTask(String kind) {
            main.post(() -> showJsToast("正在执行 Root 配置…"));
            bgExecutor.execute(() -> {
                RootHardeningManager.CommandResult res;
                if ("hot".equals(kind)) {
                    res = RootHardeningManager.applyHotProtection(MainActivity.this);
                } else if ("install".equals(kind)) {
                    res = RootHardeningManager.installMagiskModule(MainActivity.this);
                } else {
                    res = RootHardeningManager.uninstallMagiskModule();
                }
                main.post(() -> {
                    String msg = res.ok
                            ? ("install".equals(kind) ? "Magisk 系统特权固化成功（重启后完全生效）" : "操作成功")
                            : ("操作失败：" + res.output);
                    showJsToast(msg);
                    refreshRootAndAntiKillStatus(true);
                });
            });
        }

        @JavascriptInterface
        public void fetchLogs() {
            main.post(() -> sendToService(ReceiverService.MSG_FETCH_LOGS));
        }

        @JavascriptInterface
        public void exportLogs(String logText) {
            main.post(() -> {
                pendingLogText = logText == null ? "" : logText;
                try {
                    startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT)
                            .setType("text/plain")
                            .addCategory(Intent.CATEGORY_OPENABLE)
                            .putExtra(Intent.EXTRA_TITLE, "soundflow_diagnostics.txt"), 901);
                } catch (Throwable e) {
                    showJsToast("无可用文件管理器");
                }
            });
        }

        @JavascriptInterface
        public void importLocalLrc() {
            main.post(() -> {
                try {
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                            .setType("*/*")
                            .addCategory(Intent.CATEGORY_OPENABLE), 902);
                } catch (Throwable e) {
                    showJsToast("无可用文件选择器");
                }
            });
        }

        @JavascriptInterface
        public void clearLyricCache() {
            main.post(() -> {
                int removed = lyricOrchestrator.clearOfflineCache();
                refreshRootAndAntiKillStatus();
                showJsToast("已清理 " + removed + " 个本地歌词缓存");
            });
        }

        @JavascriptInterface
        public void fullscreenChanged(boolean fs) {
            isFullscreenLyrics = fs;
        }

        @JavascriptInterface
        public void modalChanged(boolean open) {
            isModalOpen = open;
        }

        @JavascriptInterface
        public void themeApplied(boolean isDark) {
            main.post(() -> {
                if (rootHost != null) {
                    rootHost.setBackgroundColor(isDark ? Color.rgb(18, 18, 22) : Color.rgb(242, 242, 247));
                }
                updateSystemBarsAppearanceForDark(isDark);
            });
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        configureEdgeToEdgeWindow();

        rootHost = new FrameLayout(this);
        rootHost.setBackgroundColor(isSystemNight() ? Color.rgb(18, 18, 22) : Color.rgb(242, 242, 247));
        rootHost.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets sb = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            float density = Math.max(1f, getResources().getDisplayMetrics().density);
            safeTopCss = Math.round(sb.top / density);
            safeRightCss = Math.round(sb.right / density);
            safeBottomCss = Math.round(sb.bottom / density);
            safeLeftCss = Math.round(sb.left / density);
            pushInsetsAndTheme();
            return WindowInsets.CONSUMED;
        });

        webView = new WebView(this);
        setupWebView(webView);
        lyricOrchestrator.attachContext(this);
        lyricOrchestrator.attachWebView(webView);
        rootHost.addView(webView, new FrameLayout.LayoutParams(-1, -1));
        setContentView(rootHost);
        updateSystemBarsAppearance();

        requestNeededPermissions();
        refreshRootAndAntiKillStatus();
        bindService(new Intent(this, ReceiverService.class), conn, Context.BIND_AUTO_CREATE);
    }

    private void configureEdgeToEdgeWindow() {
        Window w = getWindow();
        w.setDecorFitsSystemWindows(false);
        w.setStatusBarColor(Color.TRANSPARENT);
        w.setNavigationBarColor(Color.TRANSPARENT);
    }

    private void updateSystemBarsAppearance() {
        updateSystemBarsAppearanceForDark(isSystemNight());
    }

    private void updateSystemBarsAppearanceForDark(boolean dark) {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                WindowInsetsController ctrl = getWindow().getInsetsController();
                if (ctrl != null) {
                    int mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                    ctrl.setSystemBarsAppearance(dark ? 0 : mask, mask);
                }
            } catch (Throwable ignored) {}
        }
    }

    private boolean isSystemNight() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
    }

    @SuppressWarnings("SetJavaScriptEnabled")
    private void setupWebView(WebView web) {
        web.setBackgroundColor(Color.TRANSPARENT);
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        web.setVerticalScrollBarEnabled(false);
        web.setHorizontalScrollBarEnabled(false);
        WebSettings ws = web.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setMediaPlaybackRequiresUserGesture(false);
        ws.setAllowFileAccess(false);
        ws.setAllowContentAccess(false);
        web.addJavascriptInterface(new JsBridge(), "SoundFlowBridge");

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage cm) {
                if (cm.messageLevel() == ConsoleMessage.MessageLevel.ERROR
                        || cm.messageLevel() == ConsoleMessage.MessageLevel.WARNING) {
                    Log.w("SoundFlowWeb", cm.sourceId() + ":" + cm.lineNumber() + " " + cm.message());
                }
                return super.onConsoleMessage(cm);
            }
        });

        web.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if (isLocalWebOrigin(uri)) {
                    String path = uri.getPath();
                    if (path == null || "/".equals(path)) path = "/index.html";
                    if (path.contains("..") || path.contains("\\")) {
                        return new WebResourceResponse("text/plain", "utf-8", 403, "Forbidden", null, new ByteArrayInputStream(new byte[0]));
                    }
                    String assetPath = "ui" + path;
                    try {
                        InputStream is = getAssets().open(assetPath);
                        String mime = path.endsWith(".mjs") || path.endsWith(".js") ? "application/javascript"
                                : path.endsWith(".css") ? "text/css"
                                : "text/html";
                        return new WebResourceResponse(mime, "utf-8", is);
                    } catch (Throwable ex) {
                        Log.w(TAG, "Asset missing: " + assetPath, ex);
                        return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found", null, new ByteArrayInputStream(new byte[0]));
                    }
                }
                if (uri != null && "data".equalsIgnoreCase(uri.getScheme())) return null;
                return new WebResourceResponse("text/plain", "utf-8", 403, "Offline Only", null,
                        new ByteArrayInputStream(new byte[0]));
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                return !request.isForMainFrame() || !isLocalWebOrigin(uri)
                        || !"/index.html".equals(uri.getPath());
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                pushInsetsAndTheme();
            }
        });

        web.loadUrl("https://" + APP_HOST + "/index.html");
    }

    private static boolean isLocalWebOrigin(Uri uri) {
        return uri != null && "https".equalsIgnoreCase(uri.getScheme())
                && APP_HOST.equals(uri.getHost()) && uri.getUserInfo() == null
                && (uri.getPort() == -1 || uri.getPort() == 443);
    }

    private void pushInsetsAndTheme() {
        evalApp("setSafeInsets("
                + safeTopCss + "," + safeRightCss + "," + safeBottomCss + "," + safeLeftCss + ","
                + isSystemNight() + ")");
    }

    private void pushMotionPreference() {
        boolean reduceMotion = Settings.Global.getFloat(getContentResolver(),
                Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f;
        evalApp("setReducedMotion(" + reduceMotion + ")");
    }

    private void applySnapshot(Bundle b) {
        if (b == null) return;
        if (!b.containsKey("lyrics_lrc") && b.getBoolean("session_active")
                && latestSnapshot.getLong("media_seq", -1) == b.getLong("media_seq")) {
            for (String key : new String[]{"lyrics_lrc", "lyric_line", "artwork"}) {
                if (latestSnapshot.containsKey(key)) b.putString(key, latestSnapshot.getString(key));
            }
        }
        lastVisualGeneration = Math.max(lastVisualGeneration, b.getLong("visual_generation"));
        latestSnapshot = b;
        pushSnapshotToJs(b);
        handleLyricVisibility(b.getBoolean("playing", false), b.getBoolean("session_active", false), b);
    }

    private void applyMediaEvent(Bundle b) {
        if (b == null) return;
        lastVisualGeneration = Math.max(lastVisualGeneration, b.getLong("visual_generation"));
        latestSnapshot = b;
        handleLyricVisibility(b.getBoolean("playing", false), b.getBoolean("session_active", false), b);
    }

    private void pushSnapshotToJs(Bundle b) {
        if (b == null || !jsReady) return;
        try {
            JSONObject obj = new JSONObject()
                    .put("enabled", b.getBoolean("enabled", false))
                    .put("miplay_running", b.getBoolean("miplay_running", false))
                    .put("dlna_running", b.getBoolean("dlna_running", false))
                    .put("playing", b.getBoolean("playing", false))
                    .put("session_active", b.getBoolean("session_active", false))
                    .put("status", b.getString("status", "接收已停止"))
                    .put("endpoint", b.getString("endpoint", ""))
                    .put("format", b.getString("format", ""))
                    .put("source_name", b.getString("source_name", ""))
                    .put("last_error", b.getString("last_error", ""))
                    .put("device_name", b.getString("device_name", "SoundFlow-Pad"))
                    .put("device_type", b.getInt("device_type", 4))
                    .put("enable_dlna", b.getBoolean("enable_dlna", true))
                    .put("auto_online_lyrics", b.getBoolean("auto_online_lyrics", false))
                    .put("volume", b.getInt("volume", 45))
                    .put("audio_sync_offset_ms", b.getInt("audio_sync_offset_ms", 0));
            evalApp("applySnapshot(" + JSONObject.quote(obj.toString()) + ")");
        } catch (Throwable ignored) {}
    }

    private void handleLyricVisibility(boolean playing, boolean session, Bundle b) {
        currentPlaying = playing;
        if (session) {
            eval("window.SoundFlowStage && window.SoundFlowStage.setVisibility(true)");
            lyricOrchestrator.setPlayingState(playing);
            lyricOrchestrator.onMediaUpdate(
                    b.getLong("media_seq", 0),
                    b.getString("title", ""),
                    b.getString("artist", ""),
                    b.getString("album", ""),
                    b.getString("lyrics_lrc", ""),
                    b.getString("lyric_line", ""),
                    b.getString("artwork", ""),
                    b.getString("format", ""),
                    b.getString("active_protocol", "xiaomi"),
                    b.getLong("duration_ms", 0),
                    b.getLong("position_ms", 0),
                    b.getBoolean("auto_online_lyrics", false)
            );
        } else {
            lyricOrchestrator.clearStage();
            eval("window.SoundFlowStage && window.SoundFlowStage.setVisibility(false)");
        }
    }

    private void refreshRootAndAntiKillStatus() {
        refreshRootAndAntiKillStatus(false);
    }

    private void refreshRootAndAntiKillStatus(boolean forceRootProbe) {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        boolean ignoring = pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
        bgExecutor.execute(() -> {
            int cachedLyrics = lyricOrchestrator.getOfflineCacheCount();
            if (!rootInspectDone || forceRootProbe) {
                RootHardeningManager.Status st = RootHardeningManager.inspect(this);
                cachedRootSummary = st.summary;
                rootInspectDone = true;
            }
            final String summary = cachedRootSummary;
            main.post(() -> evalApp("updateGuardStatus(" + ignoring + "," + JSONObject.quote(summary) + "," + cachedLyrics + ")"));
        });
    }

    private void openBatteryWhitelistUi() {
        try {
            startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        } catch (Throwable ignored) {
            try {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            } catch (Throwable ignored2) {}
        }
    }

    private void openAutoStartUi() {
        Intent[] candidates = new Intent[]{
                new Intent().setComponent(new ComponentName("com.miui.securitycenter",
                        "com.miui.permcenter.autostart.AutoStartManagementActivity")),
                new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName()))
        };
        for (Intent it : candidates) {
            try {
                startActivity(it);
                return;
            } catch (Throwable ignored) {}
        }
    }

    private String sanitizeDeviceName(String raw) {
        String s = raw == null ? "" : raw.trim();
        if (s.isEmpty() || s.codePointCount(0, s.length()) > 32) return "SoundFlow-Pad";
        return s;
    }

    private void startServiceCompat(Intent it) {
        try {
            if (ReceiverService.ACTION_START.equals(it.getAction())) startForegroundService(it);
            else startService(it);
        } catch (Throwable t) {
            showJsToast("服务控制失败：" + t.getMessage());
        }
    }

    private void sendToService(int what) {
        if (!boundToService || serviceMessenger == null) return;
        Message msg = Message.obtain(null, what);
        msg.replyTo = replyMessenger;
        try { serviceMessenger.send(msg); } catch (RemoteException ignored) {}
    }

    private void sendUiVisibilityToService(boolean visible) {
        if (!boundToService || serviceMessenger == null) return;
        Message msg = Message.obtain(null, ReceiverService.MSG_UI_VISIBILITY);
        msg.arg1 = visible ? 1 : 0;
        msg.arg2 = visualDemand;
        try { serviceMessenger.send(msg); } catch (RemoteException ignored) {}
    }

    private void sendOnsetSensitivityToService(int value) {
        if (!boundToService || serviceMessenger == null) return;
        Message msg = Message.obtain(null, ReceiverService.MSG_SET_ONSET_SENSITIVITY);
        msg.arg1 = Math.max(0, Math.min(100, value));
        try { serviceMessenger.send(msg); } catch (RemoteException ignored) {}
    }

    private void showJsToast(String msg) {
        evalApp("showToast(" + JSONObject.quote(msg == null ? "" : msg) + ")");
    }

    private void evalApp(String call) {
        if (!jsReady || webView == null) return;
        eval("window.SoundFlowApp && window.SoundFlowApp." + call);
    }

    private void eval(String js) {
        if (webView == null) return;
        main.post(() -> {
            if (webView != null) webView.evaluateJavascript(js, null);
        });
    }

    private void performTickHaptic() {
        if (rootHost != null) {
            rootHost.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        }
    }

    private void requestNeededPermissions() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 101);
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (rootHost != null) {
            rootHost.setBackgroundColor(isSystemNight() ? Color.rgb(18, 18, 22) : Color.rgb(242, 242, 247));
            rootHost.requestLayout();
            rootHost.requestApplyInsets();
        }
        if (webView != null) {
            webView.requestLayout();
        }
        updateSystemBarsAppearance();
        pushInsetsAndTheme();
        eval("window.SoundFlowStage && window.SoundFlowStage.onViewportChanged && window.SoundFlowStage.onViewportChanged()");
        main.postDelayed(() -> {
            if (isDestroyed()) return;
            if (rootHost != null) {
                rootHost.requestLayout();
                rootHost.requestApplyInsets();
            }
            if (webView != null) {
                webView.requestLayout();
            }
            updateSystemBarsAppearance();
            pushInsetsAndTheme();
            eval("window.SoundFlowStage && window.SoundFlowStage.onViewportChanged && window.SoundFlowStage.onViewportChanged()");
        }, 240);
    }

    @Override
    public void onBackPressed() {
        if (isFullscreenLyrics || isModalOpen) {
            evalApp("handleBack()");
            return;
        }
        super.onBackPressed();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == 901 && res == RESULT_OK && data != null && data.getData() != null) {
            try (OutputStream out = getContentResolver().openOutputStream(data.getData())) {
                if (out != null) out.write(pendingLogText.getBytes(StandardCharsets.UTF_8));
                showJsToast("诊断日志已导出");
            } catch (Throwable e) {
                showJsToast("导出失败：" + e.getMessage());
            }
        } else if (req == 902 && res == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            bgExecutor.execute(() -> {
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) return;
                    ByteArrayOutputStream bos = new ByteArrayOutputStream();
                    byte[] buf = new byte[4096];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        bos.write(buf, 0, n);
                        if (bos.size() > 512 * 1024) break;
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
                    final String finalLrc = text;
                    main.post(() -> {
                        if (lyricOrchestrator.importLocalLrcForCurrentTrack(finalLrc)) {
                            refreshRootAndAntiKillStatus();
                            showJsToast("已导入并离线绑定当前曲目 LRC 歌词");
                        } else {
                            showJsToast("未识别到有效歌词内容");
                        }
                    });
                } catch (Throwable e) {
                    main.post(() -> showJsToast("读取歌词失败：" + e.getMessage()));
                }
            });
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        getContentResolver().registerContentObserver(
                Settings.Global.getUriFor(Settings.Global.ANIMATOR_DURATION_SCALE),
                false, motionPreferenceObserver);
        uiResumed = true;
        lyricOrchestrator.setUiVisible(true);
        sendUiVisibilityToService(true);
        sendToService(ReceiverService.MSG_REGISTER);
        refreshRootAndAntiKillStatus();
        if (jsReady) {
            lyricOrchestrator.setPlayingState(currentPlaying);
            pushInsetsAndTheme();
            pushMotionPreference();
        }
    }

    @Override
    protected void onPause() {
        getContentResolver().unregisterContentObserver(motionPreferenceObserver);
        uiResumed = false;
        sendUiVisibilityToService(false);
        lyricOrchestrator.setUiVisible(false);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacksAndMessages(null);
        bgExecutor.shutdownNow();
        lyricOrchestrator.destroy();
        if (boundToService) {
            sendToService(ReceiverService.MSG_UNREGISTER);
            unbindService(conn);
            boundToService = false;
        }
        if (webView != null) webView.destroy();
        super.onDestroy();
    }
}
