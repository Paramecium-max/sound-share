package io.soundflow.sender;

import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;

import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import rikka.shizuku.Shizuku;

/**
 * Main-process bridge to Shizuku (UID 2000) for zero-dialog AudioPolicy capture
 * on non-root devices with wireless debugging / Shizuku active.
 */
public final class ShizukuCaptureBridge {
    public static final int REQUEST_CODE_SHIZUKU = 1001;

    public interface StateListener {
        void onShizukuStateChanged();
    }

    private final AtomicBoolean listenersRegistered = new AtomicBoolean(false);
    private volatile StateListener stateListener;
    private final Object serviceLock = new Object();
    private IBinder remoteService;
    private ServiceConnection activeConnection;
    private Shizuku.UserServiceArgs activeArgs;

    public void register(StateListener listener) {
        this.stateListener = listener;
        if (!listenersRegistered.compareAndSet(false, true)) return;
        try {
            Shizuku.addBinderReceivedListenerSticky(() -> notifyState());
            Shizuku.addBinderDeadListener(() -> {
                clearRemoteService();
                notifyState();
            });
            Shizuku.addRequestPermissionResultListener((requestCode, grantResult) -> {
                if (requestCode == REQUEST_CODE_SHIZUKU) {
                    notifyState();
                }
            });
        } catch (Throwable ignored) {}
    }

    private void notifyState() {
        StateListener l = this.stateListener;
        if (l != null) {
            try { l.onShizukuStateChanged(); } catch (Throwable ignored) {}
        }
    }

    public boolean isAvailable() {
        try {
            return Shizuku.pingBinder();
        } catch (Throwable ignored) {
            return false;
        }
    }

    public boolean isGranted() {
        try {
            if (!Shizuku.pingBinder()) return false;
            if (Shizuku.isPreV11()) return false;
            return Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public boolean requestPermission() {
        try {
            if (!Shizuku.pingBinder() || Shizuku.isPreV11()) return false;
            if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) {
                notifyState();
                return true;
            }
            Shizuku.requestPermission(REQUEST_CODE_SHIZUKU);
            return true;
        } catch (Throwable ignored) {
            return false;
        }
    }

    private void clearRemoteService() {
        synchronized (serviceLock) {
            remoteService = null;
        }
    }

    private IBinder ensureRemoteService(Context appCtx) throws Exception {
        synchronized (serviceLock) {
            if (remoteService != null && remoteService.pingBinder()) {
                return remoteService;
            }
        }
        if (!isGranted()) {
            throw new SecurityException("Shizuku permission not granted");
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final IBinder[] holder = new IBinder[1];
        ComponentName cn = new ComponentName(appCtx.getPackageName(), ShizukuAudioCaptureService.class.getName());
        Shizuku.UserServiceArgs args = new Shizuku.UserServiceArgs(cn)
                .daemon(false)
                .processNameSuffix("shizuku_audio")
                .debuggable(false)
                .version(134);

        ServiceConnection conn = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                synchronized (serviceLock) {
                    remoteService = service;
                }
                holder[0] = service;
                latch.countDown();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
                clearRemoteService();
            }
        };

        synchronized (serviceLock) {
            activeArgs = args;
            activeConnection = conn;
        }
        Shizuku.bindUserService(args, conn);
        if (!latch.await(4, TimeUnit.SECONDS) || holder[0] == null) {
            throw new IllegalStateException("Shizuku 音频抓音服务绑定超时");
        }
        return holder[0];
    }

    private void sendRemoteStop(IBinder binder) {
        if (binder == null) return;
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(ShizukuAudioCaptureService.DESCRIPTOR);
            binder.transact(ShizukuAudioCaptureService.TX_STOP_CAPTURE, data, reply, 0);
            reply.readException();
        } catch (Throwable ignored) {
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    public AudioCaptureEngine.Recorder createRecorder(
            Context appCtx, int sampleRate, int frameSamples, int targetUid, boolean muteLocal) {
        try {
            IBinder binder = ensureRemoteService(appCtx);
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            final ParcelFileDescriptor readPfd;
            try {
                data.writeInterfaceToken(ShizukuAudioCaptureService.DESCRIPTOR);
                data.writeInt(sampleRate);
                data.writeInt(frameSamples);
                data.writeInt(Process.myUid());
                data.writeInt(targetUid);
                data.writeInt(muteLocal ? 1 : 0);
                binder.transact(ShizukuAudioCaptureService.TX_OPEN_CAPTURE, data, reply, 0);
                reply.readException();
                if (reply.readInt() == 0) {
                    throw new IllegalStateException("Shizuku 未返回 PCM 管道");
                }
                readPfd = ParcelFileDescriptor.CREATOR.createFromParcel(reply);
            } finally {
                data.recycle();
                reply.recycle();
            }
            if (readPfd == null) {
                throw new IllegalStateException("Shizuku PCM 管道为空");
            }
            final InputStream pipeIn = new ParcelFileDescriptor.AutoCloseInputStream(readPfd);
            final AtomicBoolean closed = new AtomicBoolean(false);
            return new AudioCaptureEngine.Recorder() {
                @Override
                public void start() {
                    // Remote AudioRecord is already streaming into the pipe.
                }

                @Override
                public int read(byte[] pcm, int offset, int length) {
                    if (closed.get()) return -1;
                    try {
                        return pipeIn.read(pcm, offset, length);
                    } catch (Throwable t) {
                        return -1;
                    }
                }

                @Override
                public void stop() {
                    if (closed.compareAndSet(false, true)) {
                        try { pipeIn.close(); } catch (Throwable ignored) {}
                        sendRemoteStop(binder);
                    }
                }

                @Override
                public void release() {
                    stop();
                }
            };
        } catch (RuntimeException re) {
            throw re;
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    public void unbind() {
        ServiceConnection conn;
        Shizuku.UserServiceArgs args;
        IBinder binder;
        synchronized (serviceLock) {
            conn = activeConnection;
            args = activeArgs;
            binder = remoteService;
            activeConnection = null;
            activeArgs = null;
            remoteService = null;
        }
        sendRemoteStop(binder);
        if (conn != null && args != null) {
            try {
                Shizuku.unbindUserService(args, conn, true);
            } catch (Throwable ignored) {}
        }
    }
}
