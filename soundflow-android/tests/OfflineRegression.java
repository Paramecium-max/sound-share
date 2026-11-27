package io.soundflow.sender;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public final class OfflineRegression {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void await(CountDownLatch latch) {
        try {
            check(latch.await(3, TimeUnit.SECONDS), "latch timed out");
        } catch (InterruptedException e) {
            throw new AssertionError(e);
        }
    }

    private static int extractJsonInt(String json, String key) {
        String pat = "\"" + key + "\":";
        int idx = json.indexOf(pat);
        check(idx >= 0, "missing key " + key + " in " + json);
        int start = idx + pat.length();
        while (start < json.length() && Character.isWhitespace(json.charAt(start))) start++;
        int end = start;
        while (end < json.length() && (Character.isDigit(json.charAt(end)) || json.charAt(end) == '-')) end++;
        return Integer.parseInt(json.substring(start, end));
    }

    private static void testNativeSenderLifecycleAndProtocol() throws Exception {
        try (NativeSender sender = new NativeSender()) {
            check(sender.isOpen(), "NativeSender should be open");

            String stateResp = sender.request("{\"id\":\"1\",\"method\":\"state.get\"}");
            check(stateResp != null && stateResp.contains("\"type\":\"rpc\"") && stateResp.contains("\"multiroomRunning\":false"),
                    "unexpected initial state: " + stateResp);

            String selfAdd = sender.request("{\"id\":\"2\",\"method\":\"device.addManual\",\"name\":\"Loop\",\"ip\":\"127.0.0.1\",\"cmdPort\":8899}");
            check(selfAdd != null && selfAdd.contains("\"error\"") && selfAdd.contains("本机"),
                    "self loopback IP must be rejected: " + selfAdd);

            String validAdd = sender.request("{\"id\":\"3\",\"method\":\"device.addManual\",\"name\":\"LivingRoom\",\"ip\":\"198.51.100.42\",\"cmdPort\":8899}");
            check(validAdd != null && validAdd.contains("\"manual_198.51.100.42_8899\""),
                    "manual device add failed: " + validAdd);

            String extMode = sender.request("{\"id\":\"4\",\"method\":\"source.setMode\",\"mode\":3,\"param\":\"android\"}");
            check(extMode != null && extMode.contains("\"result\":true"), "ExternalPcm mode failed: " + extMode);

            String setMeta = sender.request("{\"id\":\"5\",\"method\":\"media.setMetadata\",\"title\":\"夜曲\",\"artist\":\"周杰伦\",\"album\":\"十一月的萧邦\",\"durationMs\":226000,\"positionMs\":12000,\"lyrics\":\"[00:10.00]一群嗜血的蚂蚁\\n[00:15.00]被腐肉所吸引\\n\"}");
            check(setMeta != null && setMeta.contains("\"result\":true"), "setMetadata failed: " + setMeta);

            String startResp = sender.request("{\"id\":\"6\",\"method\":\"multiroom.start\",\"codec\":\"LPCM\",\"sampleRate\":48000,\"basePlayDelayMs\":350}");
            check(startResp != null && startResp.contains("\"result\":true"), "multiroom.start failed: " + startResp);

            String pollJson = sender.poll();
            check(pollJson != null && pollJson.contains("\"multiroomRunning\":true") && pollJson.contains("一群嗜血的蚂蚁"),
                    "poll snapshot missing active state or synced lyric line: " + pollJson);

            int timerPort = extractJsonInt(pollJson, "timerServicePort");
            check(timerPort > 0 && timerPort <= 65535, "invalid native timerServicePort: " + timerPort);

            try (DatagramSocket socket = new DatagramSocket()) {
                socket.setSoTimeout(2000);
                ByteBuffer frame = ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN);
                frame.putLong(123L).putLong(234L).putLong(0L).putLong(0L).putLong(19L);
                DatagramPacket packet = new DatagramPacket(frame.array(), 40,
                        InetAddress.getByName("127.0.0.1"), timerPort);
                socket.send(packet);
                socket.receive(packet);
                check(packet.getLength() == 40
                        && frame.getLong(0) == 123L
                        && frame.getLong(8) == 234L
                        && frame.getLong(32) == 19L
                        && frame.getLong(16) > 0L
                        && frame.getLong(24) >= frame.getLong(16),
                        "native TimerService response validation failed");
            }

            byte[] pcm = new byte[480 * 4];
            for (int i = 0; i < 12; i++) {
                sender.pushPcm(pcm, 480);
            }

            String stopResp = sender.request("{\"id\":\"7\",\"method\":\"multiroom.stop\"}");
            check(stopResp != null && stopResp.contains("\"result\":true"), "multiroom.stop failed: " + stopResp);
        }
        System.out.println("[PASS] testNativeSenderLifecycleAndProtocol");
    }

    private static void testNativeWavAndLrcLoading() throws Exception {
        Path tmpWav = Files.createTempFile("sf_test_", ".wav");
        Path tmpLrc = Files.createTempFile("sf_test_", ".lrc");
        try {
            int frames = 4800; // 100ms @ 48kHz stereo 16-bit
            int dataBytes = frames * 4;
            ByteBuffer wav = ByteBuffer.allocate(44 + dataBytes).order(ByteOrder.LITTLE_ENDIAN);
            wav.put("RIFF".getBytes(StandardCharsets.US_ASCII));
            wav.putInt(36 + dataBytes);
            wav.put("WAVEfmt ".getBytes(StandardCharsets.US_ASCII));
            wav.putInt(16);
            wav.putShort((short) 1);
            wav.putShort((short) 2);
            wav.putInt(48000);
            wav.putInt(48000 * 4);
            wav.putShort((short) 4);
            wav.putShort((short) 16);
            wav.put("data".getBytes(StandardCharsets.US_ASCII));
            wav.putInt(dataBytes);
            for (int i = 0; i < frames; i++) {
                wav.putShort((short) 1200).putShort((short) -1200);
            }
            Files.write(tmpWav, wav.array());
            Files.writeString(tmpLrc, "\uFEFF[offset:500]\r\n[00:01.50]第一句\n[00:04.00][00:10.00]副歌\n");

            try (NativeSender sender = new NativeSender()) {
                String wavReq = "{\"id\":\"w1\",\"method\":\"source.setMode\",\"mode\":1,\"param\":\""
                        + tmpWav.toAbsolutePath().toString().replace("\\", "\\\\") + "\"}";
                String wavResp = sender.request(wavReq);
                check(wavResp != null && wavResp.contains("\"result\":true"), "WAV load failed: " + wavResp);

                String lrcReq = "{\"id\":\"l1\",\"method\":\"media.loadLrcFile\",\"path\":\""
                        + tmpLrc.toAbsolutePath().toString().replace("\\", "\\\\") + "\"}";
                String lrcResp = sender.request(lrcReq);
                check(lrcResp != null && lrcResp.contains("\"result\":true"), "LRC load failed: " + lrcResp);
            }
        } finally {
            Files.deleteIfExists(tmpWav);
            Files.deleteIfExists(tmpLrc);
        }

        String lrc = "\uFEFF[offset:500]\r\n[00:01.50]第一句\n[00:04.00][00:10.00]副歌\n";
        List<AudioCaptureEngine.LrcLine> lines = AudioCaptureEngine.parseLrcLines(lrc);
        check(lines.size() == 3, "lrc line count mismatch");
        check(AudioCaptureEngine.lrcLineAt(lines, 500L).isEmpty(), "before first line should be empty");
        check("第一句".equals(AudioCaptureEngine.lrcLineAt(lines, 1000L)), "first lrc line mismatch");
        check("副歌".equals(AudioCaptureEngine.lrcLineAt(lines, 3600L)), "chorus lrc line mismatch");
        System.out.println("[PASS] testNativeWavAndLrcLoading");
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

    private static void testMuteLocalAppGenerationInvalidation() {
        AudioCaptureEngine engine = new AudioCaptureEngine(() -> System.nanoTime() / 1000, () -> {});
        FakeRecorder first = new FakeRecorder();
        FakeRecorder second = new FakeRecorder();
        AtomicInteger creations = new AtomicInteger();
        engine.setCaptureSource((rate, size) -> creations.getAndIncrement() == 0 ? first : second);
        engine.start(48000, 480, System.nanoTime() / 1000, new AudioCaptureEngine.Callbacks() {
            @Override public void onPcmFrame(byte[] pcm, int count, long position) {}
            @Override public void onSpectrum(float[] bands) {}
        });
        await(first.reading);
        check(!engine.isMuteLocalApp(), "muteLocalApp should default to false");
        engine.setMuteLocalApp(true);
        await(first.released);
        await(second.reading);
        check(engine.isMuteLocalApp(), "muteLocalApp should be true after toggle");
        check(first.stops.get() == 1 && first.releases.get() == 1, "first recorder was not stopped on mute toggle");
        engine.stop();
        await(second.released);
        System.out.println("[PASS] testMuteLocalAppGenerationInvalidation");
    }

    public static void main(String[] args) throws Exception {
        testNativeSenderLifecycleAndProtocol();
        testNativeWavAndLrcLoading();
        testRecorderGenerations();
        testCapturePacing();
        testMuteLocalAppGenerationInvalidation();
    }
}
