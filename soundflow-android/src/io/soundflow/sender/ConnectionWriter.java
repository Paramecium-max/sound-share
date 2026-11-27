package io.soundflow.sender;

import java.io.IOException;
import java.io.OutputStream;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

final class ConnectionWriter {
    private static final ScheduledThreadPoolExecutor DEADLINES = new ScheduledThreadPoolExecutor(2, task -> {
        Thread thread = new Thread(task, "MiPlay-WriteDeadline");
        thread.setDaemon(true);
        return thread;
    });

    static {
        DEADLINES.setRemoveOnCancelPolicy(true);
    }

    final Socket socket;
    private final ReentrantLock lock = new ReentrantLock();
    private final Object deadlineLock = new Object();
    private long activeWrite;
    private long writeId;

    ConnectionWriter(Socket socket) {
        this.socket = socket;
    }

    void write(byte[] bytes, long timeoutMs) throws IOException, InterruptedException {
        final long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
        if (!lock.tryLock(timeoutMs, TimeUnit.MILLISECONDS)) {
            abort();
            throw new SocketTimeoutException("Timed out waiting to write to MiPlay");
        }
        ScheduledFuture<?> timer = null;
        try {
            if (socket.isClosed()) throw new IOException("MiPlay connection closed");
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) throw new SocketTimeoutException("MiPlay write deadline elapsed");
            final long id = ++writeId;
            synchronized (deadlineLock) {
                activeWrite = id;
                timer = DEADLINES.schedule(() -> {
                    synchronized (deadlineLock) {
                        if (activeWrite == id) abort();
                    }
                }, remaining, TimeUnit.NANOSECONDS);
            }
            OutputStream out = socket.getOutputStream();
            out.write(bytes);
            out.flush();
            synchronized (deadlineLock) {
                activeWrite = 0;
                if (socket.isClosed() || System.nanoTime() >= deadline) {
                    throw new SocketTimeoutException("MiPlay write deadline elapsed");
                }
            }
        } catch (IOException | RuntimeException failure) {
            abort();
            throw failure;
        } finally {
            synchronized (deadlineLock) {
                activeWrite = 0;
                if (timer != null) timer.cancel(false);
            }
            lock.unlock();
        }
    }

    void abort() {
        try { socket.close(); } catch (IOException ignored) {}
    }
}
