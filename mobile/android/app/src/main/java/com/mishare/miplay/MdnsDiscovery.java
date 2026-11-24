package com.mishare.miplay;

import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 局域网 mDNS 发现器：同时监听/查询 UDP 5355 (_miplay_lan._tcp.local.) 与 UDP 5353 (_mi-connect._udp.local.)
 * 支持解析 _mi-connect TXT appsData 内的真实 TCP 控制端口，并具备防异常死循环保护
 */
public class MdnsDiscovery {
    public static class DiscoveredDevice {
        public String id;
        public String name;
        public String ip;
        public int cmdPort = 8899;
        public String mac = "";
        public String roomName = "";
        public int deviceType = 4;
        public boolean isManual = false;
        public long lastSeenUs;
    }

    public interface Callback {
        void onDeviceFound(DiscoveredDevice dev);
    }

    private static final String MDNS_GROUP = "224.0.0.251";
    private final AtomicBoolean running = new AtomicBoolean(false);
    private MulticastSocket sock5355;
    private MulticastSocket sock5353;
    private Thread th5355;
    private Thread th5353;
    private Thread thQuery;
    private Callback callback;

    public static String pickWifiIpv4() {
        try {
            List<NetworkInterface> ifaces = Collections.list(NetworkInterface.getNetworkInterfaces());
            String fallback = "127.0.0.1";
            for (NetworkInterface nif : ifaces) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                String name = nif.getName().toLowerCase();
                for (InetAddress addr : Collections.list(nif.getInetAddresses())) {
                    if (!(addr instanceof Inet4Address)) continue;
                    String ip = addr.getHostAddress();
                    if (ip == null) continue;
                    if (name.startsWith("wlan") || name.startsWith("eth")) return ip;
                    if (ip.startsWith("192.168.") || ip.startsWith("10.")) fallback = ip;
                }
            }
            return fallback;
        } catch (Exception e) {
            return "127.0.0.1";
        }
    }

    public boolean start(Callback cb) {
        if (running.get()) return true;
        this.callback = cb;
        sock5355 = openMulticast(5355);
        sock5353 = openMulticast(5353);
        if (sock5355 == null && sock5353 == null) return false;

        running.set(true);
        if (sock5355 != null) {
            th5355 = new Thread(() -> recvLoop(sock5355), "MiPlay-mDNS-5355");
            th5355.start();
        }
        if (sock5353 != null) {
            th5353 = new Thread(() -> recvLoop(sock5353), "MiPlay-mDNS-5353");
            th5353.start();
        }
        thQuery = new Thread(() -> {
            int tick = 0;
            while (running.get()) {
                if (tick % 6 == 0) triggerQuery();
                tick++;
                try { Thread.sleep(1000); } catch (InterruptedException ignored) { break; }
            }
        }, "MiPlay-mDNS-Query");
        thQuery.start();
        return true;
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        if (sock5355 != null) { try { sock5355.close(); } catch (Exception ignored) {} sock5355 = null; }
        if (sock5353 != null) { try { sock5353.close(); } catch (Exception ignored) {} sock5353 = null; }
        if (thQuery != null) thQuery.interrupt();
    }

    public void triggerQuery() {
        sendPtrQuery(sock5355, "_miplay_lan._tcp.local", 5355);
        sendPtrQuery(sock5353, "_miplay_lan._tcp.local", 5353);
        sendPtrQuery(sock5353, "_mi-connect._udp.local", 5353);
    }

    private MulticastSocket openMulticast(int port) {
        try {
            MulticastSocket s = new MulticastSocket(port);
            s.setReuseAddress(true);
            s.setSoTimeout(300);
            s.joinGroup(InetAddress.getByName(MDNS_GROUP));
            return s;
        } catch (Exception e) {
            try {
                MulticastSocket s = new MulticastSocket();
                s.setSoTimeout(300);
                s.joinGroup(InetAddress.getByName(MDNS_GROUP));
                return s;
            } catch (Exception ex) {
                return null;
            }
        }
    }

    private void sendPtrQuery(MulticastSocket sock, String domain, int dstPort) {
        if (sock == null) return;
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            bos.write(new byte[]{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0});
            for (String label : domain.split("\\.")) {
                if (label.isEmpty()) continue;
                byte[] b = label.getBytes(StandardCharsets.UTF_8);
                bos.write(b.length);
                bos.write(b);
            }
            bos.write(0);
            bos.write(0x00); bos.write(0x0C);
            bos.write(0x00); bos.write(0x01);
            byte[] pkt = bos.toByteArray();
            sock.send(new DatagramPacket(pkt, pkt.length, InetAddress.getByName(MDNS_GROUP), dstPort));
        } catch (Exception ignored) {}
    }

    private void recvLoop(MulticastSocket sock) {
        byte[] buf = new byte[4096];
        DatagramPacket pkt = new DatagramPacket(buf, buf.length);
        while (running.get()) {
            try {
                pkt.setLength(buf.length);
                sock.receive(pkt);
                if (pkt.getLength() <= 12) continue;
                String srcIp = pkt.getAddress().getHostAddress();
                if (srcIp != null) parseResponse(buf, pkt.getLength(), srcIp);
            } catch (SocketTimeoutException ignored) {
            } catch (Exception e) {
                if (!running.get()) break;
                try { Thread.sleep(200); } catch (InterruptedException ignored) { break; }
            }
        }
    }

    private static int readU16(byte[] b, int off) {
        return ((b[off] & 0xFF) << 8) | (b[off + 1] & 0xFF);
    }

    private static String decodeName(byte[] buf, int len, int[] offRef, int depth) {
        if (depth > 8) return "";
        StringBuilder sb = new StringBuilder();
        int off = offRef[0];
        int orig = off;
        boolean jumped = false;
        int steps = 0;
        while (off < len && steps++ < 64) {
            int lb = buf[off] & 0xFF;
            if (lb == 0) { off++; break; }
            if ((lb & 0xC0) == 0xC0) {
                if (off + 1 >= len) break;
                int ptr = ((lb & 0x3F) << 8) | (buf[off + 1] & 0xFF);
                if (ptr >= len) break;
                if (!jumped) { orig = off + 2; jumped = true; }
                int[] subOff = new int[]{ptr};
                String sub = decodeName(buf, len, subOff, depth + 1);
                if (sb.length() > 0 && !sub.isEmpty()) sb.append('.');
                sb.append(sub);
                break;
            } else {
                off++;
                if (off + lb > len) break;
                if (sb.length() > 0) sb.append('.');
                sb.append(new String(buf, off, lb, StandardCharsets.UTF_8));
                off += lb;
            }
        }
        offRef[0] = jumped ? orig : off;
        return sb.toString();
    }

    private void parseResponse(byte[] data, int len, String srcIp) {
        int flags = readU16(data, 2);
        if ((flags & 0x8000) == 0) return;

        int qd = readU16(data, 4);
        int an = readU16(data, 6);
        int ns = readU16(data, 8);
        int ar = readU16(data, 10);
        int[] offRef = new int[]{12};

        for (int i = 0; i < qd && offRef[0] < len; i++) {
            decodeName(data, len, offRef, 0);
            offRef[0] += 4;
        }

        DiscoveredDevice dev = new DiscoveredDevice();
        dev.ip = srcIp;
        dev.cmdPort = 8899;
        boolean matched = false;

        int total = an + ns + ar;
        for (int i = 0; i < total && offRef[0] + 10 <= len; i++) {
            String rname = decodeName(data, len, offRef, 0);
            if (offRef[0] + 10 > len) break;
            int rtype = readU16(data, offRef[0]);
            int rdlen = readU16(data, offRef[0] + 8);
            offRef[0] += 10;
            int rstart = offRef[0];
            if (rstart + rdlen > len) break;

            if (rname.contains("_miplay_lan") || rname.contains("_mi-connect")) {
                matched = true;
                if ((dev.name == null || dev.name.isEmpty()) && !rname.startsWith("_")) {
                    int dot = rname.indexOf('.');
                    if (dot > 0) dev.name = rname.substring(0, dot);
                }
            }

            if (rtype == 12) {
                int[] tRef = new int[]{rstart};
                String target = decodeName(data, len, tRef, 0);
                if (target.contains("_miplay_lan") || target.contains("_mi-connect")) {
                    matched = true;
                    int dot = target.indexOf('.');
                    if (dot > 0) dev.name = target.substring(0, dot);
                }
            } else if (rtype == 33 && rdlen >= 6) {
                int p = readU16(data, rstart + 4);
                if (p > 0 && rname.contains("_miplay_lan")) dev.cmdPort = p;
            } else if (rtype == 1 && rdlen == 4) {
                dev.ip = (data[rstart] & 0xFF) + "." + (data[rstart + 1] & 0xFF) + "."
                       + (data[rstart + 2] & 0xFF) + "." + (data[rstart + 3] & 0xFF);
            } else if (rtype == 16) {
                int pos = rstart;
                while (pos < rstart + rdlen) {
                    int kvl = data[pos++] & 0xFF;
                    if (pos + kvl > rstart + rdlen) break;
                    String kv = new String(data, pos, kvl, StandardCharsets.UTF_8);
                    pos += kvl;
                    int eq = kv.indexOf('=');
                    if (eq <= 0) continue;
                    String k = kv.substring(0, eq);
                    String v = kv.substring(eq + 1);
                    if ("name".equals(k) && !v.isEmpty()) dev.name = v;
                    else if ("mac".equals(k) || "m".equals(k)) dev.mac = v;
                    else if ("roomName".equals(k)) dev.roomName = v;
                    else if ("appsData".equals(k)) {
                        try {
                            byte[] raw = Base64.decode(v, Base64.DEFAULT);
                            if (raw != null && raw.length >= 7 && (raw[0] & 0xFF) == 0x81) {
                                int cp = ((raw[5] & 0xFF) << 8) | (raw[6] & 0xFF);
                                if (cp > 0 && cp < 65536) dev.cmdPort = cp;
                            }
                        } catch (Exception ignored) {}
                    } else if ("deviceType".equals(k) || "type".equals(k)) {
                        try { dev.deviceType = Integer.parseInt(v); } catch (Exception ignored) {}
                    }
                }
            }
            offRef[0] = rstart + rdlen;
        }

        if (!matched) return;
        if (dev.name == null || dev.name.isEmpty()) dev.name = "MiPlay-" + dev.ip;
        dev.id = (dev.mac != null && !dev.mac.isEmpty()) ? dev.mac : dev.ip;
        dev.lastSeenUs = TimerService.monotonicNowUs();
        if (callback != null) callback.onDeviceFound(dev);
    }
}
