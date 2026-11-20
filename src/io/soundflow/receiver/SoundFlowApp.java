package io.soundflow.receiver;

import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationManager;

/** Application entry point creating the shared foreground notification channel. */
public final class SoundFlowApp extends Application {
    public static final String CHANNEL_ID = "soundflow_receiver_channel";

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID,
                    "音流局域网音频接收服务",
                    NotificationManager.IMPORTANCE_LOW
            );
            ch.setDescription("保持局域网音频投送监听与播放不断流");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }
}

