package io.soundflow.sender;

import android.util.Log;
import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 反向 WFD RTSP Server (严格对齐红米 K90 实机抓包 M1-M7，同时兼容官方音箱/PC/Pad 与标准 WFD 接收端)
 */
public class WfdRtspServer {
    private static final String TAG = "MiPlayRTSP";
    private static final long MEDIA_CONNECT_TIMEOUT_NANOS = TimeUnit.MILLISECONDS.toNanos(1500);
    // The receiver may also open an auxiliary connection for cover art.
    private static final int MAX_PENDING_SOCKETS = 4;

    public interface Callbacks {
        void onStreamReady(String deviceId, String peerIp, ConnectionWriter writer, Runnable failure);
        void onStopMedia(String deviceId);
        void onLatencyReport(String deviceId, int latencyMs, int bitrate, long rtpPacketNum);
        void onSessionClosed(String deviceId);
        void onStatus(String deviceId, String status);
    }

    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private final Object socketLock = new Object();
    private ServerSocket serverSocket;
    private Socket activeCtrlSock;
    private ConnectionWriter activeCtrlWriter;
    private Socket activeDataSock;
    private volatile int listenPort = 0;
    private String deviceId;
    private String senderIp;
    private int timerPort;
    private long timeOffsetUs;
    private boolean preferLpcm;
    private int sampleRate = 48000;
    private Callbacks cbs;
    private final Map<Integer, Socket> pendingMultiPorts = new HashMap<>();

    public static long ipv4ToUint32(String ip) {
        String[] parts = ip.split("\\.");
        if (parts.length != 4) return 0;
        long r = 0;
        for (String p : parts) {
            r = (r << 8) | (Long.parseLong(p) & 0xFFL);
        }
        return r;
    }

    public boolean start(String deviceId, String senderIp, int timerPort,
                         long timeOffsetUs, boolean preferLpcm, int sampleRate, Callbacks cbs) {
        stop();
        this.deviceId = deviceId;
        this.senderIp = senderIp;
        this.timerPort = timerPort;
        this.timeOffsetUs = timeOffsetUs;
        this.preferLpcm = preferLpcm;
        this.sampleRate = (sampleRate == 44100 || sampleRate == 48000) ? sampleRate : 48000;
        this.cbs = cbs;
        try {
            ServerSocket listener = new ServerSocket(0);
            synchronized (socketLock) {
                serverSocket = listener;
                listenPort = listener.getLocalPort();
                stopping.set(false);
                running.set(true);
            }
            new Thread(() -> acceptLoop(listener), "MiPlay-RTSP-" + deviceId).start();
            return true;
        } catch (Exception e) {
            stop();
            return false;
        }
    }

    public void stop() {
        Socket ctrl;
        ConnectionWriter writer;
        Socket data;
        ServerSocket listener;
        List<Socket> pending;
        synchronized (socketLock) {
            stopping.set(true);
            if (!running.getAndSet(false)) return;
            listener = serverSocket;
            serverSocket = null;
            ctrl = activeCtrlSock;
            writer = activeCtrlWriter;
            activeCtrlSock = null;
            activeCtrlWriter = null;
            data = activeDataSock;
            activeDataSock = null;
            pending = new ArrayList<>(pendingMultiPorts.values());
            pendingMultiPorts.clear();
            listenPort = 0;
            socketLock.notifyAll();
        }
        if (listener != null) {
            try { listener.close(); } catch (Exception ignored) {}
        }
        if (ctrl != null) {
            try {
                String body = "wfd_trigger_method: TEARDOWN\r\n";
                String td = "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                        + "CSeq: 99\r\nContent-Type: text/parameters\r\n"
                        + "Content-Length: " + body.length() + "\r\n\r\n" + body;
                sendStr(writer, td);
            } catch (Exception ignored) {}
            try { ctrl.shutdownOutput(); } catch (Exception ignored) {}
            try { ctrl.close(); } catch (Exception ignored) {}
        }
        closeSocket(data);
        for (Socket s : pending) closeSocket(s);
    }

    public int getListenPort() {
        return listenPort;
    }

    private void report(String s) {
        Log.i(TAG, "[" + deviceId + "] " + s);
        if (cbs != null) cbs.onStatus(deviceId, s);
    }

