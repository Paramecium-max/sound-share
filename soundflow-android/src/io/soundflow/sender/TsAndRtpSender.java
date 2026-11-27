package io.soundflow.sender;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.util.Log;
import java.io.IOException;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.function.Consumer;
import java.util.function.IntSupplier;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 1. Android MediaCodec 硬件 ADTS AAC-LC (48kHz Stereo, 0xFF 0xF9 0x4C) & WFD LPCM 编码
 *    - SS-07: 返回实际生效的编码配置（AAC 初始化失败回退 LPCM 后供 RTSP/采集帧长共享）
 *    - SS-08: 按每个编码输出帧的 BufferInfo.presentationTimeUs 独立换算 90kHz PTS
 * 2. 标准 MPEG-TS 打包器 (PAT 0x0000 / PMT 0x0100 / PCR 0x1000 / Audio PES 0x1100)
 * 3. 一源多发 RTP-over-TCP 分发引擎 (SS-03 按全局 sessionId 防旧覆盖新; SS-04 每设备有界队列 + 独立写入线程隔离慢接收端)
 */
public class TsAndRtpSender {
    public static final int STREAM_ADTS_AAC = 0x0F;
    public static final int STREAM_WFD_LPCM = 0x83;
    public static final int MAX_QUEUED_FRAMES = 24;
    private static final int MAX_PENDING_PCM_FRAMES = 6;

    private static final class AudioFrame {
        final byte[] data;
        final long pts;

        AudioFrame(byte[] data, long pts) {
            this.data = data;
            this.pts = pts;
        }
    }

    public static class AudioConfig {
        public final boolean useLpcm;
        public final int sampleRate;
        public final int frameSamples;

        public AudioConfig(boolean useLpcm, int sampleRate, int frameSamples) {
            this.useLpcm = useLpcm;
            this.sampleRate = sampleRate;
            this.frameSamples = frameSamples;
        }
    }

    public static class TargetNode {
        public final String deviceId;
        public final long sessionId;
        public final String peerIp;
        public final Socket sock;
        private final ConnectionWriter out;
        private final Runnable onFailure;
        public int rtpSeq = 0;
        public volatile int delayBiasMs = 0;
        public volatile long packetsSent = 0;
        public volatile long droppedFrames = 0;
        public volatile boolean healthy = true;
        private final AtomicBoolean forcePsi = new AtomicBoolean(true);
        public final AtomicBoolean stopRequested = new AtomicBoolean(false);
        private final ArrayBlockingQueue<AudioFrame> queue = new ArrayBlockingQueue<>(MAX_QUEUED_FRAMES);
        private final int streamType;
        private final long frameDuration90k;
        private long lastPts = -1;
        public final Thread writerThread;

        private TargetNode(String id, long sessionId, String ip, ConnectionWriter writer, int bias, int streamType, Runnable onFailure) {
            this.deviceId = id;
            this.sessionId = sessionId;
            this.peerIp = ip;
            Socket s = writer.socket;
            this.sock = s;
            this.onFailure = onFailure;
            try { s.setTcpNoDelay(true); } catch (Exception ignored) {}
            try { s.setSendBufferSize(Math.max(s.getSendBufferSize(), 65536)); } catch (Exception ignored) {}
            this.out = writer;
            this.delayBiasMs = bias;
            this.streamType = streamType;
            this.frameDuration90k = streamType == STREAM_WFD_LPCM ? 900 : 1920;
            this.writerThread = new Thread(this::writeLoop, "MiPlay-RtpOut-" + id + "-" + sessionId);
            this.writerThread.setDaemon(true);
            this.writerThread.setPriority(Thread.NORM_PRIORITY + 2);
            this.writerThread.start();
        }

        private void writeLoop() {
            try (NativePacketizer packer = new NativePacketizer(streamType)) {
                while (!stopRequested.get() && healthy) {
                    AudioFrame frame = queue.poll(200, TimeUnit.MILLISECONDS);
                    if (frame != null && !stopRequested.get()) writeFrame(packer, frame);
                }
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            } catch (Exception | LinkageError e) {
                healthy = false;
                if (!stopRequested.get()) {
                    out.abort();
                    onFailure.run();
                }
            }
        }

