package com.mishare.miplay;

import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 局域网高可靠 mDNS 发现器：
 * - 主力通道：独占随机临时端口 DatagramSocket 发送 RFC 6762 QU (0x8001) 单播响应查询，
 *   绕开系统 5353 端口冲突与 Wi-Fi 组播省电丢包，毫秒级直达回包；
 * - 兜底通道：UDP 5355 + 5353 组播监听，实时接收设备上线广播并兼容老款设备；
 * - 全服务覆盖：_miplay_lan._tcp.local. / _mi-connect._udp.local. / _lyra-mdns._udp.local.，
 *   解析 _mi-connect TXT appsData 真实控制端口、过滤 TTL=0 退网包并按 IP/MAC 智能去重合并；
 * - 无感自动刷新：启动/手动刷新 3 连发 Burst (0/220/650ms)，后台每 3s 轻量无感巡检。
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
        public int namePriority = 0; // 0=兜底, 1=Lyra AppData/实例名, 2=_mi-connect/_miplay_lan 显式名
        public long lastSeenUs;
    }

    public interface Callback {
        void onDeviceFound(DiscoveredDevice dev);
    }

    private static final String MDNS_GROUP = "224.0.0.251";
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final Object burstLock = new Object();
    private long burstGen = 0L;
    private final ConcurrentHashMap<String, DiscoveredDevice> mergedDevices = new ConcurrentHashMap<>();

    private MulticastSocket sock5355;
    private MulticastSocket sock5353;
    private DatagramSocket querySock;
    private Thread th5355;
    private Thread th5353;
    private Thread thUnicastRecv;
    private Thread thQuery;
    private Callback callback;

    public static String pickWifiIpv4() {
        try {
            List<NetworkInterface> ifaces = Collections.list(NetworkInterface.getNetworkInterfaces());
            String fallback = "127.0.0.1";
            for (NetworkInterface nif : ifaces) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                String name = nif.getName().toLowerCase(Locale.US);
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
        querySock = openUnicastQuerySocket();
        if (sock5355 == null && sock5353 == null && querySock == null) return false;

        running.set(true);
        if (sock5355 != null) {
            th5355 = new Thread(() -> recvLoop(sock5355), "MiPlay-mDNS-5355");
            th5355.start();
        }
        if (sock5353 != null) {
            th5353 = new Thread(() -> recvLoop(sock5353), "MiPlay-mDNS-5353");
            th5353.start();
        }
        if (querySock != null) {
            thUnicastRecv = new Thread(() -> recvLoop(querySock), "MiPlay-mDNS-QU");
            thUnicastRecv.start();
        }
        thQuery = new Thread(this::queryLoop, "MiPlay-mDNS-Query");
        thQuery.start();
        return true;
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        synchronized (burstLock) {
            burstGen++;
            burstLock.notifyAll();
        }
        if (sock5355 != null) { try { sock5355.close(); } catch (Exception ignored) {} sock5355 = null; }
        if (sock5353 != null) { try { sock5353.close(); } catch (Exception ignored) {} sock5353 = null; }
        if (querySock != null) { try { querySock.close(); } catch (Exception ignored) {} querySock = null; }
        if (thQuery != null) thQuery.interrupt();
    }

    public void triggerQuery() {
        if (running.get()) {
            synchronized (burstLock) {
                burstGen++;
                burstLock.notifyAll();
            }
        } else {
            sendProbeRound(true);
        }
    }

    private void queryLoop() {
        long handledGen = 0L;
        runBurst();
        int bgTick = 0;
        while (running.get()) {
            boolean manual = false;
            synchronized (burstLock) {
                if (burstGen == handledGen) {
                    try { burstLock.wait(3000); } catch (InterruptedException ignored) { break; }
                }
                if (!running.get()) break;
                if (burstGen != handledGen) {
                    handledGen = burstGen;
                    manual = true;
                }
            }
            if (manual) {
                runBurst();
                bgTick = 0;
            } else {
                bgTick++;
                sendProbeRound((bgTick % 4) == 0);
            }
        }
    }

    private void runBurst() {
        int[] delays = new int[]{0, 220, 430};
        for (int i = 0; i < delays.length && running.get(); i++) {
            if (delays[i] > 0) {
                synchronized (burstLock) {
                    try { burstLock.wait(delays[i]); } catch (InterruptedException ignored) { return; }
                }
                if (!running.get()) return;
            }
            sendProbeRound(i == 0 || i == 2);
        }
    }

    private void sendProbeRound(boolean includeMulticastQm) {
        // 1. 独占临时端口 QU (0x8001) 单播响应查询
        if (querySock != null) {
            sendPtrQuery(querySock, "_miplay_lan._tcp.local", 5355, true);
            sendPtrQuery(querySock, "_mi-connect._udp.local", 5353, true);
            sendPtrQuery(querySock, "_lyra-mdns._udp.local", 5353, true);
            sendPtrQuery(querySock, "_miplay_lan._tcp.local", 5353, true);
        }
        // 2. 标准 5355/5353 端口 QM 组播兜底
        if (includeMulticastQm) {
            sendPtrQuery(sock5355, "_miplay_lan._tcp.local", 5355, false);
            sendPtrQuery(sock5353, "_mi-connect._udp.local", 5353, false);
            sendPtrQuery(sock5353, "_lyra-mdns._udp.local", 5353, false);
            sendPtrQuery(sock5353, "_miplay_lan._tcp.local", 5353, false);
        }
    }

    private DatagramSocket openUnicastQuerySocket() {
        try {
            DatagramSocket s = new DatagramSocket(0);
            s.setSoTimeout(300);
            return s;
        } catch (Exception e) {
            return null;
        }
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

    private void sendPtrQuery(DatagramSocket sock, String domain, int dstPort, boolean requestUnicast) {
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
            bos.write(requestUnicast ? 0x80 : 0x00); bos.write(0x01);
            byte[] pkt = bos.toByteArray();
            sock.send(new DatagramPacket(pkt, pkt.length, InetAddress.getByName(MDNS_GROUP), dstPort));
        } catch (Exception ignored) {}
    }

    private void recvLoop(DatagramSocket sock) {
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

    private static long readU32(byte[] b, int off) {
        return ((b[off] & 0xFFL) << 24)
                | ((b[off + 1] & 0xFFL) << 16)
                | ((b[off + 2] & 0xFFL) << 8)
                | (b[off + 3] & 0xFFL);
    }

    private static boolean isDedicatedMiPlayService(String raw) {
        if (raw == null) return false;
        String low = raw.toLowerCase(Locale.US);
        return low.contains("_miplay_lan") || low.contains("_mi-connect");
    }

    private static boolean isMiPlayService(String raw) {
        if (raw == null) return false;
        String low = raw.toLowerCase(Locale.US);
        return low.contains("_miplay_lan") || low.contains("_mi-connect") || low.contains("_lyra-mdns");
    }

    private static boolean isValidUtf8Printable(byte[] b, int off, int len) {
        if (b == null || len <= 0 || len > 128 || off + len > b.length) return false;
        int i = off;
        int end = off + len;
        boolean hasVisible = false;
        while (i < end) {
            int c = b[i] & 0xFF;
            if (c < 0x20 || c == 0x7F) return false;
            if (c < 0x80) {
                if (c != ' ') hasVisible = true;
                i++;
            } else if ((c & 0xE0) == 0xC0) {
                if (i + 1 >= end || (b[i + 1] & 0xC0) != 0x80) return false;
                hasVisible = true;
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                if (i + 2 >= end || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80) return false;
                hasVisible = true;
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                if (i + 3 >= end || (b[i + 1] & 0xC0) != 0x80
                        || (b[i + 2] & 0xC0) != 0x80 || (b[i + 3] & 0xC0) != 0x80) return false;
                hasVisible = true;
                i += 4;
            } else {
                return false;
            }
        }
        return hasVisible;
    }

    private static String parseLyraAppDataName(byte[] raw) {
        if (raw == null || raw.length < 10) return "";
        int[] offsets = new int[]{7, 9, 8, 6, 10, 11, 12, 5, 13, 14, 15, 16};
        String fallback = "";
        for (int start : offsets) {
            if (start + 3 > raw.length) continue;
            int pos = start;
            boolean ok = true;
            boolean hasAnchor = false;
            String found = "";
            while (pos < raw.length) {
                if (pos + 2 > raw.length) { ok = false; break; }
                int tag = raw[pos] & 0xFF;
                int len = raw[pos + 1] & 0xFF;
                pos += 2;
                if (pos + len > raw.length) { ok = false; break; }
                if (tag == 0x00 || tag == 0x0A) hasAnchor = true;
                if (tag == 0x02 && len >= 2 && isValidUtf8Printable(raw, pos, len)) {
                    found = new String(raw, pos, len, StandardCharsets.UTF_8);
                }
                pos += len;
            }
            if (ok && pos == raw.length && !found.isEmpty()) {
                if (hasAnchor) return found;
                if (fallback.isEmpty()) fallback = found;
            }
        }
        return fallback;
    }

    private static boolean isUsableIp(String ip) {
        return ip != null && !ip.isEmpty()
                && !"0.0.0.0".equals(ip)
                && !ip.startsWith("127.")
                && !ip.startsWith("169.254.")
                && !ip.startsWith("198.18.")
                && !ip.startsWith("198.19.");
    }

    private static boolean isHexIdName(String name) {
        if (name == null || name.length() != 8) return false;
        for (int i = 0; i < 8; i++) {
            char c = name.charAt(i);
            boolean hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            if (!hex) return false;
        }
        return true;
    }

    private static String cleanInstanceLabel(String raw) {
        if (raw == null) return "";
        int lp = raw.indexOf('(');
        if (lp > 0 && raw.endsWith(")")) {
            return raw.substring(0, lp);
        }
        return raw;
    }

    private static boolean isGenericName(String name, String ip) {
        return name == null || name.isEmpty() || name.startsWith("_")
                || name.equals("MiPlay-" + ip) || isHexIdName(name);
    }

    private static boolean appsListContainsMiPlay(String appsVal) {
        if (appsVal == null) return false;
        int i = 0;
        while (i < appsVal.length()) {
            while (i < appsVal.length() && !Character.isDigit(appsVal.charAt(i))) i++;
            if (i >= appsVal.length()) break;
            int start = i;
            while (i < appsVal.length() && Character.isDigit(appsVal.charAt(i))) i++;
            if ("5".equals(appsVal.substring(start, i))) return true;
        }
        return false;
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
        boolean matchedDedicated = false;
        boolean hasLiveRr = false;
        boolean explicitNameFromTxt = false;
        boolean explicitPortFromTxt = false;
        boolean hasAppsField = false;
        boolean appsHasMiPlay = false;
        boolean hasMiPlayAppsData = false;
        String fallbackAIp = null;

        int total = an + ns + ar;
        for (int i = 0; i < total && offRef[0] + 10 <= len; i++) {
            String rname = decodeName(data, len, offRef, 0);
            if (offRef[0] + 10 > len) break;
            int rtype = readU16(data, offRef[0]);
            long ttl = readU32(data, offRef[0] + 4);
            int rdlen = readU16(data, offRef[0] + 8);
            offRef[0] += 10;
            int rstart = offRef[0];
            if (rstart + rdlen > len) break;
            offRef[0] = rstart + rdlen;

            if (ttl == 0) continue; // 忽略 Goodbye 退网记录

            boolean rnameMiPlay = isMiPlayService(rname);
            if (rnameMiPlay) {
                matched = true;
                if (isDedicatedMiPlayService(rname)) matchedDedicated = true;
                hasLiveRr = true;
                if (!explicitNameFromTxt && isGenericName(dev.name, dev.ip) && !rname.startsWith("_")) {
                    int dot = rname.indexOf('.');
                    if (dot > 0) {
                        String cleaned = cleanInstanceLabel(rname.substring(0, dot));
                        if (!isHexIdName(cleaned)) dev.name = cleaned;
                    }
                }
            }

            if (rtype == 12) {
                int[] tRef = new int[]{rstart};
                String target = decodeName(data, len, tRef, 0);
                if (isMiPlayService(target)) {
                    matched = true;
                    if (isDedicatedMiPlayService(target)) matchedDedicated = true;
                    hasLiveRr = true;
                    int dot = target.indexOf('.');
                    if (dot > 0 && !target.startsWith("_") && !explicitNameFromTxt && isGenericName(dev.name, dev.ip)) {
                        String cleaned = cleanInstanceLabel(target.substring(0, dot));
                        if (!isHexIdName(cleaned)) dev.name = cleaned;
                    }
                }
            } else if (rtype == 33 && rdlen >= 6) {
                int p = readU16(data, rstart + 4);
                if (p > 0 && !explicitPortFromTxt && rname.toLowerCase(Locale.US).contains("_miplay_lan")) {
                    dev.cmdPort = p;
                }
            } else if (rtype == 1 && rdlen == 4) {
                String aIp = (data[rstart] & 0xFF) + "." + (data[rstart + 1] & 0xFF) + "."
                        + (data[rstart + 2] & 0xFF) + "." + (data[rstart + 3] & 0xFF);
                if (isUsableIp(aIp)) fallbackAIp = aIp;
            } else if (rtype == 16 && rnameMiPlay) {
                int pos = rstart;
                while (pos < rstart + rdlen) {
                    int kvl = data[pos++] & 0xFF;
                    if (pos + kvl > rstart + rdlen) break;
                    String kv = new String(data, pos, kvl, StandardCharsets.UTF_8);
                    pos += kvl;
                    int eq = kv.indexOf('=');
                    if (eq <= 0) continue;
                    String k = kv.substring(0, eq).toLowerCase(Locale.US);
                    String v = kv.substring(eq + 1);
                    if (("name".equals(k) || "dn".equals(k) || "devicename".equals(k)) && !v.isEmpty()) {
                        dev.name = v;
                        explicitNameFromTxt = true;
                    } else if ("mac".equals(k) || "m".equals(k)) {
                        if (!v.isEmpty()) dev.mac = v;
                    } else if ("roomname".equals(k)) {
                        dev.roomName = v;
                    } else if ("apps".equals(k)) {
                        hasAppsField = true;
                        if (appsListContainsMiPlay(v)) appsHasMiPlay = true;
                    } else if ("port".equals(k) || "po".equals(k)) {
                        try {
                            int cp = Integer.parseInt(v);
                            if (cp > 0 && cp < 65536) {
                                dev.cmdPort = cp;
                                explicitPortFromTxt = true;
                            }
                        } catch (Exception ignored) {}
                    } else if ("appsdata".equals(k)) {
                        try {
                            byte[] raw = Base64.decode(v, Base64.DEFAULT);
                            if (raw != null && raw.length >= 7 && (raw[0] & 0xFF) == 0x81) {
                                int cp = ((raw[5] & 0xFF) << 8) | (raw[6] & 0xFF);
                                if (cp > 0 && cp < 65536) {
                                    dev.cmdPort = cp;
                                    explicitPortFromTxt = true;
                                    hasMiPlayAppsData = true;
                                }
                            }
                        } catch (Exception ignored) {}
                    } else if ("appdata".equals(k) && !v.isEmpty()) {
                        try {
                            byte[] raw = Base64.decode(v, Base64.DEFAULT);
                            String lyraName = parseLyraAppDataName(raw);
                            if (!lyraName.isEmpty()) {
                                dev.name = lyraName;
                                explicitNameFromTxt = true;
                            }
                        } catch (Exception ignored) {}
                    } else if ("devicetype".equals(k) || "type".equals(k) || "dev".equals(k)) {
                        try { dev.deviceType = Integer.parseInt(v); } catch (Exception ignored) {}
                    }
                }
            }
        }

        if (!matched || !hasLiveRr) return;
        if (hasAppsField && !appsHasMiPlay && !hasMiPlayAppsData) return;
        if (!isUsableIp(dev.ip) && fallbackAIp != null) dev.ip = fallbackAIp;
        if (!isUsableIp(dev.ip)) return;
        if (dev.name == null || dev.name.isEmpty()) {
            dev.name = "MiPlay-" + dev.ip;
            dev.namePriority = 0;
        } else if (matchedDedicated && explicitNameFromTxt) {
            dev.namePriority = 2;
        } else {
            dev.namePriority = 1;
        }
        dev.id = (dev.mac != null && !dev.mac.isEmpty()) ? dev.mac : dev.ip;
        dev.lastSeenUs = TimerService.monotonicNowUs();

        // 按 id 或同 IP 智能合并，并按名称优先级防止 _lyra-mdns 覆盖 _mi-connect 显式真名
        DiscoveredDevice merged = dev;
        synchronized (mergedDevices) {
            DiscoveredDevice existing = mergedDevices.get(dev.id);
            if (existing == null) {
                for (Map.Entry<String, DiscoveredDevice> e : mergedDevices.entrySet()) {
                    if (dev.ip.equals(e.getValue().ip)) {
                        existing = e.getValue();
                        break;
                    }
                }
            }
            if (existing != null) {
                existing.ip = dev.ip;
                existing.lastSeenUs = dev.lastSeenUs;
                boolean curGeneric = isGenericName(existing.name, existing.ip);
                boolean incGeneric = isGenericName(dev.name, dev.ip);
                if (!incGeneric && (curGeneric || dev.namePriority >= existing.namePriority)) {
                    existing.name = dev.name;
                    existing.namePriority = dev.namePriority;
                }
                if (dev.mac != null && !dev.mac.isEmpty()) existing.mac = dev.mac;
                if (dev.roomName != null && !dev.roomName.isEmpty()) existing.roomName = dev.roomName;
                if (dev.cmdPort != 8899 || existing.cmdPort <= 0) existing.cmdPort = dev.cmdPort;
                if (dev.deviceType != 4) existing.deviceType = dev.deviceType;
                merged = existing;
            } else {
                mergedDevices.put(dev.id, dev);
            }
        }
        if (callback != null) callback.onDeviceFound(merged);
    }
}
