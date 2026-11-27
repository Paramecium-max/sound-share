package io.soundflow.sender;

import android.app.Activity;
import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaMetadata;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import io.soundflow.receiver.MainActivity;
import io.soundflow.receiver.R;
import io.soundflow.receiver.SoundFlowApp;

/**
 * On-demand foreground service hosting Multi-Room Audio Sender & MediaProjection capture.
 * Complies strictly with Android 14/15 (targetSdk 35) mediaProjection foreground service rules:
 * - Never requests FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION until a valid user consent token arrives.
 * - Immediately stops itself and releases locks/projection when multi-room streaming stops.
 */
public final class SenderService extends Service {
    public static final String ACTION_START = "io.soundflow.sender.ACTION_START";
    public static final String ACTION_ATTACH_PROJECTION = "io.soundflow.sender.ACTION_ATTACH_PROJECTION";
    public static final String ACTION_STOP = "io.soundflow.sender.ACTION_STOP";
    public static final String EXTRA_RESULT_CODE = "result_code";
    public static final String EXTRA_RESULT_DATA = "result_data";

    private static final int NOTICE_SENDER_ID = 2027;

    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;
    private MediaProjection activeProjection;
    private MediaProjection.Callback projectionCallback;
    private MediaSession senderMediaSession;
    private volatile boolean keepAliveAudioRunning = false;
    private Thread keepAliveAudioThread;
    private AudioTrack keepAliveSilentTrack;

    @Override
    public void onCreate() {
        super.onCreate();
        acquireLocks();
        startMediaKeepAlive();
        MultiRoomManager.getInstance().attachContext(getApplicationContext());
    }

    @Override
    @SuppressWarnings("deprecation")
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : ACTION_START;
        if (ACTION_STOP.equals(action)) {
            releaseProjection(false);
            stopMediaKeepAlive();
            MultiRoomManager.getInstance().stopNativeStreaming();
            releaseLocks();
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }
        startMediaKeepAlive();

        boolean attachProjection = ACTION_ATTACH_PROJECTION.equals(action);
        int code = attachProjection && intent != null
                ? intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
                : Activity.RESULT_CANCELED;
        Intent projData = attachProjection && intent != null
                ? intent.getParcelableExtra(EXTRA_RESULT_DATA)
                : null;
        boolean hasConsentToken = attachProjection && code == Activity.RESULT_OK && projData != null;

        boolean useProjectionType = hasConsentToken || activeProjection != null;
        String text = useProjectionType
                ? "全屋同步发送 · 系统音频内录投送中"
                : "全屋同步音频发送运行中";
        promoteForeground(text, useProjectionType);