        private void writeFrame(NativePacketizer packer, AudioFrame frame) throws Exception {
            long pts = Math.max(0, frame.pts + delayBiasMs * 90L);
            // Discard overlapping AUs before allocating continuity counters.
            if (lastPts >= 0 && pts < lastPts + frameDuration90k) {
                droppedFrames++;
                forcePsi.set(true);
                return;
            }
            byte[] wire = packer.pack(frame.data, pts, rtpSeq, forcePsi.getAndSet(false));
            int count = NativePacketizer.packetCount(wire);
            if (!stopRequested.get() && count > 0) {
                out.write(wire, 120);
                rtpSeq = (rtpSeq + count) & 0xffff;
                lastPts = pts;
                packetsSent += count;
            }
        }

        private void enqueueFrame(AudioFrame frame) {
            if (!healthy || stopRequested.get()) return;
            while (!queue.offer(frame)) {
                if (queue.poll() != null) {
                    droppedFrames++;
                    forcePsi.set(true);
                }
            }
        }

        public void stopAndClose() {
            requestWriterStop();
            closeSocket();
            awaitWriter(400);
        }

        private void stopForTeardown() {
            requestWriterStop();
            awaitWriter(100);
            // If a blocked writer cannot leave in time, close its socket to wake it.
            if (writerThread.isAlive()) closeSocket();
        }

        private void requestWriterStop() {
            stopRequested.set(true);
            healthy = false;
            queue.clear();
            writerThread.interrupt();
        }

        private void closeSocket() {
            try { sock.close(); } catch (Exception ignored) {}
        }

        private void awaitWriter(long timeoutMs) {
            try {
                writerThread.join(timeoutMs);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private final Object lock = new Object();
    private final ConcurrentHashMap<String, TargetNode> targets = new ConcurrentHashMap<>();
    private final ArrayDeque<AudioFrame> pendingPcm = new ArrayDeque<>();
    private Encoder aacEncoder;
    private final EncoderFactory encoderFactory;
    private final Consumer<RuntimeException> reportCodecError;
    private volatile boolean useLpcm = false;
    private volatile int sampleRate = 48000;
    private final ArrayDeque<Long> acceptedPcmPts = new ArrayDeque<>();
    private int codecFailures;
    private Runnable onEncoderFailed;
    private boolean reportedInputOverflow;

    static final class EncodedAudio {
        final byte[] data;
        final long ptsUs;
        EncodedAudio(byte[] data, long ptsUs) {
            this.data = data;
            this.ptsUs = ptsUs;
        }
    }

    interface Encoder {
        int acquireInput();
        ByteBuffer inputBuffer(int index);
        void submitInput(int index, int size, long ptsUs);
        EncodedAudio takeOutput();
        void close();
    }

    interface EncoderFactory {
        Encoder create(int rate) throws Exception;
    }

    public TsAndRtpSender() {
        this(NativeEncoder::new, error -> Log.e("MiPlayRTP", "AAC encoding failed", error));
    }

    TsAndRtpSender(EncoderFactory encoderFactory, Consumer<RuntimeException> reportCodecError) {
        this.encoderFactory = encoderFactory;
        this.reportCodecError = reportCodecError;
    }

    private static final class NativeEncoder implements Encoder {
        private final MediaCodec codec;
        private final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

        NativeEncoder(int rate) throws IOException {
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
            try {
                MediaFormat fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, rate, 2);
                fmt.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, 256000);
                fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384);
                codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                codec.start();
            } catch (RuntimeException error) {
                close();
                throw error;
            }
        }

        @Override public int acquireInput() { return codec.dequeueInputBuffer(0); }

        @Override public ByteBuffer inputBuffer(int index) { return codec.getInputBuffer(index); }

        @Override public void submitInput(int index, int size, long ptsUs) {
            codec.queueInputBuffer(index, 0, size, ptsUs, 0);
        }

