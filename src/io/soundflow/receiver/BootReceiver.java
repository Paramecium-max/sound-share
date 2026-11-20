package io.soundflow.receiver;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

/** Restarts background audio receiver service after boot or package update when enabled. */
public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        SharedPreferences prefs = context.getSharedPreferences(ReceiverService.PREF, Context.MODE_PRIVATE);
        if (!prefs.getBoolean("enabled", false)) return;
        KeepAliveJobService.schedule(context);
        try {
            context.startForegroundService(new Intent(context, ReceiverService.class).setAction(ReceiverService.ACTION_START));
        } catch (Throwable t) {
            // Android 14/15 may block background FGS without battery exemption; JobScheduler & service.d handle recovery.
            Log.w(ReceiverService.TAG, "Boot FGS deferred: " + t.getMessage());
        }
    }
}