        if (attachProjection) {
            if (hasConsentToken) {
                MediaProjectionManager mpm = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
                if (mpm != null) {
                    releaseProjection(false);
                    try {
                        MediaProjection mp = mpm.getMediaProjection(code, projData);
                        if (mp != null) {
                            activeProjection = mp;
                            projectionCallback = new MediaProjection.Callback() {
                                @Override
                                public void onStop() {
                                    if (activeProjection == mp) {
                                        releaseProjection(true);
                                        if (MultiRoomManager.getInstance().isMultiroomRunning()) {
                                            promoteForeground("全屋同步音频发送运行中", false);
                                        }
                                    }
                                }
                            };
                            mp.registerCallback(projectionCallback, new Handler(Looper.getMainLooper()));
                            MultiRoomManager.getInstance().onMediaProjectionGranted(mp);
                        } else {
                            MultiRoomManager.getInstance().onMediaProjectionDenied();
                        }
                    } catch (Throwable t) {
                        MultiRoomManager.getInstance().onMediaProjectionRevoked("系统拒绝启动音频内录：" + t.getMessage());
                    }
                }
            } else {
                MultiRoomManager.getInstance().onMediaProjectionDenied();
            }
        }
        return START_STICKY;
    }

    private void promoteForeground(String text, boolean withProjection) {
        Notification notif = buildNotification(text);
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                int fgType = withProjection
                        ? (ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
                        : ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
                startForeground(NOTICE_SENDER_ID, notif, fgType);
            } else {
                startForeground(NOTICE_SENDER_ID, notif);
            }
        } catch (Throwable ex) {
            try {
                startForeground(NOTICE_SENDER_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            } catch (Throwable ignored) {}
        }
    }

    private void releaseProjection(boolean notifyRevoked) {
        MediaProjection mp = activeProjection;
        MediaProjection.Callback cb = projectionCallback;
        activeProjection = null;
        projectionCallback = null;
        if (mp != null) {
            MultiRoomManager.getInstance().onProjectionReleased();
            if (cb != null) {
                try { mp.unregisterCallback(cb); } catch (Throwable ignored) {}
            }
            try { mp.stop(); } catch (Throwable ignored) {}
        }
        if (notifyRevoked) {
            MultiRoomManager.getInstance().onMediaProjectionRevoked("系统已结束音频内录授权，已切回校准测试音");
        }
    }

    @Override
    public void onDestroy() {
        releaseProjection(false);
        stopMediaKeepAlive();
        MultiRoomManager.getInstance().stopNativeStreaming();
        releaseLocks();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private Notification buildNotification(String text) {
        PendingIntent openPi = PendingIntent.getActivity(
                this, 10,
                new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stopPi = PendingIntent.getService(
                this, 11,
                new Intent(this, SenderService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder nb = new Notification.Builder(this, SoundFlowApp.SENDER_CHANNEL_ID)
                .setContentTitle("音流 · 全屋多设备发送")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_soundflow_notif)
                .setContentIntent(openPi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(null, "停止发送", stopPi).build());
        if (senderMediaSession != null) {
            nb.setStyle(new Notification.MediaStyle()
                    .setMediaSession(senderMediaSession.getSessionToken())
                    .setShowActionsInCompactView(0));
        }
        return nb.build();
    }

    private void startMediaKeepAlive() {
        if (senderMediaSession == null) {
            try {
                MediaSession ms = new MediaSession(this, "SoundFlowSender");
                ms.setCallback(new MediaSession.Callback() {
                    @Override
                    public void onPause() {
                        MultiRoomManager.getInstance().stopMultiroom();
                    }

                    @Override
                    public void onStop() {
                        MultiRoomManager.getInstance().stopMultiroom();
                    }
                });
                ms.setMetadata(new MediaMetadata.Builder()
                        .putString(MediaMetadata.METADATA_KEY_TITLE, "音流 · 全屋同步发送中")
                        .putString(MediaMetadata.METADATA_KEY_ARTIST, "多房间低延迟音频投送")
                        .build());
                ms.setPlaybackState(new PlaybackState.Builder()
                        .setActions(PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_STOP)
                        .setState(PlaybackState.STATE_PLAYING, 0L, 1.0f)
                        .build());
                ms.setActive(true);
                senderMediaSession = ms;
            } catch (Throwable ignored) {}
        }
        if (!keepAliveAudioRunning) {
            keepAliveAudioRunning = true;
            Thread t = new Thread(() -> {
                AudioTrack track = null;
                try {
                    // 48kHz stereo MODE_STREAM AudioTrack with non-zero gain (0.01f) writing all-zero PCM:
                    // produces 100% digital silence (and self-UID is excluded by AudioPolicy) while
                    // registering in PlaybackActivityMonitor as an active USAGE_MEDIA player so HyperOS
                    // SmartPower / GreezeManager never freezes this process in the background.
                    final int sr = 48000;
                    final int minBuf = AudioTrack.getMinBufferSize(
                            sr, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
                    final int chunkBytes = Math.max(minBuf, sr * 4 / 5); // ~200ms chunk (5 wakes/sec, <0.1% CPU)
                    byte[] silence = new byte[chunkBytes];
                    track = new AudioTrack.Builder()
                            .setAudioAttributes(new AudioAttributes.Builder()
                                    .setUsage(AudioAttributes.USAGE_MEDIA)
                                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                    .build())
                            .setAudioFormat(new AudioFormat.Builder()
                                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                    .setSampleRate(sr)
                                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                                    .build())
                            .setBufferSizeInBytes(chunkBytes * 2)
                            .setTransferMode(AudioTrack.MODE_STREAM)
                            .build();
                    if (track.getState() == AudioTrack.STATE_INITIALIZED) {
                        keepAliveSilentTrack = track;
                        track.setVolume(0.01f);
                        track.play();
                        while (keepAliveAudioRunning) {
                            int w = track.write(silence, 0, silence.length, AudioTrack.WRITE_BLOCKING);
                            if (w < 0) break;
                        }
                    }
                } catch (Throwable ignored) {
                } finally {
                    if (track != null) {
                        if (keepAliveSilentTrack == track) keepAliveSilentTrack = null;
                        try { track.stop(); } catch (Throwable ignored) {}
                        try { track.release(); } catch (Throwable ignored) {}
                    }
                }
            }, "SoundFlow-AudioKeepAlive");
            t.setDaemon(true);
            keepAliveAudioThread = t;
            t.start();
        }
    }

    private void stopMediaKeepAlive() {
        keepAliveAudioRunning = false;
        AudioTrack track = keepAliveSilentTrack;
        keepAliveSilentTrack = null;
        if (track != null) {
            try { track.pause(); } catch (Throwable ignored) {}
            try { track.flush(); } catch (Throwable ignored) {}
            try { track.stop(); } catch (Throwable ignored) {}
        }
        Thread t = keepAliveAudioThread;
        keepAliveAudioThread = null;
        if (t != null) {
            t.interrupt();
        }
        MediaSession ms = senderMediaSession;
        senderMediaSession = null;
        if (ms != null) {
            try { ms.setActive(false); } catch (Throwable ignored) {}
            try { ms.release(); } catch (Throwable ignored) {}
        }
    }

    private void acquireLocks() {
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                if (multicastLock == null) {
                    multicastLock = wm.createMulticastLock("SoundFlow:sender_mdns");
                    multicastLock.setReferenceCounted(false);
                }
                if (!multicastLock.isHeld()) multicastLock.acquire();
                if (wifiLock == null) {
                    wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "SoundFlow:sender_lowlat");
                    wifiLock.setReferenceCounted(false);
                }
                if (!wifiLock.isHeld()) wifiLock.acquire();
            }
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                if (wakeLock == null) {
                    wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SoundFlow:sender_stream");
                    wakeLock.setReferenceCounted(false);
                }
                if (!wakeLock.isHeld()) wakeLock.acquire(4 * 60 * 60 * 1000L);
            }
        } catch (Throwable ignored) {}
    }

    private void releaseLocks() {
        try { if (multicastLock != null && multicastLock.isHeld()) multicastLock.release(); } catch (Throwable ignored) {}
        try { if (wifiLock != null && wifiLock.isHeld()) wifiLock.release(); } catch (Throwable ignored) {}
        try { if (wakeLock != null && wakeLock.isHeld()) wakeLock.release(); } catch (Throwable ignored) {}
    }
}
