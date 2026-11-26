package com.mishare.miplay;

import android.app.Notification;
import android.content.ComponentName;
import android.content.Context;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.Settings;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import android.util.Log;
import android.view.KeyEvent;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Android 系统媒体会话与状态栏/蓝牙歌词监听服务（可选授权，零侵入）：
 * - 已授「通知使用权」时：通过 MediaSessionManager 实时同步当前音乐 App 的歌名/歌手/专辑/时长/进度，
 *   捕获状态栏歌词（网易云/QQ音乐/酷狗/酷我等），并精准执行 Pause/Play/Next/Previous/Seek。
 * - 未授「通知使用权」时：自动降级由 AudioManager.dispatchMediaKeyEvent 兜底实现开箱即用的切歌与暂停/恢复。
 */
public class MiPlayMediaListenerService extends NotificationListenerService {
    private static final String TAG = "MiPlayMediaListener";
    private static volatile MiPlayMediaListenerService activeInstance;

    private MediaSessionManager sessionManager;
    private final CopyOnWriteArrayList<TrackedController> tracked = new CopyOnWriteArrayList<>();
    private final MediaSessionManager.OnActiveSessionsChangedListener sessionsListener = this::rebindControllers;

    private static final class TrackedController {
        final MediaController controller;
        final MediaController.Callback callback;
        TrackedController(MediaController c, MediaController.Callback cb) {
            this.controller = c;
            this.callback = cb;
        }
    }

    public static boolean isNotificationAccessGranted(Context ctx) {
        if (ctx == null) return false;
        try {
            String flat = Settings.Secure.getString(ctx.getContentResolver(), "enabled_notification_listeners");
            return flat != null && flat.contains(ctx.getPackageName());
        } catch (Exception ignored) {
            return false;
        }
    }

