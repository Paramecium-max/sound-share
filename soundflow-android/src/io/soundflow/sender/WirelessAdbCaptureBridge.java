package io.soundflow.sender;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Process;
import android.os.Build;
import android.util.Log;

import io.github.muntashirakon.adb.android.AdbMdns;

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
    private static final String TAG = "SoundFlowWirelessAdb";
    private static final String PREFS = "soundflow_wireless_adb";
    private static final String KEY_PORT = "adb_port";
    private static final String KEY_DAEMON_PORT = "daemon_port";
    private static final String KEY_TOKEN = "daemon_token";

    private volatile int configuredPort = 5555;
    private volatile String lastError = "";
    private volatile int activeDaemonPort = 0;
    private volatile String activeToken = "";
    private volatile boolean cachedAlive = false;
    private volatile boolean pairingSuggested;
    private LocalWirelessAdb daemonConnection;

    public void init(Context ctx) {
        if (ctx == null) return;
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int savedPort = sp.getInt(KEY_PORT, -1);
        configuredPort = savedPort > 0 ? savedPort : Build.VERSION.SDK_INT >= 30 ? 0 : WirelessAdbClient.detectTcpPort();
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

    public boolean shouldSuggestPairing() {
        return pairingSuggested;
    }

    public synchronized boolean connectAndStartDaemon(Context ctx, int port) {
        return startDaemon(ctx, port, 0, null);
    }

    public synchronized boolean pairAndStartDaemon(Context ctx, int pairingPort, int port, String code) {
        if (Build.VERSION.SDK_INT < 30 || code == null || !code.matches("[0-9]{6}")) {
            lastError = "请输入系统当前显示的六位无线调试配对码";
            return false;
        }
        return startDaemon(ctx, port, pairingPort, code);
    }

    private boolean startDaemon(Context ctx, int port, int pairingPort, String pairingCode) {
        pairingSuggested = false;
        if (ctx == null) {
            lastError = "上下文不可用";
            return false;
        }
        if (isDaemonAlive() && pairingCode == null) {
            lastError = "";
            return true;
        }
        boolean legacy = Build.VERSION.SDK_INT < 30 || (pairingCode == null && port == 5555);
        int targetPort = port > 0 && port <= 65535 ? port : 0;
        int daemonPort;
        try {
            daemonPort = pickFreeLoopbackPort();
        } catch (IOException e) {
            lastError = "分配本机音频守护进程端口失败";
            return false;
        }
        String token = randomHexToken();
        String classPath = shellQuote(ctx.getApplicationInfo().sourceDir);
        String daemon = "/system/bin/app_process /system/bin io.soundflow.sender.WirelessAdbAudioDaemon "
                + daemonPort + " " + shellQuote(token);

        String phase = pairingCode != null ? "pair" : "connect";
        LocalWirelessAdb connection = null;
        try {
            if (legacy) {
                if (targetPort == 0) targetPort = WirelessAdbClient.detectTcpPort();
                try (WirelessAdbClient adb = WirelessAdbClient.connect(ctx, "127.0.0.1", targetPort)) {
                    phase = "launch";
                    String reply = adb.execShell("CLASSPATH=" + classPath + " setsid " + daemon
                            + " </dev/null >/dev/null 2>&1 & echo LAUNCHED", 8000);
                    if (!reply.contains("LAUNCHED")) throw new IOException("未收到音频守护进程启动确认");
                }
            } else {
                connection = new LocalWirelessAdb(ctx);
                if (pairingCode != null) {
                    int peerPort = pairingPort > 0 && pairingPort <= 65535 ? pairingPort
                            : LocalWirelessAdb.discover(ctx, AdbMdns.SERVICE_TYPE_TLS_PAIRING);
                    if (peerPort == 0) throw new IOException("未发现本机配对端口，请保持系统配对码窗口打开");
                    connection.pairLocal(peerPort, pairingCode);
                    Log.i(TAG, "Local TLS pairing completed");
                    phase = "connect";
                }
                if (targetPort == 0) targetPort = LocalWirelessAdb.discover(ctx, AdbMdns.SERVICE_TYPE_TLS_CONNECT);
                if (targetPort == 0) throw new IOException("未发现本机无线调试连接端口");
                connection.connectLocal(targetPort);
                phase = "launch";
                // Keep the ADB shell open while its foreground daemon is in use.
                // Closing immediately after launching lets some adbd versions reap it.
                connection.launchDaemon("export CLASSPATH=" + classPath + "; exec " + daemon);
            }
        } catch (Exception error) {
            closeQuietly(connection);
            if (error instanceof InterruptedException) Thread.currentThread().interrupt();
            pairingSuggested = !legacy && "connect".equals(phase) && pairingCode == null;
            String detail = error.getMessage();
            if ("pair".equals(phase)) {
                lastError = error instanceof IOException && detail != null && detail.startsWith("未发现")
                        ? detail : "无线调试配对失败，请确认配对码仍有效";
            } else if ("connect".equals(phase)) {
                lastError = pairingSuggested ? "无线调试尚未连接，请完成本机配对后重试"
                        : "无线调试连接失败" + (detail == null ? "" : "：" + detail);
            } else {
                lastError = "无线 ADB 已连接，音频守护进程启动失败";
            }
            Log.w(TAG, "Local ADB " + phase + " failed: " + error.getClass().getSimpleName());
            return false;
        }

        sendQuitQuiet(activeDaemonPort, activeToken);
        closeQuietly(daemonConnection);
        daemonConnection = connection;
        configuredPort = targetPort;
        activeDaemonPort = daemonPort;
        activeToken = token;
        long deadline = android.os.SystemClock.elapsedRealtime() + 5000;
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            if (isDaemonAlive()) {
                ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt(KEY_PORT, targetPort)
                        .putInt(KEY_DAEMON_PORT, daemonPort).putString(KEY_TOKEN, token).apply();
                lastError = "";
                Log.i(TAG, "Local audio daemon authenticated and ready, ADB port=" + targetPort);
                return true;
            }
            try {
                Thread.sleep(80);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        disconnect(ctx);
        lastError = "无线 ADB 已连通，但音频守护进程启动超时";
        Log.w(TAG, "Local audio daemon PING timed out");
        return false;
    }

    private static void closeQuietly(LocalWirelessAdb connection) {
        if (connection == null) return;
        try { connection.close(); } catch (IOException ignored) {}
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
        closeQuietly(daemonConnection);
        daemonConnection = null;
        activeDaemonPort = 0;
        activeToken = "";
        cachedAlive = false;
        pairingSuggested = false;
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
