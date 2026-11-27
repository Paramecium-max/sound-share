package io.soundflow.sender;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Process;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Main-process bridge managing the local Wireless ADB UID-2000/0 AudioPolicy daemon
 * over authenticated 127.0.0.1 TCP loopback.
 */
public final class WirelessAdbCaptureBridge {
    private static final String PREFS = "soundflow_wireless_adb";
    private static final String KEY_PORT = "adb_port";
    private static final String KEY_DAEMON_PORT = "daemon_port";
    private static final String KEY_TOKEN = "daemon_token";

    private volatile int configuredPort = 5555;
    private volatile String lastError = "";
    private volatile int activeDaemonPort = 0;
    private volatile String activeToken = "";
    private volatile boolean cachedAlive = false;

    public void init(Context ctx) {
        if (ctx == null) return;
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int savedPort = sp.getInt(KEY_PORT, -1);
        configuredPort = savedPort > 0 ? savedPort : WirelessAdbClient.detectTcpPort();
        activeDaemonPort = sp.getInt(KEY_DAEMON_PORT, 0);
        activeToken = sp.getString(KEY_TOKEN, "");
        cachedAlive = probeDaemonAlive();
    }

    public int getConfiguredPort() {
        return configuredPort;
    }

    public String getLastError() {
        return lastError;
    }

    public boolean isDaemonAliveCached() {
        return cachedAlive;
    }