    public static boolean dispatchControl(Context ctx, CmdSession.RemoteControlEvent ev) {
        if (ev == null) return false;
        // 1. 若通知监听服务已绑定，优先通过活跃 MediaController 精准控制（支持 Seek 且区分 Pause/Play）
        MiPlayMediaListenerService svc = activeInstance;
        if (svc != null && svc.dispatchViaMediaSession(ev)) {
            return true;
        }
        // 2. 兜底通过 AudioManager 派发系统硬件媒体键（无需通知使用权即可控制当前音乐 App）
        if (ctx == null) return false;
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return false;
        int keyCode;
        switch (ev.action) {
            case PAUSE:
                keyCode = KeyEvent.KEYCODE_MEDIA_PAUSE;
                break;
            case RESUME:
                keyCode = KeyEvent.KEYCODE_MEDIA_PLAY;
                break;
            case NEXT:
                keyCode = KeyEvent.KEYCODE_MEDIA_NEXT;
                break;
            case PREVIOUS:
                keyCode = KeyEvent.KEYCODE_MEDIA_PREVIOUS;
                break;
            default:
                return false;
        }
        long now = SystemClock.uptimeMillis();
        am.dispatchMediaKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0));
        am.dispatchMediaKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0));
        return true;
    }

    public static void pollCurrentPosition() {
        MiPlayMediaListenerService svc = activeInstance;
        if (svc != null) svc.emitActiveControllerSnapshot(false);
    }

    @Override
    public void onListenerConnected() {
        super.onListenerConnected();
        activeInstance = this;
        try {
            sessionManager = (MediaSessionManager) getSystemService(Context.MEDIA_SESSION_SERVICE);
            ComponentName cn = new ComponentName(this, MiPlayMediaListenerService.class);
            if (sessionManager != null) {
                sessionManager.addOnActiveSessionsChangedListener(sessionsListener, cn);
                rebindControllers(sessionManager.getActiveSessions(cn));
            }
        } catch (Exception e) {
            Log.w(TAG, "Bind MediaSessionManager failed: " + e.getMessage());
        }
        MultiRoomManager.getInstance().onNotificationAccessStateChanged();
    }

    @Override
    public void onListenerDisconnected() {
        clearControllers();
        if (activeInstance == this) activeInstance = null;
        super.onListenerDisconnected();
        MultiRoomManager.getInstance().onNotificationAccessStateChanged();
    }

    @Override
    public void onDestroy() {
        clearControllers();
        if (activeInstance == this) activeInstance = null;
        super.onDestroy();
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        if (sbn == null || sbn.getNotification() == null) return;
        String pkg = sbn.getPackageName();
        if (pkg == null || pkg.equals(getPackageName())) return;
        Notification n = sbn.getNotification();
        // 兼容主流音乐 App（网易云/QQ音乐/酷狗/酷我/Apple Music 等）的魅族/小米状态栏歌词广播
        String lyricLine = extractStatusBarLyric(n);
        if (lyricLine != null && !lyricLine.isEmpty()) {
            MultiRoomManager.getInstance().onExternalLyricLine(lyricLine);
        }
    }

    private static String extractStatusBarLyric(Notification n) {
        if (n == null) return null;
        Bundle extras = n.extras;
        boolean hasLyricFlag = (n.flags & 0x01000000) != 0; // Meizu/MIUI FLAG_ALWAYS_SHOW_TICKER
        if (extras != null) {
            if (extras.getBoolean("extra.is_lyric", false) || extras.getBoolean("is_lyric", false)) {
                hasLyricFlag = true;
            }
            String explicit = extras.getString("lyric");
            if (explicit != null && !explicit.trim().isEmpty()) return explicit.trim();
        }
        if (hasLyricFlag && n.tickerText != null) {
            String t = n.tickerText.toString().trim();
            if (!t.isEmpty() && t.length() <= 160) return t;
        }
        return null;
    }

    private void clearControllers() {
        if (sessionManager != null) {
            try { sessionManager.removeOnActiveSessionsChangedListener(sessionsListener); } catch (Exception ignored) {}
        }
        for (TrackedController tc : tracked) {
            try { tc.controller.unregisterCallback(tc.callback); } catch (Exception ignored) {}
        }
        tracked.clear();
    }

    private void rebindControllers(List<MediaController> controllers) {
        for (TrackedController tc : tracked) {
            try { tc.controller.unregisterCallback(tc.callback); } catch (Exception ignored) {}
        }
        tracked.clear();
        if (controllers == null) return;
        for (MediaController c : controllers) {
            if (c == null || getPackageName().equals(c.getPackageName())) continue;
            MediaController.Callback cb = new MediaController.Callback() {
                @Override
                public void onMetadataChanged(MediaMetadata metadata) {
                    emitControllerSnapshot(c, true);
                }
                @Override
                public void onPlaybackStateChanged(PlaybackState state) {
                    emitControllerSnapshot(c, true);
                }
            };
            try {
                c.registerCallback(cb);
                tracked.add(new TrackedController(c, cb));
            } catch (Exception ignored) {}
        }
        emitActiveControllerSnapshot(true);
    }

    private MediaController pickPrimaryController() {
        MediaController fallback = null;
        for (TrackedController tc : tracked) {
            PlaybackState ps = tc.controller.getPlaybackState();
            if (ps != null && ps.getState() == PlaybackState.STATE_PLAYING) {
                return tc.controller;
            }
            if (fallback == null) fallback = tc.controller;
        }
        return fallback;
    }

    private boolean dispatchViaMediaSession(CmdSession.RemoteControlEvent ev) {
        MediaController c = pickPrimaryController();
        if (c == null) return false;
        MediaController.TransportControls tc = c.getTransportControls();
        if (tc == null) return false;
        switch (ev.action) {
            case PAUSE:
                tc.pause();
                return true;
            case RESUME:
                tc.play();
                return true;
            case NEXT:
                tc.skipToNext();
                return true;
            case PREVIOUS:
                tc.skipToPrevious();
                return true;
            case SEEK:
                tc.seekTo(Math.max(0L, ev.positionMs));
                return true;
            default:
                return false;
        }
    }

    private void emitActiveControllerSnapshot(boolean forceMetadata) {
        MediaController c = pickPrimaryController();
        if (c != null) emitControllerSnapshot(c, forceMetadata);
    }

    private void emitControllerSnapshot(MediaController c, boolean forceMetadata) {
        if (c == null) return;
        MediaMetadata meta = c.getMetadata();
        PlaybackState state = c.getPlaybackState();
        if (meta == null && state == null) return;

        String title = meta != null ? safeStr(meta.getString(MediaMetadata.METADATA_KEY_TITLE)) : "";
        String artist = meta != null ? safeStr(meta.getString(MediaMetadata.METADATA_KEY_ARTIST)) : "";
        String album = meta != null ? safeStr(meta.getString(MediaMetadata.METADATA_KEY_ALBUM)) : "";
        long durationMs = meta != null ? Math.max(0L, meta.getLong(MediaMetadata.METADATA_KEY_DURATION)) : 0L;

        long posMs = 0L;
        boolean paused = false;
        if (state != null) {
            paused = state.getState() != PlaybackState.STATE_PLAYING;
            long basePos = Math.max(0L, state.getPosition());
            if (!paused && state.getLastPositionUpdateTime() > 0L) {
                long delta = Math.max(0L, SystemClock.elapsedRealtime() - state.getLastPositionUpdateTime());
                posMs = basePos + (long) (delta * Math.max(0.0f, state.getPlaybackSpeed()));
            } else {
                posMs = basePos;
            }
            if (durationMs > 0L && posMs > durationMs) posMs = durationMs;
        }

        MultiRoomManager.getInstance().onSystemMediaUpdate(
                title, artist, album, durationMs, posMs, paused, forceMetadata);
    }

    private static String safeStr(String s) {
        return s == null ? "" : s.trim();
    }
}
