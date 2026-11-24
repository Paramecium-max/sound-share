package com.mishare.miplay;

import android.os.SystemClock;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 实机实证 40 字节 (5 x Little-Endian uint64) UDP 主时钟校时服务器
 */
public class TimerService {
    public static class ClientTelemetry {
        public String clientIp;
        public int clientPort;
        public long lastSeq;
        public long lastSeenUs;
        public long residualErrorUs;
        public long packetCount;
    }

    private final AtomicBoolean running = new AtomicBoolean(false);
    private DatagramSocket socket;
    private Thread worker;
    private volatile int listenPort = 0;
    private final ConcurrentHashMap<String, ClientTelemetry> telemetry = new ConcurrentHashMap<>();

    public static long monotonicNowUs() {
        return SystemClock.elapsedRealtimeNanos() / 1000L;
    }

    public boolean start() {
        if (running.get()) return true;
        try {
            socket = new DatagramSocket(0);
            socket.setReuseAddress(true);
            socket.setSoTimeout(200);
            listenPort = socket.getLocalPort();
            running.set(true);
            worker = new Thread(this::runLoop, "MiPlay-TimerService");
            worker.setPriority(Thread.MAX_PRIORITY);
            worker.start();
            return true;
        } catch (Exception e) {
            stop();
            return false;
        }
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        if (socket != null) {
            try { socket.close(); } catch (Exception ignored) {}
            socket = null;
        }
        if (worker != null) {
            try { worker.join(500); } catch (Exception ignored) {}
            worker = null;
        }
        listenPort = 0;
    }

    public int getPort() {
        return listenPort;
    }

    public Map<String, ClientTelemetry> snapshotTelemetry() {
        return new HashMap<>(telemetry);
    }

    private void runLoop() {
        byte[] buf = new byte[128];
        DatagramPacket pkt = new DatagramPacket(buf, buf.length);
        while (running.get()) {
            try {
                pkt.setLength(buf.length);
                socket.receive(pkt);
                if (pkt.getLength() != 40) continue;

                long tRecv = monotonicNowUs();
                ByteBuffer bb = ByteBuffer.wrap(buf, 0, 40).order(ByteOrder.LITTLE_ENDIAN);
                long q0 = bb.getLong(0);
                long q4 = bb.getLong(32);

                long tSend = monotonicNowUs();
                bb.putLong(16, tRecv);
                bb.putLong(24, tSend);

                DatagramPacket resp = new DatagramPacket(buf, 40, pkt.getAddress(), pkt.getPort());
                socket.send(resp);

                String ip = pkt.getAddress().getHostAddress();
                if (ip != null) {
                    ClientTelemetry t = telemetry.computeIfAbsent(ip, k -> {
                        ClientTelemetry c = new ClientTelemetry();
                        c.clientIp = k;
                        return c;
                    });
                    t.clientPort = pkt.getPort();
                    t.lastSeq = q4;
                    t.lastSeenUs = tRecv;
                    t.residualErrorUs = (q0 > 0) ? (tRecv - q0) : 0;
                    t.packetCount++;
                }
            } catch (SocketTimeoutException ignored) {
            } catch (Exception e) {
                if (!running.get()) break;
            }
        }
    }
}
