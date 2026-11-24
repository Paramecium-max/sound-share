package com.mishare.miplay;

import android.util.Log;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 反向 WFD RTSP Server (严格对齐红米 K90 实机抓包 M1-M7，同时兼容官方音箱/PC/Pad 与标准 WFD 接收端)
 */
public class WfdRtspServer {
    private static final String TAG = "MiPlayRTSP";

    public interface Callbacks {
        void onStreamReady(String deviceId, String peerIp, Socket dataSock);
        void onLatencyReport(String deviceId, int latencyMs, int bitrate, long rtpPacketNum);
        void onSessionClosed(String deviceId);
        void onStatus(String deviceId, String status);
    }

    private final AtomicBoolean running = new AtomicBoolean(false);
    private ServerSocket serverSocket;
    private volatile int listenPort = 0;
    private String deviceId;
    private String senderIp;
    private int timerPort;
    private long timeOffsetUs;
    private boolean preferLpcm;
    private Callbacks cbs;
    private final ConcurrentHashMap<Integer, Socket> pendingMultiPorts = new ConcurrentHashMap<>();
    private Thread acceptTh;

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
                         long timeOffsetUs, boolean preferLpcm, Callbacks cbs) {
        stop();
        this.deviceId = deviceId;
        this.senderIp = senderIp;
        this.timerPort = timerPort;
        this.timeOffsetUs = timeOffsetUs;
        this.preferLpcm = preferLpcm;
        this.cbs = cbs;
        try {
            serverSocket = new ServerSocket(0);
            serverSocket.setReuseAddress(true);
            serverSocket.setSoTimeout(300);
            listenPort = serverSocket.getLocalPort();
            running.set(true);
            acceptTh = new Thread(this::acceptLoop, "MiPlay-RTSP-" + deviceId);
            acceptTh.start();
            return true;
        } catch (Exception e) {
            stop();
            return false;
        }
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        if (serverSocket != null) {
            try { serverSocket.close(); } catch (Exception ignored) {}
            serverSocket = null;
        }
        for (Socket s : pendingMultiPorts.values()) {
            try { s.close(); } catch (Exception ignored) {}
        }
        pendingMultiPorts.clear();
        listenPort = 0;
    }

    public int getListenPort() {
        return listenPort;
    }

    private void report(String s) {
        Log.i(TAG, "[" + deviceId + "] " + s);
        if (cbs != null) cbs.onStatus(deviceId, s);
    }

    private void acceptLoop() {
        boolean hasCtrl = false;
        while (running.get()) {
            try {
                Socket client = serverSocket.accept();
                client.setTcpNoDelay(true);
                int peerPort = client.getPort();
                String peerIp = client.getInetAddress().getHostAddress();
                if (!hasCtrl) {
                    hasCtrl = true;
                    new Thread(() -> handleControl(client, peerIp), "MiPlay-RTSPCtrl-" + deviceId).start();
                } else {
                    pendingMultiPorts.put(peerPort, client);
                }
            } catch (SocketTimeoutException ignored) {
            } catch (Exception e) {
                if (!running.get()) break;
                try { Thread.sleep(50); } catch (InterruptedException ignored) { break; }
            }
        }
    }

    private Socket claimMultiPortSocket(int port) {
        for (int i = 0; i < 30 && running.get(); i++) {
            Socket s = pendingMultiPorts.remove(port);
            if (s != null) return s;
            try { Thread.sleep(50); } catch (InterruptedException ignored) { break; }
        }
        return null;
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

    private static void sendStr(OutputStream out, String s) throws Exception {
        out.write(s.getBytes(StandardCharsets.UTF_8));
        out.flush();
    }

    private void handleControl(Socket ctrl, String peerIp) {
        Socket dataSock = null;
        try {
            ctrl.setSoTimeout(500);
            InputStream in = ctrl.getInputStream();
            OutputStream out = ctrl.getOutputStream();

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
            boolean sentTimeOffset = false, hasMultiPort = false;
            final String sessionId = "1189641421";

            while (running.get()) {
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
                                    + "wfd_audio_codecs\r\n"
                                    + "wfd_audio_codecs_v2\r\n"
                                    + "wfd_client_rtp_ports\r\n"
                                    + "wfd_tcp_enable\r\n"
                                    + "wfd_tcp_multi_session_enable\r\n";
                            sendStr(out, "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 2\r\nContent-Type: text/parameters\r\n"
                                    + "Content-Length: " + body.length() + "\r\n\r\n" + body);
                        }
                    } else if (msg.startsWith("RTSP/1.0 200 OK")) {
                        if ("2".equals(cseq) && !sentM4) {
                            sentM4 = true;
                            boolean hasV2 = msg.contains("wfd_audio_codecs_v2");
                            boolean hasV1 = msg.contains("wfd_audio_codecs:");
                            report("RTSP M4 下发" + (preferLpcm ? "LPCM" : "ADTS-AAC") + "参数...");
                            StringBuilder sb = new StringBuilder();
                            if (hasV2 || !hasV1) {
                                sb.append("wfd_type_encryp: 0 1 1 0 0\r\n");
                                sb.append("wfd_audio_codecs_v2: ").append(preferLpcm ? "2 1" : "1 1").append("\r\n");
                            }
                            if (hasV1) {
                                sb.append("wfd_audio_codecs: ")
                                  .append(preferLpcm ? "LPCM 00000002 00" : "AAC 00000001 00").append("\r\n");
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
                        String mp = extractHeader(msg, "MultiPort");
                        int idx = mp.indexOf("multi_port=");
                        if (idx >= 0) {
                            hasMultiPort = true;
                            String sub = mp.substring(idx + 11).split("[;\\s]")[0];
                            try { dataSock = claimMultiPortSocket(Integer.parseInt(sub)); } catch (Exception ignored) {}
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
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq
                                + "\r\nSession: " + sessionId + ";timeout=60\r\nRange: npt=now-\r\n\r\n");
                        if (!sentTimeOffset) {
                            String timeReq = "TIME_OFFSET rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
                                    + "CSeq: 5\r\nContent-Type: text/parameters\r\n"
                                    + "TimeOffset:" + timeOffsetUs + "\r\nContent-Length: 0\r\n\r\n";
                            sendStr(out, timeReq);
                            sentTimeOffset = true;
                        }
                        report(hasMultiPort ? "RTSP M7 同步推流中 (MultiPort)" : "RTSP M7 同步推流中 (TCP Interleaved)");
                        Socket outSock = (dataSock != null) ? dataSock : ctrl;
                        if (cbs != null) cbs.onStreamReady(deviceId, peerIp, outSock);
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
                        sendStr(out, "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n\r\n");
                        break;
                    }
                }
            }
        } catch (Exception e) {
            report("RTSP 异常结束: " + e.getMessage());
        } finally {
            try { ctrl.close(); } catch (Exception ignored) {}
            if (cbs != null) cbs.onSessionClosed(deviceId);
        }
    }
}
