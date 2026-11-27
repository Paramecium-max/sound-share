package io.soundflow.sender;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.StandardProtocolFamily;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.DatagramChannel;
import java.nio.channels.MembershipKey;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public class MdnsDiscovery {
    enum PortSource { DEFAULT, SRV, TXT, APPS_DATA, MANUAL }

    public static class DiscoveredDevice {
        public String id;
        public String name;
        public String ip;
        public int cmdPort = 8899;
        PortSource portSource = PortSource.DEFAULT;
        boolean hasExplicitPort() { return portSource != PortSource.DEFAULT; }
        public boolean online = true;
        public String idHash = "";
        public long revision;
        public String mac = "";
        public String roomName = "";
        public int deviceType = 4;
        public boolean isManual = false;
        public int namePriority = 0; // 0=兜底, 1=Lyra AppData/实例名, 2=_mi-connect/_miplay_lan 显式名
        public long lastSeenUs;

        DiscoveredDevice copy() {
            DiscoveredDevice d = new DiscoveredDevice();
            d.id = id;
            d.name = name;
            d.ip = ip;
            d.cmdPort = cmdPort;
            d.portSource = portSource;
            d.online = online;
            d.mac = mac;
            d.idHash = idHash;
            d.roomName = roomName;
            d.deviceType = deviceType;
            d.isManual = isManual;
            d.namePriority = namePriority;
            d.lastSeenUs = lastSeenUs;
            d.revision = revision;
            return d;
        }
    }

    public interface Callback {
        void onDeviceFound(DiscoveredDevice dev);
    }

    private static final String MDNS_GROUP = "224.0.0.251";
    private static final int MIPLAY_PORT = 5355;
    private static final int STANDARD_PORT = 5353;
    private static final String LAN_SERVICE = "_miplay_lan._tcp.local";
    private static final String CONNECT_SERVICE = "_mi-connect._udp.local";
    private static final String LYRA_SERVICE = "_lyra-mdns._udp.local";
    private final AtomicBoolean running = new AtomicBoolean();
    private final AtomicBoolean autoQuery = new AtomicBoolean();
    private final AtomicBoolean manualQuery = new AtomicBoolean();
    private final AtomicBoolean interfacesChanged = new AtomicBoolean();
    private volatile boolean bursting;
    private volatile Selector selector;
    private Thread worker;
    private final DeviceCache cache = new DeviceCache();

    static Map<Inet4Address, NetworkInterface> ipv4Interfaces() {
        Map<Inet4Address, NetworkInterface> addresses = new HashMap<>();
        try {
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp()) continue;
                for (InetAddress a : Collections.list(nif.getInetAddresses())) {
                    if (a instanceof Inet4Address) addresses.put((Inet4Address) a, nif);
                }
            }
        } catch (Exception ignored) {}
        return addresses;
    }

    public static String pickWifiIpv4() {
        String fallback = "127.0.0.1";
        for (Map.Entry<Inet4Address, NetworkInterface> entry : ipv4Interfaces().entrySet()) {
            String ip = entry.getKey().getHostAddress();
            if (!isUsableIp(ip)) continue;
            String name = entry.getValue().getName().toLowerCase(Locale.US);
            if (name.startsWith("wlan") || name.startsWith("eth")) return ip;
            fallback = ip;
        }
        return fallback;
    }

    public static boolean isSelfLocalIp(String ip) {
        if (ip == null || ip.trim().isEmpty()) return false;
        String clean = ip.trim();
        if ("127.0.0.1".equals(clean) || "0.0.0.0".equals(clean) || "localhost".equalsIgnoreCase(clean)) {
            return true;
        }
        for (Inet4Address local : ipv4Interfaces().keySet()) {
            if (clean.equals(local.getHostAddress())) return true;
        }
        return false;
    }

    public synchronized boolean start(Callback callback, Runnable onInterfacesChanged) {
        if (running.get()) return true;
        try {
            selector = Selector.open();
        } catch (IOException e) {
            return false;
        }
        autoQuery.set(false);
        manualQuery.set(false);
        interfacesChanged.set(false);
        bursting = true;
        running.set(true);
        worker = new Thread(() -> runLoop(callback, onInterfacesChanged), "MiPlay-mDNS");
        worker.start();
        return true;
    }

    public synchronized void stop() {
        running.set(false);
        wake();
        if (worker == null) return;
        boolean interrupted = false;
        while (worker.isAlive()) {
            try { worker.join(); } catch (InterruptedException e) { interrupted = true; }
        }
        worker = null;
        if (interrupted) Thread.currentThread().interrupt();
    }

    private void wake() {
        Selector current = selector;
        if (current != null) current.wakeup();
    }

    public void triggerQuery(boolean automatic) {
        if (!running.get() || (automatic && bursting)) return;
        (automatic ? autoQuery : manualQuery).set(true);
        wake();
    }

    public void onNetworkChanged() {
        interfacesChanged.set(true);
        wake();
    }

    private static final class Sockets {
        private final Selector selector;
        private final InetAddress group;
        private final Map<Inet4Address, DatagramChannel> queries = new HashMap<>();
        private final Map<Integer, DatagramChannel> listeners = new HashMap<>();
        private final Map<Integer, Map<NetworkInterface, MembershipKey>> memberships = new HashMap<>();
        private Map<Inet4Address, NetworkInterface> local = new HashMap<>();

        Sockets(Selector selector, InetAddress group) {
            this.selector = selector;
            this.group = group;
        }

        private DatagramChannel open(InetSocketAddress address, NetworkInterface nif) throws IOException {
            DatagramChannel channel = DatagramChannel.open(StandardProtocolFamily.INET);
            boolean keep = false;
            try {
                if (address.getPort() != 0) channel.setOption(StandardSocketOptions.SO_REUSEADDR, true);
                channel.setOption(StandardSocketOptions.IP_MULTICAST_TTL, 255);
                if (nif != null) channel.setOption(StandardSocketOptions.IP_MULTICAST_IF, nif);
                channel.configureBlocking(false);
                channel.bind(address);
                channel.register(selector, SelectionKey.OP_READ);
                keep = true;
                return channel;
            } finally {
                if (!keep) close(channel);
            }
        }

        boolean refresh(boolean networkEvent) {
            Map<Inet4Address, NetworkInterface> current = ipv4Interfaces();
            boolean changed = networkEvent || !current.equals(local);
            for (Iterator<Map.Entry<Inet4Address, DatagramChannel>> it = queries.entrySet().iterator();
                 it.hasNext();) {
                Map.Entry<Inet4Address, DatagramChannel> entry = it.next();
                NetworkInterface previous = local.get(entry.getKey());
                if (networkEvent || !current.containsKey(entry.getKey()) || !current.get(entry.getKey()).equals(previous)) {
                    close(entry.getValue());
                    it.remove();
                }
            }
            local = current;
            Set<NetworkInterface> eligible = new HashSet<>();
            for (Map.Entry<Inet4Address, NetworkInterface> entry : local.entrySet()) {
                Inet4Address ip = entry.getKey();
                if (!isUsableIp(ip.getHostAddress())) continue;
                NetworkInterface nif = entry.getValue();
                eligible.add(nif);
                if (!queries.containsKey(ip)) {
                    try { queries.put(ip, open(new InetSocketAddress(ip, 0), nif)); }
                    catch (IOException | RuntimeException ignored) {}
                }
            }
            for (int port : new int[]{MIPLAY_PORT, STANDARD_PORT}) {
                Map<NetworkInterface, MembershipKey> joined = memberships.computeIfAbsent(port, k -> new HashMap<>());
                DatagramChannel channel = listeners.get(port);
                if (channel != null) {
                    try {
                        for (Iterator<Map.Entry<NetworkInterface, MembershipKey>> it = joined.entrySet().iterator();
                             it.hasNext();) {
                            Map.Entry<NetworkInterface, MembershipKey> membership = it.next();
                            if (changed || !eligible.contains(membership.getKey())) {
                                membership.getValue().drop();
                                it.remove();
                            }
                        }
                    } catch (RuntimeException e) {
                        close(channel);
                        listeners.remove(port);
                        joined.clear();
                        channel = null;
                    }
                }
                if (channel == null) {
                    try {
                        channel = open(new InetSocketAddress(port), null);
                        listeners.put(port, channel);
                    } catch (IOException | RuntimeException e) { continue; }
                }
                for (NetworkInterface nif : eligible) {
                    if (!joined.containsKey(nif)) {
                        try { joined.put(nif, channel.join(group, nif)); }
                        catch (IOException | RuntimeException ignored) {}
                    }
                }
            }
            return changed;
        }

        void probe(boolean qm) {
            for (Iterator<DatagramChannel> it = queries.values().iterator(); it.hasNext();) {
                DatagramChannel channel = it.next();
                try {
                    query(channel, LAN_SERVICE, MIPLAY_PORT, true);
                    standardQueries(channel, true);
                } catch (IOException e) {
                    // The next interface pass retries sockets whose routes disappeared.
                    close(channel);
                    it.remove();
                }
            }
            if (!qm) return;
            for (Map.Entry<Integer, DatagramChannel> e : listeners.entrySet()) {
                int port = e.getKey();
                DatagramChannel channel = e.getValue();
                for (NetworkInterface nif : memberships.get(port).keySet()) {
                    try {
                        channel.setOption(StandardSocketOptions.IP_MULTICAST_IF, nif);
                        if (port == MIPLAY_PORT) query(channel, LAN_SERVICE, port, false);
                        else standardQueries(channel, false);
                    } catch (IOException | RuntimeException ignored) {}
                }
            }
        }

        private void standardQueries(DatagramChannel channel, boolean qu) throws IOException {
            query(channel, CONNECT_SERVICE, STANDARD_PORT, qu);
            query(channel, LYRA_SERVICE, STANDARD_PORT, qu);
            query(channel, LAN_SERVICE, STANDARD_PORT, qu);
        }

        private void query(DatagramChannel channel, String service, int port, boolean qu) throws IOException {
            byte[] packet = buildQuery(service, qu);
            if (channel.send(ByteBuffer.wrap(packet), new InetSocketAddress(group, port)) != packet.length) {
                throw new IOException("Incomplete mDNS query");
            }
        }

        void close() {
            for (DatagramChannel c : queries.values()) close(c);
            for (DatagramChannel c : listeners.values()) close(c);
        }

        private static void close(DatagramChannel channel) {
            try { channel.close(); } catch (IOException ignored) {}
        }
    }

    static byte[] buildQuery(String domain, boolean requestUnicast) {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] header = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
        bytes.write(header, 0, header.length);
        for (String label : domain.split("\\.")) {
            byte[] data = label.getBytes(StandardCharsets.US_ASCII);
            bytes.write(data.length);
            bytes.write(data, 0, data.length);
        }
        bytes.write(0);
        bytes.write(0);
        bytes.write(12);
        bytes.write(requestUnicast ? 0x80 : 0);
        bytes.write(1);
        return bytes.toByteArray();
    }

    static final class ProbeBurst {
        private static final long[] OFFSETS = {0L, 220_000_000L, 650_000_000L};
        private int step = OFFSETS.length;
        private long origin;

        boolean active() { return step < OFFSETS.length; }
        boolean begin(long now) {
            if (active()) return false;
            origin = now;
            step = 0;
            return true;
        }
        long deadline() { return origin + OFFSETS[step]; }
        int takeDue(long now) { return active() && now >= deadline() ? step++ : -1; }
    }

    private void runLoop(Callback callback, Runnable onInterfacesChanged) {
        Sockets sockets = null;
        try (Selector loop = selector) {
            sockets = new Sockets(loop, InetAddress.getByName(MDNS_GROUP));
            if (sockets.refresh(false)) onInterfacesChanged.run();
            final ByteBuffer packet = ByteBuffer.allocate(4096);
            ProbeBurst burst = new ProbeBurst();
            burst.begin(System.nanoTime());
            long nextPass = System.nanoTime();
            int backgroundRound = 0;
            while (running.get()) {
                boolean automatic = autoQuery.getAndSet(false);
                boolean manual = manualQuery.getAndSet(false);
                boolean network = interfacesChanged.getAndSet(false);
                if (!burst.active() && (automatic || manual || network)) {
                    bursting = true;
                    if (sockets.refresh(network)) onInterfacesChanged.run();
                    burst.begin(System.nanoTime());
                } else if (burst.active() && (manual || network)) {
                    if (sockets.refresh(network)) onInterfacesChanged.run();
                    if (System.nanoTime() < burst.deadline()) sockets.probe(true);
                }
                long now = System.nanoTime();
                long next = nextPass;
                if (burst.active()) {
                    int step = burst.takeDue(now);
                    if (step >= 0) {
                        sockets.probe(step != 1);
                        if (!burst.active()) {
                            bursting = false;
                            backgroundRound = 0;
                            nextPass = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                        }
                    }
                    next = burst.active() ? burst.deadline() : nextPass;
                } else if (now >= nextPass) {
                    boolean changed = sockets.refresh(false);
                    if (changed) onInterfacesChanged.run();
                    sockets.probe(++backgroundRound % 4 == 0 || changed);
                    nextPass = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    next = nextPass;
                }
                publish(callback, Collections.emptyList());
                long remaining = Math.max(1L, TimeUnit.NANOSECONDS.toMillis(next - System.nanoTime()));
                loop.select(Math.min(remaining, 250L));
                for (Iterator<SelectionKey> it = loop.selectedKeys().iterator(); it.hasNext();) {
                    SelectionKey key = it.next();
                    it.remove();
                    if (!key.isValid() || !key.isReadable()) continue;
                    DatagramChannel channel = (DatagramChannel) key.channel();
                    for (int i = 0; i < 8 && running.get(); i++) {
                        packet.clear();
                        InetSocketAddress peer;
                        try { peer = (InetSocketAddress) channel.receive(packet); }
                        catch (IOException ignored) { break; }
                        if (peer == null) break;
                        if (sockets.local.containsKey(peer.getAddress())) continue;
                        List<Announcement> records = parseAnnouncements(packet.array(), packet.position(),
                                                                         peer.getAddress().getHostAddress());
                        Sockets current = sockets;
                        records.removeIf(r -> r.ttl != 0 && current.local.keySet().stream()
                                .anyMatch(ip -> ip.getHostAddress().equals(r.device.ip)));
                        publish(callback, records);
                    }
                }
            }
        } catch (IOException e) {
            // start() can retry after the loop has retired its channels.
        } finally {
            if (sockets != null) sockets.close();
            selector = null;
            running.set(false);
        }
    }

    private void publish(Callback callback, List<Announcement> records) {
        for (DiscoveredDevice d : cache.update(records, TimerService.monotonicNowUs())) {
            if (!running.get()) break;
            callback.onDeviceFound(d);
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

    private static String strictUtf8(byte[] b, int off, int len) {
        try {
            return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(b, off, len)).toString();
        } catch (CharacterCodingException e) {
            return null;
        }
    }

    private static boolean isValidUtf8Printable(byte[] b, int off, int len) {
        if (len <= 0 || len > 128) return false;
        String text = strictUtf8(b, off, len);
        if (text == null || text.trim().isEmpty()) return false;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) < 32 || text.charAt(i) == 127) return false;
        }
        return true;
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
                && ip.charAt(0) != ':'
                && ip.indexOf(':') < 0
                && !"0.0.0.0".equals(ip)
                && !"255.255.255.255".equals(ip)
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

    private static String asciiLower(String value) {
        StringBuilder out = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            out.append(c >= 'A' && c <= 'Z' ? (char) (c + 'a' - 'A') : c);
        }
        return out.toString();
    }

    private static List<String> decodeName(byte[] data, int len, int[] offset, int end) {
        List<String> labels = new ArrayList<>();
        int pos = offset[0], expanded = 1, pointers = 0;
        boolean jumped = false;
        while (pos < end) {
            int size = data[pos++] & 0xff;
            if (size == 0) {
                if (!jumped) offset[0] = pos;
                return labels;
            }
            if ((size & 0xc0) == 0xc0) {
                if (pos >= end || ++pointers > 16) return null;
                int pointer = ((size & 0x3f) << 8) | (data[pos++] & 0xff);
                if (pointer < 12 || pointer >= pos - 2) return null;
                if (!jumped) offset[0] = pos;
                jumped = true;
                end = len;
                pos = pointer;
                continue;
            }
            if (size > 63 || size > end - pos || (expanded += size + 1) > 255) return null;
            // Preserve arbitrary label bytes and boundaries for lookups.
            labels.add(new String(data, pos, size, StandardCharsets.ISO_8859_1));
            pos += size;
        }
        return null;
    }

    private static String serviceType(List<String> name, int prefix) {
        if (name.size() != prefix + 3) return "";
        String service = asciiLower(String.join(".", name.subList(prefix, name.size())));
        // Compare labels independently so a literal dot inside a label cannot act as a separator.
        if (!asciiLower(name.get(prefix + 2)).equals("local") ||
            name.get(prefix).contains(".") || name.get(prefix + 1).contains(".")) return "";
        return service.equals(LAN_SERVICE) || service.equals(CONNECT_SERVICE) || service.equals(LYRA_SERVICE)
                ? service : "";
    }

    private static String nameKey(List<String> name) {
        StringBuilder key = new StringBuilder();
        for (String label : name) key.append((char) label.length()).append(asciiLower(label));
        return key.toString();
    }

    private static byte[] decodeBase64(String value) {
        if (value.isEmpty()) return new byte[0];
        try {
            String standard = value.replace('-', '+').replace('_', '/');
            byte[] decoded = Base64.getDecoder().decode(standard);
            String canonical = Base64.getEncoder().encodeToString(decoded);
            if (!standard.equals(canonical) && !standard.equals(canonical.replace("=", ""))) return null;
            return decoded;
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    private static String macKey(String mac) {
        String key = asciiLower(mac.replace(":", "").replace("-", ""));
        return key.length() == 12 && key.chars().allMatch(c ->
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) &&
                !key.equals("000000000000") && !key.equals("ffffffffffff") ? key : "";
    }

    private static String hashKey(String hash) {
        return hash.length() >= 8 && hash.matches("[0-9A-Za-z+/=_-]+") ? "hash:" + hash : "";
    }

    private static int portNumber(String value) {
        if (value.isEmpty()) return 0;
        int result = 0;
        for (int i = 0; i < value.length(); i++) {
            int digit = value.charAt(i) - '0';
            if (digit < 0 || digit > 9) return 0;
            result = result * 10 + digit;
            if (result > 65535) return 0;
        }
        return result;
    }

    private static final class Instance {
        String label = "";
        String service = "";
        String host = "";
        int srvPort;
        long ttl = Long.MAX_VALUE;
        boolean goodbye;
        boolean invalid;
        final Map<String, String> txt = new HashMap<>();

        String text(String... keys) {
            for (String key : keys) {
                if (txt.containsKey(key)) return txt.get(key);
            }
            return "";
        }
    }

    private static Instance instanceFor(Map<String, Instance> instances, List<String> name, String service, long ttl) {
        Instance instance = instances.computeIfAbsent(nameKey(name), key -> new Instance());
        instance.label = name.get(0);
        instance.service = service;
        if (ttl == 0) instance.goodbye = true;
        else instance.ttl = Math.min(instance.ttl, ttl);
        return instance;
    }

    private static final class AddressRecord {
        final String ip;
        final long ttl;

        AddressRecord(String ip, long ttl) {
            this.ip = ip;
            this.ttl = ttl;
        }
    }

    static final class Announcement {
        final String instance;
        final String sourceIp;
        final long ttl;
        final DiscoveredDevice device;

        Announcement(String instance, String sourceIp, long ttl, DiscoveredDevice device) {
            this.instance = instance;
            this.sourceIp = sourceIp;
            this.ttl = ttl;
            this.device = device;
        }
    }

    static List<Announcement> parseAnnouncements(byte[] data, int len, String srcIp) {
        List<Announcement> out = new ArrayList<>();
        if (len < 12 || len > data.length || (data[2] & 0x80) == 0) return out;
        int[] off = {12};
        for (int i = 0; i < readU16(data, 4); i++) {
            if (decodeName(data, len, off, len) == null || len - off[0] < 4) return out;
            off[0] += 4;
        }
        Map<String, Instance> instances = new TreeMap<>();
        Map<String, List<AddressRecord>> addresses = new HashMap<>();
        int records = readU16(data, 6) + readU16(data, 8) + readU16(data, 10);
        for (int i = 0; i < records; i++) {
            List<String> owner = decodeName(data, len, off, len);
            if (owner == null || len - off[0] < 10) return out;
            int type = readU16(data, off[0]);
            int rrclass = readU16(data, off[0] + 2) & 0x7fff;
            long ttl = readU32(data, off[0] + 4);
            int size = readU16(data, off[0] + 8);
            int pos = off[0] + 10;
            if (size > len - pos) return out;
            int end = pos + size;
            off[0] = end;
            if (rrclass != 1) continue;
            if (type == 12) {
                String service = serviceType(owner, 0);
                if (service.isEmpty()) continue;
                int[] nameEnd = {pos};
                List<String> target = decodeName(data, len, nameEnd, end);
                if (target == null || nameEnd[0] != end) return out;
                if (service.equals(serviceType(target, 1))) instanceFor(instances, target, service, ttl);
                continue;
            }
            if (type == 1 && size == 4 && ttl != 0) {
                String ip = (data[pos] & 0xff) + "." + (data[pos + 1] & 0xff) + "." +
                            (data[pos + 2] & 0xff) + "." + (data[pos + 3] & 0xff);
                if (isUsableIp(ip)) addresses.computeIfAbsent(nameKey(owner), key -> new ArrayList<>())
                                             .add(new AddressRecord(ip, ttl));
                continue;
            }
            String service = serviceType(owner, 1);
            if (service.isEmpty() || (type != 33 && type != 16)) continue;
            Instance instance = instanceFor(instances, owner, service, ttl);
            if (ttl == 0) continue;
            if (type == 33) {
                if (size < 7) return out;
                int port = readU16(data, pos + 4);
                int[] nameEnd = {pos + 6};
                List<String> host = decodeName(data, len, nameEnd, end);
                if (host == null || host.isEmpty() || nameEnd[0] != end) return out;
                String hostKey = nameKey(host);
                if (!instance.host.isEmpty() && (!instance.host.equals(hostKey) || instance.srvPort != port)) {
                    instance.invalid = true;
                }
                instance.host = hostKey;
                instance.srvPort = port;
                continue;
            }
            while (pos < end) {
                int length = data[pos++] & 0xff;
                if (length > end - pos) return out;
                String value = strictUtf8(data, pos, length);
                pos += length;
                if (value == null) {
                    instance.invalid = true;
                    continue;
                }
                int eq = value.indexOf('=');
                if (eq <= 0) continue;
                String key = asciiLower(value.substring(0, eq));
                String text = value.substring(eq + 1);
                String old = instance.txt.putIfAbsent(key, text);
                if (old != null && !old.equals(text)) instance.invalid = true;
            }
        }
        for (Map.Entry<String, Instance> e : instances.entrySet()) {
            Instance instance = e.getValue();
            if (instance.invalid) continue;
            if (instance.goodbye && instance.ttl == Long.MAX_VALUE) {
                out.add(new Announcement(e.getKey(), srcIp, 0, null));
                continue;
            }
            DiscoveredDevice d = new DiscoveredDevice();
            d.ip = srcIp;
            AddressRecord chosen = null;
            for (AddressRecord address : addresses.getOrDefault(instance.host, Collections.emptyList())) {
                if (chosen == null || (!chosen.ip.equals(srcIp) &&
                    (address.ip.equals(srcIp) || address.ip.compareTo(chosen.ip) < 0))) chosen = address;
            }
            if (chosen != null) {
                d.ip = chosen.ip;
                instance.ttl = Math.min(instance.ttl, chosen.ttl);
            }
            if (!isUsableIp(d.ip)) continue;

            d.name = instance.text("name", "dn", "devicename");
            if (instance.service.equals(LYRA_SERVICE)) {
                byte[] raw = decodeBase64(instance.text("appdata"));
                if (raw == null) continue;
                String lyra = parseLyraAppDataName(raw);
                if (!lyra.isEmpty()) d.name = lyra;
            }
            if (!isGenericName(d.name, d.ip)) {
                d.namePriority = instance.service.equals(LYRA_SERVICE) ? 1 : 2;
            } else {
                byte[] raw = instance.label.getBytes(StandardCharsets.ISO_8859_1);
                String label = isValidUtf8Printable(raw, 0, raw.length)
                        ? cleanInstanceLabel(strictUtf8(raw, 0, raw.length)) : "";
                if (!isGenericName(label, d.ip)) {
                    d.name = label;
                    d.namePriority = 1;
                } else {
                    d.name = "MiPlay-" + d.ip;
                }
            }

            String textPort = instance.text("port", "po");
            int port = portNumber(textPort);
            if (!textPort.isEmpty() && port == 0) continue;
            byte[] app = decodeBase64(instance.text("appsdata"));
            if (app == null) continue;
            int appPort = app.length >= 7 && (app[0] & 0xff) == 0x81 ? readU16(app, 5) : 0;
            if (instance.service.equals(CONNECT_SERVICE) && instance.txt.containsKey("apps") &&
                !appsListContainsMiPlay(instance.text("apps")) && appPort == 0) continue;
            if (appPort != 0) {
                port = appPort;
                d.portSource = PortSource.APPS_DATA;
            } else if (port != 0) {
                d.portSource = PortSource.TXT;
            } else if (instance.service.equals(LAN_SERVICE) && instance.srvPort != 0) {
                port = instance.srvPort;
                d.portSource = PortSource.SRV;
            }
            if (port != 0) d.cmdPort = port;
            d.mac = instance.text("mac", "m");
            d.idHash = instance.text("idhash");
            d.roomName = instance.text("roomname");
            try { d.deviceType = Integer.parseInt(instance.text("devicetype", "type", "dev")); }
            catch (NumberFormatException ignored) {}
            d.id = macKey(d.mac).isEmpty() ? d.ip : d.mac;
            out.add(new Announcement(e.getKey(), srcIp, instance.ttl, d));
        }
        return out;
    }

    static final class DeviceCache {
        private static final class Service {
            final Announcement announcement;
            final DiscoveredDevice device;
            final long expiry;
            Service(Announcement announcement, DiscoveredDevice device, long expiry) {
                this.announcement = announcement;
                this.device = device;
                this.expiry = expiry;
            }
        }

        private final Map<String, Service> services = new HashMap<>();
        private final Map<String, DiscoveredDevice> devices = new TreeMap<>();
        private final Map<String, String> aliases = new HashMap<>();
        private long revision;

        List<DiscoveredDevice> update(List<Announcement> announcements, long now) {
            Set<String> dirty = new HashSet<>();
            services.values().removeIf(s -> {
                if (s.expiry > now) return false;
                dirty.add(s.device.id);
                return true;
            });
            for (Announcement r : announcements) {
                String service = r.sourceIp + "|" + r.instance;
                Service prior = services.get(service);
                if (r.ttl == 0) {
                    if (prior != null) {
                        dirty.add(prior.device.id);
                        services.remove(service);
                    }
                    continue;
                }
                DiscoveredDevice d = r.device.copy();
                d.lastSeenUs = now;
                d.revision = ++revision;
                String id = null;
                String inputMac = macKey(d.mac);
                String priorMac = prior == null ? "" : macKey(prior.device.mac);
                if (prior != null && (inputMac.isEmpty() || priorMac.isEmpty() || inputMac.equals(priorMac))) {
                    DiscoveredDevice previous = prior.device;
                    if (d.mac.isEmpty()) d.mac = previous.mac;
                    if (d.idHash.isEmpty()) d.idHash = previous.idHash;
                    if (!d.hasExplicitPort()) {
                        d.cmdPort = previous.cmdPort;
                        d.portSource = previous.portSource;
                    }
                    if (previous.namePriority > d.namePriority) {
                        d.name = previous.name;
                        d.namePriority = previous.namePriority;
                    }
                    id = previous.id;
                }
                String mac = macKey(d.mac);
                String macAlias = mac.isEmpty() ? "" : "mac:" + mac;
                String hashAlias = hashKey(d.idHash);
                if (aliases.containsKey(macAlias)) id = aliases.get(macAlias);
                if (id == null && aliases.containsKey(hashAlias)) {
                    String candidate = aliases.get(hashAlias);
                    String previousMac = macKey(devices.get(candidate).mac);
                    if (mac.isEmpty() || previousMac.isEmpty() || mac.equals(previousMac)) id = candidate;
                }
                if (id == null) {
                    for (DiscoveredDevice existing : devices.values()) {
                        String existingMac = macKey(existing.mac);
                        boolean conflict = !existingMac.isEmpty() && !mac.isEmpty() && !existingMac.equals(mac);
                        if (!conflict && existing.ip.equals(d.ip) &&
                            (!existing.hasExplicitPort() || !d.hasExplicitPort() || existing.cmdPort == d.cmdPort)) {
                            if (id != null) { id = null; break; }
                            id = existing.id;
                        }
                    }
                }
                if (id == null) id = devices.containsKey(d.id) ? d.id + "_" + d.cmdPort : d.id;
                if (!macAlias.isEmpty()) aliases.put(macAlias, id);
                if (!hashAlias.isEmpty()) aliases.put(hashAlias, id);
                d.id = id;
                if (prior != null) dirty.add(prior.device.id);
                if (!macAlias.isEmpty() || !hashAlias.isEmpty()) {
                    services.values().removeIf(s -> s.device.id.equals(d.id) &&
                            s.announcement.instance.equals(r.instance) && !s.announcement.sourceIp.equals(r.sourceIp));
                }
                services.put(service, new Service(r, d, now + r.ttl * 1_000_000L));
                devices.putIfAbsent(id, d);
                dirty.add(id);
            }

            List<DiscoveredDevice> updates = new ArrayList<>();
            for (String id : dirty) {
                DiscoveredDevice newest = null, name = null, port = null;
                for (Service service : services.values()) {
                    DiscoveredDevice d = service.device;
                    if (!d.id.equals(id)) continue;
                    if (newest == null || d.revision > newest.revision) newest = d;
                    if (name == null || d.namePriority > name.namePriority ||
                            (d.namePriority == name.namePriority && d.revision > name.revision)) name = d;
                    if (d.hasExplicitPort() && (port == null || d.revision > port.revision)) port = d;
                }
                DiscoveredDevice previous = devices.get(id);
                DiscoveredDevice d = (newest == null ? previous : newest).copy();
                d.online = newest != null;
                if (newest != null) {
                    if (d.mac.isEmpty()) d.mac = previous.mac;
                    if (d.idHash.isEmpty()) d.idHash = previous.idHash;
                    d.namePriority = name.namePriority;
                    d.name = name.namePriority > 0 ? name.name : "MiPlay-" + d.ip;
                    if (port != null) {
                        d.cmdPort = port.cmdPort;
                        d.portSource = port.portSource;
                    }
                }
                d.revision = ++revision;
                devices.put(id, d);
                updates.add(d.copy());
            }
            return updates;
        }
    }
}