        @Override public EncodedAudio takeOutput() {
            for (int attempts = 0; attempts < 16; attempts++) {
                int index = codec.dequeueOutputBuffer(info, 0);
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ||
                    index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) continue;
                if (index < 0) return null;
                try {
                    if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0 || info.size == 0) continue;
                    ByteBuffer output = codec.getOutputBuffer(index);
                    if (output == null || info.size < 0 || info.size > 8191 - 7) {
                        throw new IllegalStateException("Invalid AAC output buffer");
                    }
                    byte[] raw = new byte[info.size];
                    output.position(info.offset);
                    output.get(raw);
                    return new EncodedAudio(raw, info.presentationTimeUs);
                } finally {
                    codec.releaseOutputBuffer(index, false);
                }
            }
            throw new IllegalStateException("AAC output did not settle");
        }

        @Override public void close() {
            try { codec.stop(); } catch (RuntimeException ignored) {}
            try { codec.release(); } catch (RuntimeException ignored) {}
        }
    }

    /**
     * 配置编码器并返回实际生效的音频配置（SS-07：AAC 初始化失败回退 LPCM 时，外层 RTSP 与采集帧长立即同步）
     */
    public AudioConfig configure(boolean preferLpcm, int reqSampleRate, Runnable onEncoderFailed) {
        synchronized (lock) {
            pendingPcm.clear();
            releaseEncoder();
            this.onEncoderFailed = onEncoderFailed;
            this.sampleRate = (reqSampleRate == 44100 && preferLpcm) ? 44100 : 48000;
            this.useLpcm = preferLpcm;
            this.codecFailures = 0;
            this.reportedInputOverflow = false;
            if (!preferLpcm && !initAacEncoder(this.sampleRate)) this.useLpcm = true;
            int frameSamples = this.useLpcm ? (this.sampleRate == 44100 ? 441 : 480) : 1024;
            return new AudioConfig(this.useLpcm, this.sampleRate, frameSamples);
        }
    }

    public boolean isUsingLpcm() {
        return useLpcm;
    }

    private boolean initAacEncoder(int sr) {
        try {
            aacEncoder = encoderFactory.create(sr);
            return true;
        } catch (Exception e) {
            releaseEncoder();
            return false;
        }
    }

    private void releaseEncoder() {
        acceptedPcmPts.clear();
        if (aacEncoder != null) {
            aacEncoder.close();
            aacEncoder = null;
        }
    }

    public void addTarget(String id, long sessionId, String ip, ConnectionWriter writer,
                          int delayBiasMs, Runnable failure) {
        try {
            TargetNode prev = targets.get(id);
            if (prev != null && sessionId > 0 && prev.sessionId > sessionId) {
                writer.abort();
                return;
            }
            int streamType = useLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC;
            TargetNode node = new TargetNode(id, sessionId, ip, writer, delayBiasMs, streamType, failure);
            TargetNode replaced = targets.put(id, node);
            if (replaced != null) {
                replaced.stopAndClose();
            }
        } catch (RuntimeException error) {
            writer.abort();
            failure.run();
        }
    }

    public void removeTarget(String id, long sessionId) {
        removeTarget(id, sessionId, false);
    }

    public void stopTargetForTeardown(String id, long sessionId) {
        removeTarget(id, sessionId, true);
    }

    private void removeTarget(String id, long sessionId, boolean rtspWillClose) {
        TargetNode cur = targets.get(id);
        if (cur == null) return;
        if (sessionId > 0 && cur.sessionId != sessionId) return;
        if (targets.remove(id, cur)) {
            if (rtspWillClose) cur.stopForTeardown();
            else cur.stopAndClose();
        }
    }

    public void removeTarget(String id) {
        removeTarget(id, 0L);
    }

    public void setDelayBias(String id, int biasMs) {
        int clamped = Math.max(-300, Math.min(300, biasMs));
        TargetNode n = targets.get(id);
        if (n != null) {
            n.delayBiasMs = clamped;
        }
    }

    public void clearAll() {
        List<TargetNode> toStop = new ArrayList<>();
        for (String id : new ArrayList<>(targets.keySet())) {
            TargetNode n = targets.remove(id);
            if (n != null) toStop.add(n);
        }
        for (TargetNode n : toStop) {
            n.stopAndClose();
        }
        synchronized (lock) {
            pendingPcm.clear();
            releaseEncoder();
            onEncoderFailed = null;
        }
    }

    public long getPacketsSent(String id) {
        TargetNode n = targets.get(id);
        return n != null ? n.packetsSent : 0L;
    }

    private static byte[] wrapAdts(byte[] rawAac, int sampleRate) {
        int frameLen = rawAac.length + 7;
        int freqIdx = (sampleRate == 44100) ? 4 : 3;
        byte[] out = new byte[frameLen];
        out[0] = (byte) 0xFF;
        out[1] = (byte) 0xF9;
        out[2] = (byte) ((1 << 6) | (freqIdx << 2) | (2 >> 2));
        out[3] = (byte) (((2 & 3) << 6) | ((frameLen >> 11) & 3));
        out[4] = (byte) ((frameLen >> 3) & 0xFF);
        out[5] = (byte) (((frameLen & 7) << 5) | 0x1F);
        out[6] = (byte) 0xFC;
        System.arraycopy(rawAac, 0, out, 7, rawAac.length);
        return out;
    }

    private static byte[] wrapWfdLpcm(byte[] pcmLe, int sampleRate) {
        byte[] out = new byte[4 + pcmLe.length];
        out[0] = (byte) 0xA0;
        out[1] = (byte) 0x06;
        out[2] = (byte) 0x00;
        out[3] = (byte) (sampleRate == 44100 ? 0x09 : 0x11);
        for (int idx = 0; idx + 1 < pcmLe.length; idx += 2) {
            out[4 + idx] = pcmLe[idx + 1];
            out[4 + idx + 1] = pcmLe[idx];
        }
        return out;
    }

    public void broadcastPcmFrame(byte[] stereoPcmLe, long samplePts90k,
                                  IntSupplier baseDelayMs) {
        synchronized (lock) {
            if (useLpcm) {
                if (!targets.isEmpty()) {
                    fanoutEncodedFrame(wrapWfdLpcm(stereoPcmLe, sampleRate),
                                       samplePts90k + baseDelayMs.getAsInt() * 90L);
                }
                return;
            }
            if (aacEncoder == null || (targets.isEmpty() && pendingPcm.isEmpty())) return;
            if (stereoPcmLe.length != 1024 * 4) throw new IllegalArgumentException("AAC-LC needs 1024 stereo samples");

            if (pendingPcm.size() == MAX_PENDING_PCM_FRAMES) {
                pendingPcm.removeFirst();
                if (!reportedInputOverflow) {
                    reportedInputOverflow = true;
                    Log.w("MiPlayRTP", "AAC input backpressure exceeded the PCM buffer");
                }
            }
            pendingPcm.addLast(new AudioFrame(stereoPcmLe.clone(), samplePts90k));
            try {
                drainAac(baseDelayMs);
                while (!pendingPcm.isEmpty()) {
                    if (acceptedPcmPts.size() >= 64) throw new IllegalStateException("AAC output stalled");
                    int index = aacEncoder.acquireInput();
                    if (index < 0) break;
                    AudioFrame frame = pendingPcm.getFirst();
                    ByteBuffer input = aacEncoder.inputBuffer(index);
                    if (input == null || input.capacity() < frame.data.length) {
                        throw new IllegalStateException("AAC input buffer is unavailable or too small");
                    }
                    input.clear();
                    input.put(frame.data);
                    aacEncoder.submitInput(index, frame.data.length, frame.pts * 1000L / 90L);
                    pendingPcm.removeFirst();
                    acceptedPcmPts.addLast(frame.pts);
                }
                drainAac(baseDelayMs);
            } catch (RuntimeException error) {
                // Releasing the codec returns every acquired slot, including an unsubmitted slot.
                reportCodecError.accept(error);
                releaseEncoder();
                if (++codecFailures >= 3 || !initAacEncoder(sampleRate)) {
                    pendingPcm.clear();
                    if (onEncoderFailed != null) onEncoderFailed.run();
                }
            }
        }
    }

    private void drainAac(IntSupplier baseDelayMs) {
        for (int attempts = 0; attempts < 64; attempts++) {
            EncodedAudio output = aacEncoder.takeOutput();
            if (output == null) return;
            if (acceptedPcmPts.isEmpty()) throw new IllegalStateException("AAC output has no corresponding PCM");
            // Match the codec timestamp on the PCM timeline. For absent or vendor-primed timestamps,
            // the ordered accepted-input queue supplies the exact 90 kHz value.
            Long matched = null;
            if (output.ptsUs >= 0) {
                for (long accepted : acceptedPcmPts) {
                    if (Math.abs(accepted * 1000L / 90L - output.ptsUs) <= 1) {
                        matched = accepted;
                        break;
                    }
                }
            }
            if (matched != null) {
                while (!acceptedPcmPts.getFirst().equals(matched)) acceptedPcmPts.removeFirst();
            }
            long pts = acceptedPcmPts.removeFirst() + baseDelayMs.getAsInt() * 90L;
            fanoutEncodedFrame(wrapAdts(output.data, sampleRate), pts);
            codecFailures = 0;
        }
        throw new IllegalStateException("AAC output did not settle");
    }

    private void fanoutEncodedFrame(byte[] encodedFrame, long pts) {
        AudioFrame frame = new AudioFrame(encodedFrame, pts);
        for (TargetNode node : targets.values()) node.enqueueFrame(frame);
    }
}
