package io.soundflow.sender;

import android.content.Context;
import android.media.AudioRecord;
import android.os.Binder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Parcelable;
import android.os.Process;
import android.os.RemoteException;

import java.io.OutputStream;
import java.lang.reflect.Method;

/**
 * Shizuku UserService running under UID 2000 (shell) or UID 0 (root).
 * Registers a privileged AudioPolicy loopback sink using com.android.shell's
 * AttributionSource and streams raw stereo PCM back to the main app over an
 * anonymous ParcelFileDescriptor pipe.
 */
public final class ShizukuAudioCaptureService extends Binder {
    public static final String DESCRIPTOR = "io.soundflow.sender.IShizukuAudioCapture";
    public static final int TX_OPEN_CAPTURE = FIRST_CALL_TRANSACTION + 1;
    public static final int TX_STOP_CAPTURE = FIRST_CALL_TRANSACTION + 2;
    public static final int TX_DESTROY = 16777114; // Shizuku UserService standard destroy code

    private final Object lock = new Object();
    private volatile boolean active = false;
    private PrivilegedAudioPolicyHelper.ActivePolicySession activeSession;
    private Thread pumpThread;
    private Context cachedShellContext;

    public ShizukuAudioCaptureService() {
        attachInterface(null, DESCRIPTOR);
    }

    private Context resolveShellContext() throws Exception {
        if (cachedShellContext != null) return cachedShellContext;
        Class<?> atCls = Class.forName("android.app.ActivityThread");
        Object thread = atCls.getMethod("currentActivityThread").invoke(null);
        if (thread == null) {
            thread = atCls.getMethod("systemMain").invoke(null);
        }
        Method getSysCtx = atCls.getMethod("getSystemContext");
        Context sysCtx = (Context) getSysCtx.invoke(thread);
        if (sysCtx == null) throw new IllegalStateException("SystemContext unavailable");
        String pkg = (Process.myUid() == 0) ? "android" : "com.android.shell";
        cachedShellContext = sysCtx.createPackageContext(pkg, 0);
        return cachedShellContext;
    }

    private void stopCurrentLocked() {
        active = false;
        PrivilegedAudioPolicyHelper.ActivePolicySession sess = activeSession;
        activeSession = null;
        if (sess != null) {
            sess.close();
        }
        Thread th = pumpThread;
        pumpThread = null;
        if (th != null && th != Thread.currentThread()) {
            th.interrupt();
            try { th.join(400); } catch (InterruptedException ignored) {}
        }
    }

    private ParcelFileDescriptor openCaptureStream(
            int sampleRate, int frameSamples, int selfUid, int targetUid, boolean muteLocal) throws Exception {
        synchronized (lock) {
            stopCurrentLocked();
            Context shellCtx = resolveShellContext();
            PrivilegedAudioPolicyHelper.ActivePolicySession sess =
                    PrivilegedAudioPolicyHelper.openLoopbackSession(
                            shellCtx, sampleRate, selfUid, targetUid, muteLocal);
            ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            ParcelFileDescriptor readEnd = pipe[0];
            ParcelFileDescriptor writeEnd = pipe[1];
            try {
                sess.audioRecord.startRecording();
            } catch (Throwable t) {
                try { readEnd.close(); } catch (Throwable ignored) {}
                try { writeEnd.close(); } catch (Throwable ignored) {}
                sess.close();
                throw t;
            }

            final int frameBytes = Math.max(1920, Math.min(32768, frameSamples * 4));
            active = true;
            activeSession = sess;
            Thread th = new Thread(() -> {
                try {
                    Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
                } catch (Throwable ignored) {}
                byte[] buf = new byte[frameBytes];
                try (OutputStream out = new ParcelFileDescriptor.AutoCloseOutputStream(writeEnd)) {
                    while (active && !Thread.currentThread().isInterrupted()) {
                        int n = sess.audioRecord.read(buf, 0, buf.length, AudioRecord.READ_BLOCKING);
                        if (n <= 0) break;
                        out.write(buf, 0, n);
                    }
                } catch (Throwable ignored) {
                    // Broken pipe (EPIPE) or stop: automatically clean up AudioPolicy below.
                } finally {
                    synchronized (lock) {
                        if (activeSession == sess) {
                            active = false;
                            activeSession = null;
                        }
                    }
                    sess.close();
                }
            }, "SoundFlow-ShizukuAudioPump");
            th.setDaemon(true);
            pumpThread = th;
            th.start();
            return readEnd;
        }
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (code == TX_DESTROY) {
            synchronized (lock) {
                stopCurrentLocked();
            }
            if (reply != null) reply.writeNoException();
            System.exit(0);
            return true;
        }
        if (code == TX_OPEN_CAPTURE) {
            data.enforceInterface(DESCRIPTOR);
            int sampleRate = data.readInt();
            int frameSamples = data.readInt();
            int selfUid = data.readInt();
            int targetUid = data.readInt();
            boolean muteLocal = data.readInt() != 0;
            try {
                ParcelFileDescriptor readEnd = openCaptureStream(
                        sampleRate, frameSamples, selfUid, targetUid, muteLocal);
                reply.writeNoException();
                reply.writeInt(1);
                readEnd.writeToParcel(reply, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                try { readEnd.close(); } catch (Throwable ignored) {}
            } catch (Throwable t) {
                reply.writeException(new IllegalStateException(
                        t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName()));
            }
            return true;
        }
        if (code == TX_STOP_CAPTURE) {
            data.enforceInterface(DESCRIPTOR);
            synchronized (lock) {
                stopCurrentLocked();
            }
            if (reply != null) reply.writeNoException();
            return true;
        }
        return super.onTransact(code, data, reply, flags);
    }
}