    private void acceptLoop(ServerSocket listener) {
        while (!listener.isClosed()) {
            try {
                Socket client = listener.accept();
                try { client.setTcpNoDelay(true); } catch (Exception e) {
                    closeSocket(client);
                    continue;
                }
                synchronized (socketLock) {
                    if (serverSocket != listener || !running.get()) {
                        closeSocket(client);
                    } else if (activeCtrlSock == null) {
                        activeCtrlSock = client;
                        ConnectionWriter writer = new ConnectionWriter(client);
                        activeCtrlWriter = writer;
                        String peerIp = client.getInetAddress().getHostAddress();
                        new Thread(() -> handleControl(writer, peerIp), "MiPlay-RTSPCtrl-" + deviceId).start();
                    } else if (activeCtrlSock.getInetAddress().equals(client.getInetAddress())
                            && pendingMultiPorts.size() < MAX_PENDING_SOCKETS) {
                        Socket replaced = pendingMultiPorts.put(client.getPort(), client);
                        closeSocket(replaced);
                        socketLock.notifyAll();
                    } else {
                        closeSocket(client);
                    }
                }
            } catch (Exception e) {
                if (!listener.isClosed()) Log.w(TAG, "RTSP accept failed: " + e.getMessage());
                break;
            }
        }
    }

