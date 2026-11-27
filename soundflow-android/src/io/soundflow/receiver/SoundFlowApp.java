package io.soundflow.receiver;

import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationManager;

/** Application entry point creating the shared foreground notification channel. */
public final class SoundFlowApp extends Application {
    public static final String CHANNEL_ID = "soundflow_receiver_channel";
    public static final String SENDER_CHANNEL_ID = "soundflow_sender_channel";

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                NotificationChannel ch = new NotificationChannel(
                        CHANNEL_ID,
                        "音流局域网音频接收服务",
                        NotificationManager.IMPORTANCE_LOW
                );
                ch.setDescription("保持局域网音频投送监听与播放不断流");
                ch.setShowBadge(false);
                nm.createNotificationChannel(ch);
            }
            if (nm.getNotificationChannel(SENDER_CHANNEL_ID) == null) {
                NotificationChannel senderCh = new NotificationChannel(
                        SENDER_CHANNEL_ID,
                        "音流全屋多设备发送服务",
                        NotificationManager.IMPORTANCE_LOW
                );
                senderCh.setDescription("保持全屋多音箱同步音频推流与内录不断流");
                senderCh.setShowBadge(false);
                nm.createNotificationChannel(senderCh);
            }
        }
    }
}

