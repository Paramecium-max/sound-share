package io.soundflow.sender;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public final class OfflineRegression {
    static int appendRtpBatch(ByteArrayOutputStream wire, byte[] ts, int seq, long pts) {
        final int chunkMax = 7 * 188;
        int count = 0;
        int off = 0;
        if (ts.length > chunkMax && ts[0] == 0x47 && (ts[1] & 0x1f) == 0 && ts[2] == 0) {
            off = 3 * 188;
            appendRtpFragment(wire, ts, 0, off, seq++, pts);
            count++;
        }
        while (off < ts.length) {
            int length = Math.min(chunkMax, ts.length - off);
            appendRtpFragment(wire, ts, off, length, seq++, pts);
            off += length;
            count++;
        }
        return count;
    }

    private static void appendRtpFragment(ByteArrayOutputStream wire, byte[] ts, int off,
                                          int length, int seq, long pts) {
        int size = 12 + length;
        byte[] header = {
                0x24, 0, (byte) (size >>> 8), (byte) size, (byte) 0x80,
                (byte) (off + length == ts.length ? 0xa1 : 0x21), (byte) (seq >>> 8), (byte) seq,
                (byte) (pts >>> 24), (byte) (pts >>> 16), (byte) (pts >>> 8), (byte) pts,
                (byte) 0xde, (byte) 0xad, (byte) 0xbe, (byte) 0xef,
        };
        wire.write(header, 0, header.length);
        wire.write(ts, off, length);
    }

    static class TsPacker {
        int streamType;
        int ccPat = 0, ccPmt = 0, ccAudio = 0;
        long frameCounter = 0;

        TsPacker(int type) { this.streamType = type; }

        private static int mpegCrc32(byte[] data, int off, int len) {
            int crc = 0xFFFFFFFF;
            for (int i = 0; i < len; i++) {
                crc ^= (data[off + i] & 0xFF) << 24;
                for (int b = 0; b < 8; b++) {
                    crc = ((crc & 0x80000000) != 0) ? ((crc << 1) ^ 0x04C11DB7) : (crc << 1);
                }
            }
            return crc;
        }

        private void appendPat(ByteArrayOutputStream out) {
            byte[] pkt = new byte[188];
            Arrays.fill(pkt, (byte) 0xFF);
            pkt[0] = 0x47; pkt[1] = 0x40; pkt[2] = 0x00;
            pkt[3] = (byte) (0x10 | (ccPat++ & 0x0F));
            pkt[4] = 0x00;
            pkt[5] = 0x00; pkt[6] = (byte) 0xB0; pkt[7] = 0x0D;
            pkt[8] = 0x00; pkt[9] = 0x00; pkt[10] = (byte) 0xC3;
            pkt[11] = 0x00; pkt[12] = 0x00;
            pkt[13] = 0x00; pkt[14] = 0x01;
            pkt[15] = (byte) 0xE1; pkt[16] = 0x00;
            int crc = mpegCrc32(pkt, 5, 12);
            pkt[17] = (byte) (crc >>> 24); pkt[18] = (byte) (crc >>> 16);
            pkt[19] = (byte) (crc >>> 8);  pkt[20] = (byte) crc;
            out.write(pkt, 0, 188);
        }

        private void appendPmt(ByteArrayOutputStream out) {
            byte[] pkt = new byte[188];
            Arrays.fill(pkt, (byte) 0xFF);
            pkt[0] = 0x47; pkt[1] = 0x41; pkt[2] = 0x00;
            pkt[3] = (byte) (0x10 | (ccPmt++ & 0x0F));
            pkt[4] = 0x00;
            pkt[5] = 0x02; pkt[6] = (byte) 0xB0; pkt[7] = 0x12;
            pkt[8] = 0x00; pkt[9] = 0x01; pkt[10] = (byte) 0xC3;
            pkt[11] = 0x00; pkt[12] = 0x00;
            pkt[13] = (byte) 0xF0; pkt[14] = 0x00;
            pkt[15] = (byte) 0xF0; pkt[16] = 0x00;
            pkt[17] = (byte) streamType;
            pkt[18] = (byte) 0xF1; pkt[19] = 0x00;
            pkt[20] = (byte) 0xF0; pkt[21] = 0x00;
            int crc = mpegCrc32(pkt, 5, 17);
            pkt[22] = (byte) (crc >>> 24); pkt[23] = (byte) (crc >>> 16);
            pkt[24] = (byte) (crc >>> 8);  pkt[25] = (byte) crc;
            out.write(pkt, 0, 188);
        }

        private void appendPcr(ByteArrayOutputStream out, long pts90k) {
            byte[] pkt = new byte[188];
            Arrays.fill(pkt, (byte) 0xFF);
            pkt[0] = 0x47; pkt[1] = 0x10; pkt[2] = 0x00; pkt[3] = 0x20;
            pkt[4] = (byte) 183; pkt[5] = 0x10;
            long base = pts90k & ((1L << 33) - 1);
            pkt[6] = (byte) ((base >>> 25) & 0xFF);
            pkt[7] = (byte) ((base >>> 17) & 0xFF);
            pkt[8] = (byte) ((base >>> 9) & 0xFF);
            pkt[9] = (byte) ((base >>> 1) & 0xFF);
            pkt[10] = (byte) (((base & 1L) << 7) | 0x7E);
            pkt[11] = 0x00;
            out.write(pkt, 0, 188);
        }

        byte[] pack(byte[] payload, long pts90k, boolean forcePsi) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(188 * 8);
            if (forcePsi || frameCounter % 4 == 0) {
                appendPat(out);
                appendPmt(out);
                appendPcr(out, pts90k);
            }
            frameCounter++;

            int pesBody = 3 + 7 + payload.length;
            int pesLenField = Math.min(pesBody, 0xFFFF);
            byte[] pes = new byte[16 + payload.length];
            pes[0] = 0; pes[1] = 0; pes[2] = 1;
            pes[3] = (byte) (streamType == TsAndRtpSender.STREAM_WFD_LPCM ? 0xBD : 0xC0);
            pes[4] = (byte) ((pesLenField >>> 8) & 0xFF);
            pes[5] = (byte) (pesLenField & 0xFF);
            pes[6] = (byte) 0x84;
            pes[7] = (byte) 0x80;
            pes[8] = 0x07;
            long pts = pts90k & ((1L << 33) - 1);
            pes[9]  = (byte) (0x21 | (((pts >>> 30) & 0x07) << 1));
            pes[10] = (byte) ((pts >>> 22) & 0xFF);
            pes[11] = (byte) (0x01 | (((pts >>> 15) & 0x7F) << 1));
            pes[12] = (byte) ((pts >>> 7) & 0xFF);
            pes[13] = (byte) (0x01 | ((pts & 0x7F) << 1));
            pes[14] = (byte) 0xFF;
            pes[15] = (byte) 0xFF;
            System.arraycopy(payload, 0, pes, 16, payload.length);

            int off = 0;
            boolean first = true;
            while (off < pes.length) {
                byte[] pkt = new byte[188];
                pkt[0] = 0x47;
                pkt[1] = (byte) ((first ? 0x40 : 0x00) | 0x11);
                pkt[2] = 0x00;
                int rem = pes.length - off;
                if (rem >= 184) {
                    pkt[3] = (byte) (0x10 | (ccAudio++ & 0x0F));
                    System.arraycopy(pes, off, pkt, 4, 184);
                    off += 184;
                } else {
                    int pad = 184 - rem;
                    pkt[3] = (byte) (0x30 | (ccAudio++ & 0x0F));
                    if (pad == 1) {
                        pkt[4] = 0;
                        System.arraycopy(pes, off, pkt, 5, rem);
                    } else {
                        pkt[4] = (byte) (pad - 1);
                        pkt[5] = 0;
                        Arrays.fill(pkt, 6, 4 + pad, (byte) 0xFF);
                        System.arraycopy(pes, off, pkt, 4 + pad, rem);
                    }
                    off += rem;
                }
                out.write(pkt, 0, 188);
                first = false;
            }
            return out.toByteArray();
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void await(CountDownLatch latch) {
        try { check(latch.await(3, TimeUnit.SECONDS), "latch timed out"); }
        catch (InterruptedException e) { throw new AssertionError(e); }
    }

    private static void emitTransportFixtures(Path fixtures) throws IOException {
        for (String line : Files.readAllLines(fixtures.resolve("transport_cases.tsv"))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] f = line.split(" ");
            byte[] payload = new byte[Integer.parseInt(f[1])];
            for (int i = 0; i < payload.length; i++) payload[i] = (byte) (i * 37 + 11);
            int previous = Integer.parseInt(f[2]);
            long pts = Long.parseLong(f[4]);
            int stream = f[0].equals("AAC") ? TsAndRtpSender.STREAM_ADTS_AAC : TsAndRtpSender.STREAM_WFD_LPCM;
            TsPacker packer = new TsPacker(stream);
            for (int i = 0; i < previous; i++) packer.pack(payload, 1920L * i, false);
            byte[] ts = packer.pack(payload, pts, previous == 2);
            ByteArrayOutputStream wire = new ByteArrayOutputStream();
            appendRtpBatch(wire, ts, Integer.parseInt(f[3]), pts);
            StringBuilder hex = new StringBuilder();
            for (byte b : wire.toByteArray()) {
                hex.append(Character.forDigit((b & 0xff) >>> 4, 16));
                hex.append(Character.forDigit(b & 0x0f, 16));
            }
            System.out.println(hex);
        }
    }

    private static void testNativeTransport(Path fixtures) throws Exception {
        for (String line : Files.readAllLines(fixtures.resolve("transport_cases.tsv"))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] fields = line.split(" ");
            byte[] payload = new byte[Integer.parseInt(fields[1])];
            for (int i = 0; i < payload.length; i++) payload[i] = (byte) (i * 37 + 11);
            int stream = fields[0].equals("AAC") ? TsAndRtpSender.STREAM_ADTS_AAC : TsAndRtpSender.STREAM_WFD_LPCM;
            int previous = Integer.parseInt(fields[2]);
            int sequence = Integer.parseInt(fields[3]);
            long pts = Long.parseLong(fields[4]);
            TsPacker reference = new TsPacker(stream);
            try (NativePacketizer nativePacker = new NativePacketizer(stream)) {
                for (int i = 0; i < previous; i++) {
                    reference.pack(payload, 1920L * i, false);
                    nativePacker.pack(payload, 1920L * i, i, false);
                }
                ByteArrayOutputStream expected = new ByteArrayOutputStream();
                int count = appendRtpBatch(expected, reference.pack(payload, pts, previous == 2), sequence, pts);
                byte[] actual = nativePacker.pack(payload, pts, sequence, previous == 2);
                check(Arrays.equals(expected.toByteArray(), actual), "Native frame differs: " + line);
                check(NativePacketizer.packetCount(actual) == count, "RTP fragment count differs: " + line);
            }
        }
        TimerService timer = new TimerService();
        check(timer.start(), "native UDP clock startup");
        try (java.net.DatagramSocket socket = new java.net.DatagramSocket()) {
            socket.setSoTimeout(2000);
            ByteBuffer frame = ByteBuffer.allocate(40).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            frame.putLong(123).putLong(234).putLong(0).putLong(0).putLong(19);
            var packet = new java.net.DatagramPacket(frame.array(), 40,
                    java.net.InetAddress.getByName("127.0.0.1"), timer.getPort());
            socket.send(packet);
            socket.receive(packet);
            check(packet.getLength() == 40 && frame.getLong(0) == 123 && frame.getLong(8) == 234 &&
                frame.getLong(32) == 19 && frame.getLong(16) > 0 && frame.getLong(24) >= frame.getLong(16),
                "native timer must preserve request fields and stamp both server times");
        } finally {
            timer.stop();
        }
        check(timer.getPort() == 0, "timer port must reset on stop");
        System.out.println("[PASS] testNativeTransport");
    }

    private static void testDns(Path fixtures) throws IOException {
        for (String line : Files.readAllLines(fixtures.resolve("mdns_cases.tsv"))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] f = line.split("\t");
            byte[] packet = new byte[f[2].length() / 2];
            for (int i = 0; i < packet.length; i++) packet[i] = (byte) Integer.parseInt(f[2].substring(i * 2, i * 2 + 2), 16);
            Set<String> actual = new HashSet<>();
            for (MdnsDiscovery.Announcement record : MdnsDiscovery.parseAnnouncements(packet, packet.length, f[1])) {
                MdnsDiscovery.DiscoveredDevice d = record.device;
                if (record.ttl == 0) actual.add("Bye");
                else actual.add("Live:" + d.name + ":" + d.ip + ":" + d.cmdPort + ":" +
                                d.namePriority + ":" + (d.hasExplicitPort() ? 1 : 0));
            }
            Set<String> expected = f[3].equals("-") ? Set.of() : Set.of(f[3].split(","));
            check(actual.equals(expected), f[0] + " parsed " + actual + ", expected " + expected);
        }
        check(MdnsDiscovery.isSelfLocalIp("127.0.0.1"), "loopback must be recognized as self IP");
        check(!MdnsDiscovery.isSelfLocalIp("203.0.113.250"), "external test IP must not match self IP");

        MdnsDiscovery.DeviceCache cache = new MdnsDiscovery.DeviceCache();
        MdnsDiscovery.DiscoveredDevice device = new MdnsDiscovery.DiscoveredDevice();
        device.id = device.ip = "192.168.10.5";
        device.name = "Room";
        device.namePriority = 2;
        device.cmdPort = 9988;
        device.portSource = MdnsDiscovery.PortSource.TXT;
        MdnsDiscovery.DiscoveredDevice first = cache.update(List.of(
                new MdnsDiscovery.Announcement("first", device.ip, 2, device)), 100).get(0);
        device.mac = device.id = "aa:bb:cc:dd:ee:ff";
        device.cmdPort = 8899;
        device.portSource = MdnsDiscovery.PortSource.DEFAULT;
        MdnsDiscovery.DiscoveredDevice second = cache.update(List.of(
                new MdnsDiscovery.Announcement("first", device.ip, 3, device)), 700_000).get(0);
        check(first.id.equals(second.id) && second.cmdPort == 9988 && !second.mac.isEmpty(), "identity enrichment");
        check(first.mac.isEmpty() && second.revision > first.revision, "snapshot mutated");
        check(cache.update(List.of(), 2_000_100).isEmpty(), "TTL renewal failed");
        device.ip = "192.168.10.6";
        device.portSource = MdnsDiscovery.PortSource.TXT;
        List<MdnsDiscovery.DiscoveredDevice> moved = cache.update(List.of(
                new MdnsDiscovery.Announcement("first", device.ip, 2, device),
                new MdnsDiscovery.Announcement("second", device.ip, 1, device)), 2_100_000);
        check(moved.size() == 1 && moved.get(0).id.equals(first.id) && moved.get(0).cmdPort == 8899 &&
              moved.get(0).ip.equals(device.ip), "device migration");
        check(cache.update(List.of(new MdnsDiscovery.Announcement("second", device.ip, 0, null)),
                           2_100_100).get(0).online, "other service still online");
        check(!cache.update(List.of(), 4_100_000).get(0).online, "device ignored expiry");
        check(cache.update(List.of(new MdnsDiscovery.Announcement("second", device.ip, 2, device)),
                           4_100_001).get(0).id.equals(first.id), "device lost identity on reappearance");
        System.out.println("[PASS] testDns");
    }

    private static final class FakeSocket extends Socket {
        final ByteArrayOutputStream wire = new ByteArrayOutputStream();
        final CountDownLatch started = new CountDownLatch(1);
        final CountDownLatch resume = new CountDownLatch(1);
        final AtomicBoolean closed = new AtomicBoolean();

        @Override public boolean isClosed() { return closed.get(); }
        @Override public void close() { closed.set(true); resume.countDown(); }
        @Override public OutputStream getOutputStream() {
            return new OutputStream() {
                @Override public void write(int value) { throw new AssertionError(); }

                @Override public void write(byte[] bytes, int offset, int len) throws IOException {
                    wire.write(bytes[offset]);
                    if (bytes[offset] == '$') {
                        started.countDown();
                        await(resume);
                        if (closed.get()) throw new IOException("closed at deadline");
                    }
                    wire.write(bytes, offset + 1, len - 1);
                }
            };
        }
    }

    private static CompletableFuture<Boolean> write(ConnectionWriter writer, byte[] bytes, int deadline) {
        return CompletableFuture.supplyAsync(() -> {
            try { writer.write(bytes, deadline); return true; }
            catch (Exception e) { return false; }
        });
    }

    private static void testDiscoveryBurstDeadlines() {
        MdnsDiscovery.ProbeBurst burst = new MdnsDiscovery.ProbeBurst();
        check(burst.begin(0) && burst.takeDue(0) == 0, "missing initial query");
        check(!burst.begin(5_000_000) && burst.takeDue(219_999_999) == -1, "refresh advanced second probe");
        check(burst.takeDue(220_000_000) == 1, "second probe is not at 220 ms");
        check(!burst.begin(300_000_000) && burst.takeDue(649_999_999) == -1, "refresh advanced third probe");
        check(burst.takeDue(650_000_000) == 2, "third probe is not at 650 ms");
        check(!burst.active() && burst.takeDue(900_000_000) == -1, "refresh queued an extra burst");
        System.out.println("[PASS] testDiscoveryBurstDeadlines");
    }

    private static void testConnectionWriter() throws Exception {
        byte[] media = {'$', 0, 0, 1, 0x32};
        byte[] control = "RTSP/1.0 200 OK\r\n\r\n".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        FakeSocket socket = new FakeSocket();
        ConnectionWriter writer = new ConnectionWriter(socket);
        CompletableFuture<Boolean> frame = write(writer, media, 1500);
        await(socket.started);
        CompletableFuture<Boolean> response = write(writer, control, 1500);
        socket.resume.countDown();
        check(frame.get(2, TimeUnit.SECONDS) && response.get(2, TimeUnit.SECONDS), "shared write failed");
        ByteArrayOutputStream expected = new ByteArrayOutputStream();
        expected.write(media);
        expected.write(control);
        check(Arrays.equals(socket.wire.toByteArray(), expected.toByteArray()), "interleaved RTSP response");

        FakeSocket stalled = new FakeSocket();
        ConnectionWriter timed = new ConnectionWriter(stalled);
        check(!write(timed, media, 60).get(2, TimeUnit.SECONDS), "blocked write missed deadline");
        check(stalled.closed.get() && stalled.wire.size() == 1, "partial send didn't close socket");
        check(!write(timed, control, 60).get(2, TimeUnit.SECONDS) && stalled.wire.size() == 1, "wrote after failure");
        System.out.println("[PASS] testConnectionWriter");
    }

    private static final class FakeEncoder implements TsAndRtpSender.Encoder {
        final int fail;
        final List<Long> accepted = new ArrayList<>();
        final ArrayDeque<TsAndRtpSender.EncodedAudio> queued = new ArrayDeque<>();
        final ByteBuffer buffer = ByteBuffer.allocate(4096);
        boolean slot;
        boolean output = true;
        int closes;

        FakeEncoder(int fail) { this.fail = fail; }
        @Override public int acquireInput() {
            check(!slot && closes == 0, "lost input slot");
            slot = true;
            return 0;
        }
        @Override public ByteBuffer inputBuffer(int index) {
            if (fail == 1) return null;
            if (fail == 2) return ByteBuffer.allocate(12);
            return buffer;
        }
        @Override public void submitInput(int index, int len, long ptsUs) {
            if (fail == 3) throw new IllegalStateException("queue failed");
            check(slot && len == 4096, "input was not acquired");
            slot = false;
            accepted.add(ptsUs);
            queued.add(new TsAndRtpSender.EncodedAudio(new byte[]{buffer.get(0), 7}, ptsUs == 0 ? -1 : ptsUs));
        }
        @Override public TsAndRtpSender.EncodedAudio takeOutput() { return output ? queued.poll() : null; }
        @Override public void close() { closes++; slot = false; }
    }

    private static final class AccessUnits {
        final InputStream in;
        int expectedSeq;
        int expectedAudioCc;

        AccessUnits(InputStream in) { this.in = in; }

        long readTimestamp() throws IOException {
            for (;;) {
                byte[] header = in.readNBytes(4);
                check(header.length == 4 && header[0] == '$', "missing RTP frame");
                int length = ((header[2] & 0xff) << 8) | (header[3] & 0xff);
                byte[] rtp = in.readNBytes(length);
                check(rtp.length == length && length >= 12, "incomplete RTP frame");
                int seq = ((rtp[2] & 0xff) << 8) | (rtp[3] & 0xff);
                check(seq == (expectedSeq++ & 0xffff), "RTP sequence skipped after a delay change");
                for (int offset = 12; offset + 188 <= length; offset += 188) {
                    int pid = ((rtp[offset + 1] & 31) << 8) | (rtp[offset + 2] & 0xff);
                    if (pid == 0x1100) {
                        check((rtp[offset + 3] & 15) == (expectedAudioCc++ & 15),
                              "audio continuity skipped after a delay change");
                    }
                }
                if ((rtp[1] & 0x80) != 0) return Integer.toUnsignedLong(ByteBuffer.wrap(rtp, 4, 4).getInt());
            }
        }
    }

    private static void testCodecRecoveryAndDelay() throws Exception {
        for (int error = 1; error <= 3; error++) {
            FakeEncoder bad = new FakeEncoder(error);
            FakeEncoder good = new FakeEncoder(0);
            AtomicInteger generations = new AtomicInteger();
            AtomicInteger failures = new AtomicInteger();
            TsAndRtpSender sender = new TsAndRtpSender(rate -> generations.getAndIncrement() == 0 ? bad : good,
                                                       ignored -> {});
            try (ServerSocket server = new ServerSocket(0);
                 Socket peer = new Socket("127.0.0.1", server.getLocalPort());
                 Socket sock = server.accept();
                 Socket biasedPeer = new Socket("127.0.0.1", server.getLocalPort());
                 Socket biasedSocket = server.accept()) {
                peer.setSoTimeout(2000);
                biasedPeer.setSoTimeout(2000);
                check(!sender.configure(false, 48000, failures::incrementAndGet).useLpcm, "forced LPCM unexpectedly");
                sender.addTarget("room", 1, "127.0.0.1", new ConnectionWriter(sock), 0, failures::incrementAndGet);
                sender.addTarget("biased", 2, "127.0.0.1", new ConnectionWriter(biasedSocket), -50,
                                 failures::incrementAndGet);
                byte[] pcm = new byte[4096];
                sender.broadcastPcmFrame(pcm, 0, () -> 350);
                check(bad.closes == 1 && !bad.slot && bad.accepted.isEmpty(), "failed slot retained");
                good.output = false;
                sender.broadcastPcmFrame(pcm, 1920, () -> 350);
                check(good.accepted.equals(List.of(0L, 21333L)), "failed PCM was not retried exactly once");
                good.output = true;
                sender.broadcastPcmFrame(pcm, 3840, () -> 350);
                AccessUnits first = new AccessUnits(peer.getInputStream());
                AccessUnits biased = new AccessUnits(biasedPeer.getInputStream());
                for (long pts : new long[]{31500, 33420, 35340}) {
                    check(first.readTimestamp() == pts, "delayed output lost PCM time");
                    check(biased.readTimestamp() == pts - 4500, "device delay bias was lost");
                }
                for (int i = 3; i <= 9; i++) sender.broadcastPcmFrame(pcm, i * 1920L, () -> 250);
                for (int i = 8; i <= 9; i++) {
                    check(first.readTimestamp() == i * 1920 + 22500, "delay decrease was clamped");
                    check(biased.readTimestamp() == i * 1920 + 22500 - 4500, "biased target failed to recover");
                }
                sender.broadcastPcmFrame(pcm, 10 * 1920L, () -> 300);
                check(first.readTimestamp() == 10 * 1920 + 27000, "delay increase was ignored");
                check(biased.readTimestamp() == 10 * 1920 + 27000 - 4500, "delay increase lost device bias");
                check(failures.get() == 0, "recoverable codec failure ended session");
                sender.clearAll();
                check(good.closes == 1, "codec not released");
            }
        }

        AtomicInteger permanent = new AtomicInteger();
        List<FakeEncoder> broken = new ArrayList<>();
        TsAndRtpSender sender = new TsAndRtpSender(rate -> {
            FakeEncoder encoder = new FakeEncoder(3);
            broken.add(encoder);
            return encoder;
        }, ignored -> {});
        try (ServerSocket server = new ServerSocket(0);
             Socket peer = new Socket("127.0.0.1", server.getLocalPort());
             Socket sock = server.accept()) {
            sender.configure(false, 48000, permanent::incrementAndGet);
            sender.addTarget("bad", 1, "127.0.0.1", new ConnectionWriter(sock), 0, () -> {});
            for (int i = 0; i < 5; i++) sender.broadcastPcmFrame(new byte[4096], i * 1920L, () -> 350);
            check(permanent.get() == 1 && broken.size() == 3, "permanent AAC failure not reported once");
            for (FakeEncoder encoder : broken) check(encoder.closes == 1 && !encoder.slot, "leaked codec input slot");
            sender.clearAll();
        }
        System.out.println("[PASS] testCodecRecoveryAndDelay");
    }

    private static final class FakeRecorder implements AudioCaptureEngine.Recorder {
        final CountDownLatch reading = new CountDownLatch(1);
        final CountDownLatch resume = new CountDownLatch(1);
        final CountDownLatch released = new CountDownLatch(1);
        final AtomicInteger reads = new AtomicInteger();
        final AtomicInteger releases = new AtomicInteger();
        final AtomicInteger stops = new AtomicInteger();
        boolean failStart;

        @Override public void start() {
            if (failStart) throw new IllegalStateException("startRecording failed");
        }
        @Override public int read(byte[] pcm, int offset, int length) {
            reads.incrementAndGet();
            reading.countDown();
            try {
                check(resume.await(3, TimeUnit.SECONDS), "fake recorder read timed out");
            } catch (InterruptedException e) {
                check(stops.get() == 1, "read was interrupted before recorder stop");
            }
            Arrays.fill(pcm, offset, offset + length, (byte) 0x77);
            return length;
        }
        @Override public void stop() { stops.incrementAndGet(); resume.countDown(); }
        @Override public void release() { releases.incrementAndGet(); released.countDown(); }
    }

    private static void testRecorderGenerations() {
        AudioCaptureEngine engine = new AudioCaptureEngine(() -> System.nanoTime() / 1000, () -> {});
        FakeRecorder stale = new FakeRecorder();
        FakeRecorder current = new FakeRecorder();
        CountDownLatch constructing = new CountDownLatch(1);
        CountDownLatch constructed = new CountDownLatch(1);
        AtomicInteger staleFrames = new AtomicInteger();
        engine.setCaptureSource((rate, size) -> {
            constructing.countDown();
            await(constructed);
            return stale;
        });
        engine.start(48000, 480, System.nanoTime() / 1000, new AudioCaptureEngine.Callbacks() {
            @Override public void onPcmFrame(byte[] pcm, int frames, long position) {
                if (pcm[0] == 0x77 && pcm[1] == 0x77) staleFrames.incrementAndGet();
            }
            @Override public void onSpectrum(float[] bands) {}
        });
        await(constructing);
        engine.revokeMediaProjection("revoked");
        engine.setCaptureSource((rate, size) -> current);
        constructed.countDown();
        await(stale.released);
        await(current.reading);
        check(stale.releases.get() == 1 && stale.reads.get() == 0, "stale recorder was published");
        engine.setMode(AudioCaptureEngine.MODE_TEST_TONE, "");
        await(current.released);
        check(staleFrames.get() == 0 && current.releases.get() == 1 && current.stops.get() == 1,
              "invalid generation delivered PCM or released twice");

        FakeRecorder failed = new FakeRecorder();
        failed.failStart = true;
        FakeRecorder recovered = new FakeRecorder();
        AtomicInteger creations = new AtomicInteger();
        engine.setCaptureSource((rate, size) -> creations.getAndIncrement() == 0 ? failed : recovered);
        await(failed.released);
        await(recovered.reading);
        engine.stop();
        check(failed.releases.get() == 1 && failed.reads.get() == 0, "failed recorder leaked");
        check(recovered.releases.get() == 1 && recovered.stops.get() == 1, "stop did not release recorder once");
        System.out.println("[PASS] testRecorderGenerations");
    }

    private static void testCapturePacing() {
        AtomicLong clock = new AtomicLong();
        AudioCaptureEngine engine = new AudioCaptureEngine(clock::get, () -> {});
        CountDownLatch frames = new CountDownLatch(3);
        List<Long> samples = new ArrayList<>();
        engine.start(48000, 480, 0, new AudioCaptureEngine.Callbacks() {
            @Override public void onPcmFrame(byte[] pcm, int count, long position) {
                samples.add(position);
                if (samples.size() == 1) clock.set(60_000);
                if (samples.size() == 2) clock.set(70_000);
                frames.countDown();
            }
            @Override public void onSpectrum(float[] bands) {}
        });
        await(frames);
        engine.stop();
        check(samples.subList(0, 3).equals(List.of(0L, 2880L, 3360L)), "capture replayed obsolete ticks");
        System.out.println("[PASS] testCapturePacing");
    }

    private static void testRemoteControlAndLrc() {
        byte[] pause = CmdSession.encodeNotifyControlKey("key-pause");
        byte[] next = CmdSession.encodeNotifyControlKey("key-next");
        byte[] prev = CmdSession.encodeNotifyControlKey("key-prev");
        byte[] seek = CmdSession.encodeNotifySeekMs(65432L);
        byte[] vol = new byte[]{6, 'v', 'o', 'l', 'u', 'm', 'e', 0x03, 58};
        byte[] combined = new byte[pause.length + next.length + prev.length + seek.length + vol.length];
        int off = 0;
        for (byte[] part : new byte[][]{pause, next, prev, seek, vol}) {
            System.arraycopy(part, 0, combined, off, part.length);
            off += part.length;
        }
        List<CmdSession.RemoteControlEvent> evs = CmdSession.decodeRemoteControlEvents(CmdSession.CMD_NOTIFY, combined);
        check(evs.size() == 5, "expected 5 notify events");
        check(evs.get(0).action == CmdSession.RemoteControlAction.PAUSE, "pause mismatch");
        check(evs.get(1).action == CmdSession.RemoteControlAction.NEXT, "next mismatch");
        check(evs.get(2).action == CmdSession.RemoteControlAction.PREVIOUS, "prev mismatch");
        check(evs.get(3).action == CmdSession.RemoteControlAction.SEEK && evs.get(3).positionMs == 65432L, "seek mismatch");
        check(evs.get(4).action == CmdSession.RemoteControlAction.SET_VOLUME && evs.get(4).volume == 58, "volume mismatch");

        String lrc = "\uFEFF[offset:500]\r\n[00:01.50]第一句\n[00:04.00][00:10.00]副歌\n";
        List<AudioCaptureEngine.LrcLine> lines = AudioCaptureEngine.parseLrcLines(lrc);
        check(lines.size() == 3, "lrc line count mismatch");
        check(AudioCaptureEngine.lrcLineAt(lines, 500L).isEmpty(), "before first line should be empty");
        check("第一句".equals(AudioCaptureEngine.lrcLineAt(lines, 1000L)), "first lrc line mismatch");
        check("副歌".equals(AudioCaptureEngine.lrcLineAt(lines, 3600L)), "chorus lrc line mismatch");
        System.out.println("[PASS] testRemoteControlAndLrc");
    }

    public static void main(String[] args) throws Exception {
        Path fixtures = Path.of(args[0]);
        if (args.length == 2 && args[1].equals("--dump-transport-fixtures")) {
            emitTransportFixtures(fixtures);
            return;
        }
        testNativeTransport(fixtures);
        testDns(fixtures);
        testDiscoveryBurstDeadlines();
        testConnectionWriter();
        testCodecRecoveryAndDelay();
        testRecorderGenerations();
        testCapturePacing();
        testRemoteControlAndLrc();
    }
}