    private Socket claimMultiPortSocket(Socket ctrl, int port) throws InterruptedException {
        long deadline = System.nanoTime() + MEDIA_CONNECT_TIMEOUT_NANOS;
        synchronized (socketLock) {
            while (activeCtrlSock == ctrl) {
                Socket data = pendingMultiPorts.remove(port);
                if (data != null) {
                    activeDataSock = data;
                    return data;
                }
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) break;
                TimeUnit.NANOSECONDS.timedWait(socketLock, remaining);
            }
        }
        return null;
    }

    private static void closeSocket(Socket socket) {
        if (socket != null) {
            try { socket.close(); } catch (Exception ignored) {}
        }
    }

    private static int parseMediaPort(String multiPort) {
        for (String parameter : multiPort.split(";")) {
            int separator = parameter.indexOf('=');
            if (separator < 0 || !parameter.substring(0, separator).trim().equalsIgnoreCase("multi_port")) continue;
            try {
                int port = Integer.parseInt(parameter.substring(separator + 1).trim());
                if (port > 0 && port <= 65535) return port;
            } catch (NumberFormatException ignored) {}
        }
        return -1;
    }

    private static String extractHeader(String msg, String key) {
        String target = key.toLowerCase() + ":";
        for (String line : msg.split("\r\n")) {
            if (line.toLowerCase().startsWith(target)) {
                return line.substring(target.length()).trim();
            }
        }
        return "";
    }

    private static void sendStr(ConnectionWriter out, String s) throws Exception {
        out.write(s.getBytes(StandardCharsets.UTF_8), 120);
    }

    private void handleControl(ConnectionWriter out, String peerIp) {
        Socket ctrl = out.socket;
        Socket dataSock = null;
        try {
            ctrl.setSoTimeout(500);
            InputStream in = ctrl.getInputStream();

            report("RTSP 已反连 (" + peerIp + ")，正在 M1 OPTIONS 握手...");
            long ipU32 = ipv4ToUint32(senderIp);
            String m1 = "OPTIONS * RTSP/1.0\r\n"
                    + "Date: Tue, 24 Nov 2026 04:00:00 +0000\r\n"
                    + "Server: \r\n"
                    + "CSeq: 1\r\n"
                    + "Require: org.wfa.wfd1.0\r\n"
                    + "wfd_timer_server_port:" + ipU32 + ":" + timerPort + "\r\n"
                    + "lib_version: miplaycast_os3_release1.5 3.1.6030516\r\n\r\n";
            sendStr(out, m1);

            StringBuilder acc = new StringBuilder();
            byte[] buf = new byte[4096];
            boolean sentM3 = false, sentM4 = false, sentM5 = false;
            boolean sentTimeOffset = false, setupComplete = false, streaming = false;
            int mediaPort = 0;
            final String sessionId = "1189641421";

            while (running.get() && !ctrl.isClosed()) {
                int n;
                try {
                    n = in.read(buf);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (n < 0) break;
                acc.append(new String(buf, 0, n, StandardCharsets.UTF_8));

                while (true) {
                    int hdrEnd = acc.indexOf("\r\n\r\n");
                    if (hdrEnd < 0) break;
                    String hdr = acc.substring(0, hdrEnd + 4);
                    int contentLen = 0;
                    String cl = extractHeader(hdr, "Content-Length");
                    if (!cl.isEmpty()) {
                        try { contentLen = Integer.parseInt(cl); } catch (Exception ignored) {}
                    }
                    if (acc.length() < hdrEnd + 4 + contentLen) break;
                    String msg = acc.substring(0, hdrEnd + 4 + contentLen);
                    acc.delete(0, hdrEnd + 4 + contentLen);

                    String cseq = extractHeader(msg, "CSeq");
                    if (msg.startsWith("OPTIONS ")) {
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq
                                + "\r\nPublic: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n\r\n");
                        if (!sentM3) {
                            sentM3 = true;
                            report("RTSP M3 查询音频能力...");
                            String body = "wfd_content_SP_protection\r\n"
                                    + "wfd_video_formats\r\n"
                                    + "wfd_video_enctype\r\n"
                                    + "wfd_video_gamuttype\r\n"
                                    + "wfd_video_bitrate\r\n"
                                    + "wfd_dynamic_video_enable\r\n"
                                    + "wfd_current_video_info\r\n"
                                    + "wfd_audio_codecs\r\n"
                                    + "wfd_audio_codecs_v2\r\n"
                                    + "wfd_client_rtp_ports\r\n"
                                    + "wfd_tcp_enable\r\n"
                                    + "wfd_tcp_multi_session_enable\r\n"
                                    + "wfd_support_secure_win\r\n"
                                    + "wfd_mirror_control_enable\r\n"
                                    + "wfd_standby_resume_capability\r\n"
                                    + "wfd_image_enable_v2\r\n";
                            sendStr(out, "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 2\r\nContent-Type: text/parameters\r\n"
                                    + "Content-Length: " + body.length() + "\r\n\r\n" + body);
                        }
                    } else if (msg.startsWith("RTSP/1.0 200 OK")) {
                        if ("2".equals(cseq) && !sentM4) {
                            sentM4 = true;
                            boolean hasV2 = msg.contains("wfd_audio_codecs_v2");
                            boolean hasV1 = msg.contains("wfd_audio_codecs:");
                            report("RTSP M4 下发" + (preferLpcm ? "LPCM" : "ADTS-AAC") + " " + sampleRate + "Hz 参数...");
                            String v2Rate = (sampleRate == 44100) ? "0" : "1";
                            String lpcmMask = (sampleRate == 44100) ? "00000001" : "00000002";
                            StringBuilder sb = new StringBuilder();
                            if (hasV2 || !hasV1) {
                                sb.append("wfd_type_encryp: 0 1 1 0 0\r\n");
                                sb.append("wfd_audio_codecs_v2: ").append(preferLpcm ? "2 " : "1 ").append(v2Rate).append("\r\n");
                            }
                            if (hasV1) {
                                sb.append("wfd_audio_codecs: ")
                                  .append(preferLpcm ? ("LPCM " + lpcmMask + " 00") : "AAC 00000001 00").append("\r\n");
                            }
                            sb.append("wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n");
                            sb.append("wfd_presentation_URL: rtsp://").append(senderIp).append("/wfd1.0/streamid=0 none\r\n");
                            String body = sb.toString();
                            sendStr(out, "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 3\r\nContent-Type: text/parameters\r\n"
                                    + "Content-Length: " + body.length() + "\r\n\r\n" + body);
                        } else if ("3".equals(cseq) && !sentM5) {
                            sentM5 = true;
                            report("RTSP M5 触发 SETUP...");
                            String body = "wfd_trigger_method: SETUP\r\n";
                            sendStr(out, "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 4\r\nContent-Type: text/parameters\r\n"
                                    + "Content-Length: " + body.length() + "\r\n\r\n" + body);
                        }
                    } else if (msg.startsWith("SETUP ")) {
                        report("RTSP M6 SETUP 建立音频数据通道...");
                        String multiPort = extractHeader(hdr, "MultiPort");
                        int requestedPort = multiPort.isEmpty() ? 0 : parseMediaPort(multiPort);
                        if (requestedPort < 0 || requestedPort == ctrl.getPort()) {
                            sendStr(out, "RTSP/1.0 461 Unsupported Transport\r\nCSeq: " + cseq + "\r\n\r\n");
                            report("RTSP MultiPort 无效，未建立音频通道");
                            continue;
                        }
                        if (streaming && requestedPort != mediaPort) {
                            sendStr(out, "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: " + cseq + "\r\n\r\n");
                            continue;
                        }
                        mediaPort = requestedPort;
                        setupComplete = true;
                        if (mediaPort > 0 && !sentTimeOffset) {
                            String timeReq = "TIME_OFFSET rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 5\r\nContent-Type: text/parameters\r\n"
                                    + "TimeOffset:" + timeOffsetUs + "\r\nContent-Length: 0\r\n\r\n";
                            sendStr(out, timeReq);
                            sentTimeOffset = true;
                        }
                        String setupOk = "RTSP/1.0 200 OK\r\nCSeq: " + cseq
                                + "\r\nSession: " + sessionId + ";timeout=60"
                                + "\r\nTransport: RTP/AVP/TCP;interleaved=0-1;\r\n\r\n";
                        sendStr(out, setupOk);
                    } else if (msg.startsWith("PLAY ")) {
                        if (!setupComplete) {
                            sendStr(out, "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: " + cseq + "\r\n\r\n");
                            continue;
                        }
                        if (!streaming && mediaPort > 0) {
                            dataSock = claimMultiPortSocket(ctrl, mediaPort);
                            if (ctrl.isClosed() || stopping.get()) return;
                            if (dataSock == null) {
                                sendStr(out, "RTSP/1.0 503 Service Unavailable\r\nCSeq: " + cseq + "\r\n\r\n");
                                report("RTSP MultiPort 音频反连超时，接收端端口 " + mediaPort);
                                return;
                            }
                        }
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq
                                + "\r\nSession: " + sessionId + ";timeout=60\r\nRange: npt=now-\r\n\r\n");
                        if (!sentTimeOffset) {
                            String timeReq = "TIME_OFFSET rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 5\r\nContent-Type: text/parameters\r\n"
                                    + "TimeOffset:" + timeOffsetUs + "\r\nContent-Length: 0\r\n\r\n";
                            sendStr(out, timeReq);
                            sentTimeOffset = true;
                        }
                        if (!streaming) {
                            streaming = true;
                            report(dataSock != null
                                    ? "RTSP M7 同步推流中 (MultiPort，控制 " + ctrl.getPort() + "，音频 " + dataSock.getPort() + ")"
                                    : "RTSP M7 同步推流中 (TCP Interleaved，接收端未请求 MultiPort)");
                            if (cbs != null) {
                                ConnectionWriter media = dataSock != null ? new ConnectionWriter(dataSock) : out;
                                cbs.onStreamReady(deviceId, peerIp, media, out::abort);
                            }
                        }
                    } else if (msg.startsWith("GET_PARAMETER ")) {
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq
                                + "\r\nSession: " + sessionId + ";timeout=20\r\n\r\n");
                    } else if (msg.startsWith("VIDEO_LATENCY ")) {
                        if (cbs != null) {
                            int lat = 0, br = 0; long pkts = 0;
                            try { lat = Integer.parseInt(extractHeader(msg, "latency")); } catch (Exception ignored) {}
                            try { br = Integer.parseInt(extractHeader(msg, "bitrate")); } catch (Exception ignored) {}
                            try { pkts = Long.parseLong(extractHeader(msg, "rtpPacketNum")); } catch (Exception ignored) {}
                            cbs.onLatencyReport(deviceId, lat, br, pkts);
                        }
                    } else if (msg.startsWith("TEARDOWN ")) {
                        if (streaming && cbs != null) cbs.onStopMedia(deviceId);
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n\r\n");
                        return;
                    }
                }
            }
        } catch (Exception e) {
            if (!stopping.get()) report("RTSP 异常结束: " + e.getMessage());
        } finally {
            boolean ownsSession;
            synchronized (socketLock) {
                ownsSession = activeCtrlSock == ctrl;
                if (ownsSession) {
                    running.set(false);
                    activeCtrlSock = null;
                    activeCtrlWriter = null;
                    activeDataSock = null;
                    if (serverSocket != null) {
                        try { serverSocket.close(); } catch (Exception ignored) {}
                        serverSocket = null;
                    }
                    listenPort = 0;
                    for (Socket pending : pendingMultiPorts.values()) closeSocket(pending);
                    pendingMultiPorts.clear();
                }
            }
            if (ownsSession) {
                if (cbs != null) cbs.onStopMedia(deviceId);
                closeSocket(dataSock);
                closeSocket(ctrl);
                if (!stopping.get() && cbs != null) cbs.onSessionClosed(deviceId);
            }
        }
    }
}
