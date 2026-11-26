package com.mishare.miplay;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

/**
 * 常驻前台音频与全屋妙播服务：
 * - 持有 WifiManager.MulticastLock 确保锁屏不丢 UDP 5355 mDNS 组播
 * - SS-11: 登记 MediaProjection.Callback 监听系统撤销事件，撤销或服务销毁时立即释放令牌与采集并同步 UI
 */
public class MiPlayForegroundService extends Service {
    public static final String ACTION_START = "com.mishare.miplay.ACTION_START";
    public static final String ACTION_ATTACH_PROJECTION = "com.mishare.miplay.ACTION_ATTACH_PROJECTION";
    public static final String EXTRA_RESULT_CODE = "result_code";
    public static final String EXTRA_RESULT_DATA = "result_data";
    private static final String CHANNEL_ID = "miplay_multiroom_channel";

    private WifiManager.MulticastLock multicastLock;
    private PowerManager.WakeLock wakeLock;
    private MediaProjection activeProjection;
    private MediaProjection.Callback projectionCallback;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        acquireLocks();
        MultiRoomManager.getInstance().ensureStartedAsync(getApplicationContext());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean isProjection = intent != null && ACTION_ATTACH_PROJECTION.equals(intent.getAction());
        Notification notif = buildNotification(isProjection ? "全屋妙播 · 手机全局抓音运行中" : "全屋妙播服务运行中");

        if (Build.VERSION.SDK_INT >= 34) {
            int fgType = isProjection
                    ? (ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
                    : ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
            startForeground(1001, notif, fgType);
        } else {
            startForeground(1001, notif);
        }

        if (isProjection) {
            int code = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED);
            Intent data = intent.getParcelableExtra(EXTRA_RESULT_DATA);
            if (code == Activity.RESULT_OK && data != null) {
                MediaProjectionManager mpm = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
                if (mpm != null) {
                    releaseProjection(false);
                    MediaProjection mp = mpm.getMediaProjection(code, data);
                    if (mp != null) {
                        activeProjection = mp;
                        projectionCallback = new MediaProjection.Callback() {
                            @Override
                            public void onStop() {
                                if (activeProjection == mp) releaseProjection(true);
                            }
                        };
                        mp.registerCallback(projectionCallback, new Handler(Looper.getMainLooper()));
                        MultiRoomManager.getInstance().onMediaProjectionGranted(mp);
                    } else {
                        MultiRoomManager.getInstance().onMediaProjectionDenied();
                    }
                }
            }
        }
        return START_STICKY;
    }

    private void releaseProjection(boolean notifyRevoked) {
        MediaProjection mp = activeProjection;
        MediaProjection.Callback cb = projectionCallback;
        activeProjection = null;
        projectionCallback = null;
        if (mp != null) {
            if (cb != null) {
                try { mp.unregisterCallback(cb); } catch (Exception ignored) {}
            }
            try { mp.stop(); } catch (Exception ignored) {}
        }
        if (notifyRevoked) {
            MultiRoomManager.getInstance().onMediaProjectionRevoked("系统已停止全局抓音权限，已自动切回校准测试音");
        }
    }

    @Override
    public void onDestroy() {
        releaseProjection(true);
        MultiRoomManager.getInstance().stopMultiroom();
        releaseLocks();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createNotificationChannel() {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "全屋妙播音频服务", NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
    }

    private Notification buildNotification(String text) {
        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(
                this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("MiPlay 全屋妙播")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
    }

    private void acquireLocks() {
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                multicastLock = wm.createMulticastLock("MiPlayMulticastLock");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            }
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MiPlay::MultiRoomWakeLock");
                wakeLock.acquire();
            }
        } catch (Exception ignored) {}
    }

    private void releaseLocks() {
        try { if (multicastLock != null && multicastLock.isHeld()) multicastLock.release(); } catch (Exception ignored) {}
        try { if (wakeLock != null && wakeLock.isHeld()) wakeLock.release(); } catch (Exception ignored) {}
    }
}
