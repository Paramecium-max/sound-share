package io.soundflow.sender;

import android.content.Context;
import android.media.AudioRecord;
import android.os.Looper;
import android.os.Process;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/**
 * UID 2000 (com.android.shell) / UID 0 AudioPolicy loopback daemon launched via local wireless ADB
 * (app_process). Listens exclusively on 127.0.0.1:<port> with a 128-bit random token so SELinux
 * Enforcing never blocks untrusted_app -> shell/ksu Unix socket connections.
 */
public final class WirelessAdbAudioDaemon {
    private static final String TAG = "SoundFlowAdbAudio";
    private static final Object LOCK = new Object();
    private static volatile boolean active = false;
    private static PrivilegedAudioPolicyHelper.ActivePolicySession activeSession;
    private static Context cachedShellContext;

    private WirelessAdbAudioDaemon() {}

    public static void main(String[] args) {
        if (args == null || args.length < 2) {
            System.exit(2);
            return;
        }
        final int port;
        try {
            port = Integer.parseInt(args[0]);
        } catch (NumberFormatException e) {
            System.exit(2);
            return;
        }
        final String expectedToken = args[1];

        try {
            if (Looper.getMainLooper() == null) {
                Looper.prepareMainLooper();
            }
            resolveShellContext();
        } catch (Throwable t) {
            android.util.Log.w(TAG, "Initializing audio policy shell context", t);
        }

        Thread serverThread = new Thread(() -> {
            try (ServerSocket server = new ServerSocket(port, 8, InetAddress.getByName("127.0.0.1"))) {
                server.setReuseAddress(true);
                android.util.Log.i(TAG, "Local audio daemon ready");
                while (true) {
                    final Socket client = server.accept();
                    client.setTcpNoDelay(true);
                    Thread worker = new Thread(() -> handleClient(client, expectedToken),
                            "SoundFlow-AdbAudioClient");
                    worker.setDaemon(true);
                    worker.start();
                }
            } catch (Throwable t) {
                android.util.Log.e(TAG, "Local audio daemon failed", t);
                stopCurrentSession();
                System.exit(1);
            }
        }, "SoundFlow-AdbAudioServer");
        serverThread.setDaemon(false);
        serverThread.start();

        // Pump the main Looper so AudioPolicyFocusListener callbacks execute immediately.
        Looper.loop();
    }

