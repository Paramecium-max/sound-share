package io.soundflow.receiver;

import android.app.ActivityManager;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;

/** Periodic watchdog verifying that :receiver service remains alive when persistence is enabled. */
public final class KeepAliveJobService extends JobService {
    private static final int JOB_ID = 7701;

    public static void schedule(Context ctx) {
        JobScheduler js = ctx.getSystemService(JobScheduler.class);
        if (js == null) return;
        JobInfo info = new JobInfo.Builder(JOB_ID, new ComponentName(ctx, KeepAliveJobService.class))
                .setPeriodic(15 * 60 * 1000L)
                .setPersisted(true)
                .build();
        try {
            js.schedule(info);
        } catch (Throwable ignored) {}
    }

    public static void cancel(Context ctx) {
        JobScheduler js = ctx.getSystemService(JobScheduler.class);
        if (js != null) js.cancel(JOB_ID);
    }

    public static boolean isServiceAlive(Context ctx) {
        ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return false;
        for (ActivityManager.RunningServiceInfo info : am.getRunningServices(100)) {
            if (ReceiverService.class.getName().equals(info.service.getClassName())) {
                return true;
            }
        }
        return false;
    }

    @Override
    public boolean onStartJob(JobParameters params) {
        SharedPreferences prefs = getSharedPreferences(ReceiverService.PREF, MODE_PRIVATE);
        if (prefs.getBoolean("enabled", false) && !isServiceAlive(this)) {
            try {
                startForegroundService(new Intent(this, ReceiverService.class).setAction(ReceiverService.ACTION_START));
            } catch (Throwable ignored) {}
        }
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }
}

