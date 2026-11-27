package io.soundflow.sender;

import android.app.Notification;
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
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
 * Monitors active third-party music MediaSessions & status-bar lyrics for Multi-Room Sender:
 * 1. Privileged mode (Magisk priv-app with MEDIA_CONTENT_CONTROL): reads active sessions directly without user setup.
 * 2. Standard mode (NotificationListenerService granted): reads sessions + captures Meizu/HyperOS status-bar lyrics.
 * 3. Fallback mode: dispatches hardware media key events via AudioManager.
 * Always ignores our own package (io.soundflow.receiver) to prevent self-control loops.
 */
public final class MediaSessionMonitorService extends NotificationListenerService {
    private static final String TAG = "SoundFlowMediaMon";
    private static volatile MediaSessionMonitorService activeInstance;
    private static volatile String preferredPackage = "";

    public static void setPreferredPackage(String pkg) {
        preferredPackage = pkg == null ? "" : pkg.trim();
        MediaSessionMonitorService svc = activeInstance;
        if (svc != null) new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> svc.emitActiveControllerSnapshot(true));
    }

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

    public static boolean hasPrivilegedMediaControl(Context ctx) {
        if (ctx == null) return false;
        try {
            return ctx.checkSelfPermission("android.permission.MEDIA_CONTENT_CONTROL")
                    == PackageManager.PERMISSION_GRANTED;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public static boolean isNotificationAccessGranted(Context ctx) {
        if (ctx == null) return false;
        if (hasPrivilegedMediaControl(ctx)) return true;
        try {
            String flat = Settings.Secure.getString(ctx.getContentResolver(), "enabled_notification_listeners");
            return flat != null && flat.contains(ctx.getPackageName());
        } catch (Throwable ignored) {
            return false;
        }
    }

    private static MediaController resolveBestExternalController(Context ctx) {
        MediaSessionMonitorService svc = activeInstance;
        if (svc != null) {
            MediaController c = svc.pickPrimaryController();
            if (c != null) return c;
        }
        if (ctx != null && hasPrivilegedMediaControl(ctx)) {
            try {
                MediaSessionManager msm = (MediaSessionManager) ctx.getSystemService(Context.MEDIA_SESSION_SERVICE);
                if (msm != null) {
                    List<MediaController> list = msm.getActiveSessions(null);
                    String pref = preferredPackage;
                    if (!pref.isEmpty()) {
                        for (MediaController c : list) {
                            if (c != null && pref.equals(c.getPackageName())) return c;
                        }
                        return null;
                    }
                    MediaController fallback = null;
                    for (MediaController c : list) {
                        if (c == null || ctx.getPackageName().equals(c.getPackageName())) continue;
                        PlaybackState ps = c.getPlaybackState();
                        if (ps != null && ps.getState() == PlaybackState.STATE_PLAYING) return c;
                        if (fallback == null) fallback = c;
                    }
                    return fallback;
                }
            } catch (Throwable ignored) {}
        }
        return null;
    }

    public static boolean dispatchControl(Context ctx, String action, long positionMs) {
        if (action == null || action.isEmpty()) return false;
        String norm = action.trim().toLowerCase(java.util.Locale.US);
        MediaController c = resolveBestExternalController(ctx);
        if (c != null && dispatchToController(c, norm, positionMs)) {
            return true;
        }
        if (ctx == null || !preferredPackage.isEmpty()) return false;
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return false;
        int keyCode;
        switch (norm) {
            case "pause":
                keyCode = KeyEvent.KEYCODE_MEDIA_PAUSE;
                break;
            case "resume":
                keyCode = KeyEvent.KEYCODE_MEDIA_PLAY;
                break;
            case "next":
                keyCode = KeyEvent.KEYCODE_MEDIA_NEXT;
                break;
            case "prev":
            case "previous":
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

    public static java.util.Set<String> getActivePlayingPackages(Context ctx) {
        java.util.Set<String> pkgs = new java.util.LinkedHashSet<>();
        String selfPkg = ctx != null ? ctx.getPackageName() : "";
        MediaSessionMonitorService svc = activeInstance;
        if (svc != null) {
            for (TrackedController tc : svc.tracked) {
                try {
                    if (tc == null || tc.controller == null) continue;
                    String pkg = tc.controller.getPackageName();
                    if (pkg == null || pkg.isEmpty() || pkg.equals(selfPkg)) continue;
                    PlaybackState ps = tc.controller.getPlaybackState();
                    if (ps != null && ps.getState() == PlaybackState.STATE_PLAYING) {
                        pkgs.add(pkg);
                    }
                } catch (Throwable ignored) {}
            }
        }
        if (ctx != null && hasPrivilegedMediaControl(ctx)) {
            try {
                MediaSessionManager msm = (MediaSessionManager) ctx.getSystemService(Context.MEDIA_SESSION_SERVICE);
                if (msm != null) {
                    for (MediaController c : msm.getActiveSessions(null)) {
                        if (c == null) continue;
                        String pkg = c.getPackageName();
                        if (pkg == null || pkg.isEmpty() || pkg.equals(selfPkg)) continue;
                        PlaybackState ps = c.getPlaybackState();
                        if (ps != null && ps.getState() == PlaybackState.STATE_PLAYING) {
                            pkgs.add(pkg);
                        }
                    }
                }
            } catch (Throwable ignored) {}
        }
        return pkgs;
    }

    public static void pollCurrentPosition(Context ctx) {
        MediaSessionMonitorService svc = activeInstance;
        if (svc != null) {
            svc.emitActiveControllerSnapshot(false);
            return;
        }
        MediaController c = resolveBestExternalController(ctx);
        if (c != null) emitSnapshotFromController(c, false);
    }

    @Override
    public void onListenerConnected() {
        super.onListenerConnected();
        activeInstance = this;
        try {
            sessionManager = (MediaSessionManager) getSystemService(Context.MEDIA_SESSION_SERVICE);
            ComponentName cn = new ComponentName(this, MediaSessionMonitorService.class);
            if (sessionManager != null) {
                sessionManager.addOnActiveSessionsChangedListener(sessionsListener, cn);
                rebindControllers(sessionManager.getActiveSessions(cn));
            }
        } catch (Throwable e) {
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
        String pref = preferredPackage;
        if (!pref.isEmpty() && !pref.equals(pkg)) return;
        String lyricLine = extractStatusBarLyric(sbn.getNotification());
        if (lyricLine != null && !lyricLine.isEmpty()) {
            MultiRoomManager.getInstance().onExternalLyricLine(pkg, lyricLine);
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
            try { sessionManager.removeOnActiveSessionsChangedListener(sessionsListener); } catch (Throwable ignored) {}
        }
        for (TrackedController tc : tracked) {
            try { tc.controller.unregisterCallback(tc.callback); } catch (Throwable ignored) {}
        }
        tracked.clear();
    }

    private void rebindControllers(List<MediaController> controllers) {
        for (TrackedController tc : tracked) {
            try { tc.controller.unregisterCallback(tc.callback); } catch (Throwable ignored) {}
        }
        tracked.clear();
        if (controllers == null) return;
        for (MediaController c : controllers) {
            if (c == null || getPackageName().equals(c.getPackageName())) continue;
            MediaController.Callback cb = new MediaController.Callback() {
                @Override
                public void onMetadataChanged(MediaMetadata metadata) {
                    emitSnapshotFromController(c, true);
                }

                @Override
                public void onPlaybackStateChanged(PlaybackState state) {
                    emitSnapshotFromController(c, true);
                }
            };
            try {
                c.registerCallback(cb);
                tracked.add(new TrackedController(c, cb));
            } catch (Throwable ignored) {}
        }
        emitActiveControllerSnapshot(true);
    }

    private MediaController pickPrimaryController() {
        String pref = preferredPackage;
        if (!pref.isEmpty()) {
            for (TrackedController tc : tracked) {
                if (pref.equals(tc.controller.getPackageName())) return tc.controller;
            }
            return null;
        }
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

    private static boolean dispatchToController(MediaController c, String action, long positionMs) {
        MediaController.TransportControls tc = c.getTransportControls();
        if (tc == null) return false;
        switch (action) {
            case "pause":
                tc.pause();
                return true;
            case "resume":
                tc.play();
                return true;
            case "next":
                tc.skipToNext();
                return true;
            case "prev":
            case "previous":
                tc.skipToPrevious();
                return true;
            case "seek":
                tc.seekTo(Math.max(0L, positionMs));
                return true;
            default:
                return false;
        }
    }

    private void emitActiveControllerSnapshot(boolean forceMetadata) {
        MediaController c = pickPrimaryController();
        if (c != null) {
            emitSnapshotFromController(c, forceMetadata);
        } else {
            String selected = preferredPackage;
            if (!selected.isEmpty()) {
                MultiRoomManager.getInstance().onSystemMediaUpdate(selected, "", "", "", 0L, 0L, true, false);
            }
        }
    }

    private static void emitSnapshotFromController(MediaController c, boolean forceMetadata) {
        String selected = preferredPackage;
        if (c == null || !selected.isEmpty() && !selected.equals(c.getPackageName())) return;
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
                c.getPackageName(), title, artist, album, durationMs, posMs, paused, forceMetadata);
    }

    private static String safeStr(String s) {
        return s == null ? "" : s.trim();
    }
}