    private static Context resolveShellContext() throws Exception {
        synchronized (LOCK) {
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
    }

    private static String queryActivePlaybackUidsCsv() {
        try {
            Context ctx = resolveShellContext();
            android.media.AudioManager am = (android.media.AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am == null) return "";
            java.util.List<android.media.AudioPlaybackConfiguration> configs = am.getActivePlaybackConfigurations();
            if (configs == null || configs.isEmpty()) return "";
            java.util.LinkedHashSet<Integer> mediaUids = new java.util.LinkedHashSet<>();
            java.util.LinkedHashSet<Integer> otherUids = new java.util.LinkedHashSet<>();
            Method getClientUid = null;
            Method getPlayerState = null;
            Method getPlayerType = null;
            // Iterate from newest to oldest so the most recently started player comes first.
            for (int i = configs.size() - 1; i >= 0; i--) {
                android.media.AudioPlaybackConfiguration cfg = configs.get(i);
                if (cfg == null) continue;
                try {
                    if (getClientUid == null) {
                        getClientUid = cfg.getClass().getMethod("getClientUid");
                    }
                    int uid = (Integer) getClientUid.invoke(cfg);
                    if (uid < 10000) continue;
                    boolean started = true;
                    try {
                        if (getPlayerState == null) {
                            getPlayerState = cfg.getClass().getMethod("getPlayerState");
                        }
                        int st = (Integer) getPlayerState.invoke(cfg);
                        // AudioPlaybackConfiguration.PLAYER_STATE_STARTED == 2
                        started = (st == 2);
                    } catch (Throwable ignored) {}
                    if (!started) continue;

                    // Skip SoundPool UI/keyboard click players (PLAYER_TYPE_JAM_SOUNDPOOL == 3)
                    try {
                        if (getPlayerType == null) {
                            getPlayerType = cfg.getClass().getMethod("getPlayerType");
                        }
                        int ptype = (Integer) getPlayerType.invoke(cfg);
                        if (ptype == 3) continue;
                    } catch (Throwable ignored) {}

                    android.media.AudioAttributes attr = cfg.getAudioAttributes();
                    int usage = attr != null ? attr.getUsage() : android.media.AudioAttributes.USAGE_UNKNOWN;
                    if (usage == android.media.AudioAttributes.USAGE_MEDIA
                            || usage == android.media.AudioAttributes.USAGE_GAME) {
                        mediaUids.add(uid);
                    } else if (usage == android.media.AudioAttributes.USAGE_UNKNOWN) {
                        otherUids.add(uid);
                    }
                } catch (Throwable ignored) {}
            }
            mediaUids.addAll(otherUids);
            StringBuilder sb = new StringBuilder();
            for (Integer u : mediaUids) {
                if (sb.length() > 0) sb.append(',');
                sb.append(u);
            }
            return sb.toString();
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static void stopCurrentSession() {
        synchronized (LOCK) {
            active = false;
            PrivilegedAudioPolicyHelper.ActivePolicySession sess = activeSession;
            activeSession = null;
            if (sess != null) sess.close();
        }
    }

    private static void handleClient(Socket client, String expectedToken) {
        try (Socket sock = client) {
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(sock.getInputStream(), StandardCharsets.UTF_8));
            OutputStream out = sock.getOutputStream();
            String line = reader.readLine();
            if (line == null) return;
            String[] parts = line.trim().split("\\s+");
            if (parts.length < 2 || !expectedToken.equals(parts[0])) {
                out.write("ERR unauthorized\n".getBytes(StandardCharsets.UTF_8));
                out.flush();
                return;
            }
            String cmd = parts[1];
            if ("PING".equals(cmd)) {
                out.write("PONG\n".getBytes(StandardCharsets.UTF_8));
                out.flush();
                return;
            }
            if ("ACTIVE".equals(cmd)) {
                String uids = queryActivePlaybackUidsCsv();
                out.write(("OK " + uids + "\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
                return;
            }
            if ("STOP".equals(cmd)) {
                stopCurrentSession();
                out.write("OK\n".getBytes(StandardCharsets.UTF_8));
                out.flush();
                return;
            }
            if ("QUIT".equals(cmd)) {
                stopCurrentSession();
                out.write("BYE\n".getBytes(StandardCharsets.UTF_8));
                out.flush();
                System.exit(0);
                return;
            }
            if ("OPEN".equals(cmd) && parts.length >= 7) {
                int sampleRate = Integer.parseInt(parts[2]);
                int frameSamples = Integer.parseInt(parts[3]);
                int selfUid = Integer.parseInt(parts[4]);
                int targetUid = Integer.parseInt(parts[5]);
                boolean muteLocal = Integer.parseInt(parts[6]) != 0;
                streamAudio(out, sampleRate, frameSamples, selfUid, targetUid, muteLocal);
                return;
            }
            out.write("ERR unknown_command\n".getBytes(StandardCharsets.UTF_8));
            out.flush();
        } catch (Throwable ignored) {}
    }

    private static void streamAudio(
            OutputStream out,
            int sampleRate,
            int frameSamples,
            int selfUid,
            int targetUid,
            boolean muteLocal) {
        PrivilegedAudioPolicyHelper.ActivePolicySession sess = null;
        try {
            synchronized (LOCK) {
                stopCurrentSession();
                Context shellCtx = resolveShellContext();
                sess = PrivilegedAudioPolicyHelper.openLoopbackSession(
                        shellCtx, sampleRate, selfUid, targetUid, muteLocal);
                sess.audioRecord.startRecording();
                activeSession = sess;
                active = true;
            }
            out.write("OK\n".getBytes(StandardCharsets.UTF_8));
            out.flush();

            try {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            } catch (Throwable ignored) {}
            final int frameBytes = Math.max(1920, Math.min(32768, frameSamples * 4));
            byte[] buf = new byte[frameBytes];
            while (active) {
                int n = sess.audioRecord.read(buf, 0, buf.length, AudioRecord.READ_BLOCKING);
                if (n <= 0) break;
                out.write(buf, 0, n);
            }
        } catch (Throwable t) {
            Throwable root = t;
            while (root instanceof java.lang.reflect.InvocationTargetException && root.getCause() != null) {
                root = root.getCause();
            }
            root.printStackTrace(System.err);
            if (sess == null) {
                try {
                    String msg = root.getClass().getSimpleName()
                            + (root.getMessage() != null ? (": " + root.getMessage().replace('\n', ' ')) : "");
                    out.write(("ERR " + msg + "\n").getBytes(StandardCharsets.UTF_8));
                    out.flush();
                } catch (Throwable ignored) {}
            }
        } finally {
            synchronized (LOCK) {
                if (sess != null && activeSession == sess) {
                    active = false;
                    activeSession = null;
                }
            }
            if (sess != null) sess.close();
        }
    }
}