    public java.util.Set<Integer> queryActivePlaybackUids() {
        java.util.Set<Integer> result = new java.util.LinkedHashSet<>();
        int port = activeDaemonPort;
        String token = activeToken;
        if (port <= 0 || token == null || token.isEmpty()) return result;
        try (Socket sock = new Socket()) {
            sock.connect(new InetSocketAddress("127.0.0.1", port), 200);
            sock.setSoTimeout(300);
            OutputStream out = sock.getOutputStream();
            out.write((token + " ACTIVE\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            String resp = readLine(sock.getInputStream());
            if (resp != null && resp.startsWith("OK")) {
                String body = resp.length() > 2 ? resp.substring(2).trim() : "";
                if (!body.isEmpty()) {
                    for (String tok : body.split(",")) {
                        try {
                            int uid = Integer.parseInt(tok.trim());
                            if (uid >= 10000) result.add(uid);
                        } catch (NumberFormatException ignored) {}
                    }
                }
            }
        } catch (Throwable ignored) {}
        return result;
    }

    public boolean isDaemonAlive() {
        boolean alive = probeDaemonAlive();
        cachedAlive = alive;
        return alive;
    }

    private boolean probeDaemonAlive() {
        int port = activeDaemonPort;
        String token = activeToken;
        if (port <= 0 || token == null || token.isEmpty()) return false;
        try (Socket sock = new Socket()) {
            sock.connect(new InetSocketAddress("127.0.0.1", port), 250);
            sock.setSoTimeout(300);
            OutputStream out = sock.getOutputStream();
            out.write((token + " PING\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            String resp = readLine(sock.getInputStream());
            return "PONG".equals(resp);
        } catch (Throwable ignored) {
            return false;
        }
    }

    private static int pickFreeLoopbackPort() throws IOException {
        try (ServerSocket ss = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            return ss.getLocalPort();
        }
    }

    public synchronized boolean connectAndStartDaemon(Context ctx, int port) {
        if (ctx == null) {
            lastError = "上下文不可用";
            return false;
        }
        int targetPort = (port > 0 && port <= 65535) ? port : WirelessAdbClient.detectTcpPort();
        configuredPort = targetPort;
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        sp.edit().putInt(KEY_PORT, targetPort).apply();

        if (isDaemonAlive()) {
            lastError = "";
            return true;
        }

        // Gracefully shut down any previous daemon on the old port before launching a fresh one.
        sendQuitQuiet(activeDaemonPort, activeToken);

        int daemonPort;
        try {
            daemonPort = pickFreeLoopbackPort();
        } catch (IOException e) {
            lastError = "分配本机回环端口失败: " + e.getMessage();
            return false;
        }
        String token = randomHexToken();
        String apkPath = ctx.getApplicationInfo().sourceDir;
        String cmd = "CLASSPATH=" + shellQuote(apkPath)
                + " setsid /system/bin/app_process /system/bin io.soundflow.sender.WirelessAdbAudioDaemon "
                + daemonPort + " " + shellQuote(token)
                + " </dev/null >/dev/null 2>&1 & echo LAUNCHED";

        try (WirelessAdbClient adb = WirelessAdbClient.connect(ctx, "127.0.0.1", targetPort)) {
            adb.execShell(cmd, 8000);
        } catch (Throwable t) {
            lastError = "无线 ADB 连接失败 (127.0.0.1:" + targetPort + "): "
                    + (t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName());
            cachedAlive = false;
            return false;
        }

        activeDaemonPort = daemonPort;
        activeToken = token;
        sp.edit().putInt(KEY_DAEMON_PORT, daemonPort).putString(KEY_TOKEN, token).apply();

        long deadline = System.currentTimeMillis() + 5000;
        while (System.currentTimeMillis() < deadline) {
            if (isDaemonAlive()) {
                lastError = "";
                return true;
            }
            try { Thread.sleep(80); } catch (InterruptedException ignored) { break; }
        }
        cachedAlive = false;
        lastError = "无线 ADB 已连通，但特权音频守护进程启动超时";
        return false;
    }

    private static void sendQuitQuiet(int port, String token) {
        if (port <= 0 || token == null || token.isEmpty()) return;
        try (Socket sock = new Socket()) {
            sock.connect(new InetSocketAddress("127.0.0.1", port), 250);
            sock.setSoTimeout(300);
            sock.getOutputStream().write((token + " QUIT\n").getBytes(StandardCharsets.UTF_8));
            sock.getOutputStream().flush();
        } catch (Throwable ignored) {}
    }

    public synchronized void disconnect(Context ctx) {
        sendQuitQuiet(activeDaemonPort, activeToken);
        activeDaemonPort = 0;
        activeToken = "";
        cachedAlive = false;
        lastError = "";
        if (ctx != null) {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                    .edit().remove(KEY_DAEMON_PORT).remove(KEY_TOKEN).apply();
        }
    }

    public AudioCaptureEngine.Recorder createRecorder(
            Context appCtx, int sampleRate, int frameSamples, int targetUid, boolean muteLocal) {
        final int port = activeDaemonPort;
        final String token = activeToken;
        if (port <= 0 || token == null || token.isEmpty()) {
            cachedAlive = false;
            throw new IllegalStateException("无线 ADB 特权音频守护进程未启动");
        }
        Socket sock = new Socket();
        try {
            sock.connect(new InetSocketAddress("127.0.0.1", port), 1500);
            sock.setTcpNoDelay(true);
            sock.setSoTimeout(4000);
            OutputStream out = sock.getOutputStream();
            String openCmd = String.format(Locale.US, "%s OPEN %d %d %d %d %d\n",
                    token, sampleRate, frameSamples, Process.myUid(), targetUid, muteLocal ? 1 : 0);
            out.write(openCmd.getBytes(StandardCharsets.UTF_8));
            out.flush();

            BufferedInputStream bin = new BufferedInputStream(sock.getInputStream(), 16384);
            String reply = readLine(bin);
            if (!"OK".equals(reply)) {
                try { sock.close(); } catch (Throwable ignored) {}
                throw new IllegalStateException("无线 ADB 抓音启动失败: " + (reply != null ? reply : "无响应"));
            }
            sock.setSoTimeout(0);
            final AtomicBoolean closed = new AtomicBoolean(false);
            return new AudioCaptureEngine.Recorder() {
                @Override
                public void start() {
                    // Daemon already started AudioRecord and is streaming PCM over loopback TCP.
                }

                @Override
                public int read(byte[] pcm, int offset, int length) {
                    if (closed.get()) return -1;
                    try {
                        return bin.read(pcm, offset, length);
                    } catch (Throwable t) {
                        return -1;
                    }
                }

                @Override
                public void stop() {
                    if (closed.compareAndSet(false, true)) {
                        try { sock.close(); } catch (Throwable ignored) {}
                    }
                }

                @Override
                public void release() {
                    stop();
                }
            };
        } catch (RuntimeException re) {
            try { sock.close(); } catch (Throwable ignored) {}
            throw re;
        } catch (Throwable t) {
            try { sock.close(); } catch (Throwable ignored) {}
            throw new RuntimeException(t);
        }
    }

    private static String readLine(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        int b;
        while ((b = in.read()) != -1) {
            if (b == '\n') break;
            if (b != '\r') bos.write(b);
            if (bos.size() > 1024) break;
        }
        if (b == -1 && bos.size() == 0) return null;
        return bos.toString(StandardCharsets.UTF_8.name()).trim();
    }

    private static String randomHexToken() {
        byte[] b = new byte[16];
        new SecureRandom().nextBytes(b);
        StringBuilder sb = new StringBuilder(32);
        for (byte x : b) sb.append(String.format(Locale.US, "%02x", x & 0xFF));
        return sb.toString();
    }

    private static String shellQuote(String s) {
        return "'" + s.replace("'", "'\"'\"'") + "'";
    }
}
