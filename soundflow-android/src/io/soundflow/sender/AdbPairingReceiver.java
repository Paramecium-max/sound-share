package io.soundflow.sender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;

import io.soundflow.receiver.MainActivity;
import io.soundflow.receiver.R;

import org.json.JSONObject;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Accepts the six-digit system pairing code from an inline notification reply. */
public final class AdbPairingReceiver extends BroadcastReceiver {
    private static final String CHANNEL = "soundflow.adb.pairing";
    private static final String INPUT = "wireless_adb_pairing_code";
    private static final int NOTICE = 8321;
    private static final long VALID_FOR_MS = 180_000;
    private static final ExecutorService WORK = Executors.newSingleThreadExecutor();

    public static boolean show(Context context, boolean mute) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null || !manager.areNotificationsEnabled()) return false;
        manager.createNotificationChannel(new NotificationChannel(CHANNEL, "本机无线调试配对", NotificationManager.IMPORTANCE_DEFAULT));
        Intent intent = new Intent(context, AdbPairingReceiver.class)
                .putExtra("started", SystemClock.elapsedRealtime())
                .putExtra("mute", mute);
        PendingIntent submit = PendingIntent.getBroadcast(context, NOTICE, intent,
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_ONE_SHOT);
        RemoteInput input = new RemoteInput.Builder(INPUT).setLabel("系统六位配对码").build();
        Notification.Action action = new Notification.Action.Builder(null, "输入配对码", submit)
                .addRemoteInput(input).setAllowGeneratedReplies(false).build();
        manager.notify(NOTICE, notice(context, "输入无线调试配对码",
                "保持系统配对码窗口打开，展开这条通知并选择「输入配对码」。")
                .addAction(action).setTimeoutAfter(VALID_FOR_MS).build());
        return true;
    }

    @Override public void onReceive(Context context, Intent intent) {
        long started = intent.getLongExtra("started", -1);
        long age = SystemClock.elapsedRealtime() - started;
        Bundle input = RemoteInput.getResultsFromIntent(intent);
        CharSequence value = input == null ? null : input.getCharSequence(INPUT);
        String code = value == null ? "" : value.toString().trim();
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (age < 0 || started < 0 || age > VALID_FOR_MS || !code.matches("[0-9]{6}")) {
            if (manager != null) manager.notify(NOTICE, notice(context, "配对未完成",
                    "请在音流中重新开始，输入系统当前显示的六位数字。").build());
            return;
        }
        PendingResult pending = goAsync();
        Context app = context.getApplicationContext();
        boolean mute = intent.getBooleanExtra("mute", false);
        if (manager != null) manager.notify(NOTICE, notice(app, "正在配对",
                "配对成功后将启动本机音频守护进程。").build());
        WORK.execute(() -> {
            try {
                MultiRoomManager sender = MultiRoomManager.getInstance();
                sender.attachContext(app);
                JSONObject request = new JSONObject().put("method", "adb.pair")
                        .put("code", code).put("enableMute", mute);
                JSONObject result = new JSONObject(sender.handleRpcRequest(request.toString()));
                String error = result.optString("error", "");
                if (manager != null) manager.notify(NOTICE, error.isEmpty()
                        ? notice(app, "无线 ADB 免投屏已就绪", "本机音频守护进程已启动。").build()
                        : notice(app, "无线调试未就绪", error).build());
            } catch (Exception failure) {
                if (manager != null) manager.notify(NOTICE, notice(app, "无线调试未就绪",
                        "请回到音流，重新发起配对。").build());
            } finally {
                pending.finish();
            }
        });
    }

    private static Notification.Builder notice(Context context, String title, String text) {
        PendingIntent open = PendingIntent.getActivity(context, NOTICE,
                new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(context, CHANNEL).setSmallIcon(R.drawable.ic_soundflow_notif)
                .setContentTitle(title).setContentText(text).setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(open).setOnlyAlertOnce(true).setAutoCancel(true)
                .setVisibility(Notification.VISIBILITY_PRIVATE);
    }
}
