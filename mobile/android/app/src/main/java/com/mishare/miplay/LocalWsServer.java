package com.mishare.miplay;

import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 轻量本地 127.0.0.1 RFC 6455 WebSocket 服务器，供 Android WebView 复用桌面端同一套全屋控制台 UI
 */
public class LocalWsServer {
    public interface Handler {
        String onRequest(String requestJson);
    }

    private final AtomicBoolean running = new AtomicBoolean(false);
    private ServerSocket serverSocket;
    private volatile int port = 0;
    private String token;
    private Handler handler;
    private final Set<Socket> clients = ConcurrentHashMap.newKeySet();

    public boolean start(String authToken, Handler handler) {
        if (running.get()) return true;
        this.token = authToken;
        this.handler = handler;
        try {
            serverSocket = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
            port = serverSocket.getLocalPort();
            running.set(true);
            new Thread(this::acceptLoop, "MiPlay-LocalWS").start();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        try { if (serverSocket != null) serverSocket.close(); } catch (Exception ignored) {}
        for (Socket s : clients) {
            try { s.close(); } catch (Exception ignored) {}
        }
        clients.clear();
    }

    public int getPort() { return port; }

    public void broadcastText(String text) {
        byte[] frame = buildTextFrame(text);
        for (Socket s : clients) {
            try {
                synchronized (s) {
                    OutputStream os = s.getOutputStream();
                    os.write(frame);
                    os.flush();
                }
            } catch (Exception e) {
                clients.remove(s);
                try { s.close(); } catch (Exception ignored) {}
            }
        }
    }

    private static byte[] buildTextFrame(String text) {
        byte[] payload = text.getBytes(StandardCharsets.UTF_8);
        ByteArrayOutputStream bos = new ByteArrayOutputStream(payload.length + 10);
        bos.write(0x81);
        int n = payload.length;
        if (n < 126) {
            bos.write(n);
        } else if (n <= 0xFFFF) {
            bos.write(126);
            bos.write((n >>> 8) & 0xFF);
            bos.write(n & 0xFF);
        } else {
            bos.write(127);
            for (int i = 7; i >= 0; i--) bos.write((int) (((long) n >>> (8 * i)) & 0xFF));
        }
        bos.write(payload, 0, payload.length);
        return bos.toByteArray();
    }

    private void acceptLoop() {
        while (running.get()) {
            try {
                Socket s = serverSocket.accept();
                s.setTcpNoDelay(true);
                new Thread(() -> handleClient(s), "MiPlay-WSClient").start();
            } catch (Exception e) {
                if (!running.get()) break;
            }
        }
    }

    private void handleClient(Socket s) {
        try {
            InputStream in = s.getInputStream();
            OutputStream out = s.getOutputStream();
            byte[] hbuf = new byte[4096];
            int n = in.read(hbuf);
            if (n <= 0) { s.close(); return; }
            String req = new String(hbuf, 0, n, StandardCharsets.UTF_8);
            if (token != null && !token.isEmpty() && !req.contains("token=" + token)) {
                out.write("HTTP/1.1 403 Forbidden\r\n\r\n".getBytes(StandardCharsets.UTF_8));
                s.close();
                return;
            }
            String wsKey = "";
            for (String line : req.split("\r\n")) {
                if (line.toLowerCase().startsWith("sec-websocket-key:")) {
                    wsKey = line.substring(18).trim();
                }
            }
            MessageDigest sha1 = MessageDigest.getInstance("SHA-1");
            byte[] dig = sha1.digest((wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes(StandardCharsets.UTF_8));
            String accept = Base64.encodeToString(dig, Base64.NO_WRAP);
            String resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                    + accept + "\r\n\r\n";
            out.write(resp.getBytes(StandardCharsets.UTF_8));
            out.flush();

            clients.add(s);

            while (running.get()) {
                int b0 = in.read();
                if (b0 < 0) break;
                int b1 = in.read();
                if (b1 < 0) break;
                int opcode = b0 & 0x0F;
                boolean masked = (b1 & 0x80) != 0;
                long plen = b1 & 0x7F;
                if (plen == 126) {
                    plen = ((in.read() & 0xFF) << 8) | (in.read() & 0xFF);
                } else if (plen == 127) {
                    plen = 0;
                    for (int i = 0; i < 8; i++) plen = (plen << 8) | (in.read() & 0xFF);
                }
                byte[] mask = new byte[4];
                if (masked) readFully(in, mask, 4);
                byte[] payload = new byte[(int) plen];
                if (plen > 0) readFully(in, payload, (int) plen);
                if (masked) {
                    for (int i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
                }
                if (opcode == 0x08) break;
                if (opcode == 0x01 && handler != null) {
                    String reply = handler.onRequest(new String(payload, StandardCharsets.UTF_8));
                    if (reply != null && !reply.isEmpty()) {
                        byte[] frame = buildTextFrame(reply);
                        synchronized (s) {
                            out.write(frame);
                            out.flush();
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        } finally {
            clients.remove(s);
            try { s.close(); } catch (Exception ignored) {}
        }
    }

    private static void readFully(InputStream in, byte[] dst, int len) throws Exception {
        int off = 0;
        while (off < len) {
            int r = in.read(dst, off, len - off);
            if (r < 0) throw new java.io.EOFException();
            off += r;
        }
    }
}
