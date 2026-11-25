package com.mishare.miplay;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
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
        public final OutputStream out;
        public int rtpSeq = 0;
        public volatile int delayBiasMs = 0;
        public volatile long packetsSent = 0;
        public volatile long droppedFrames = 0;
        public volatile boolean healthy = true;
        public final AtomicBoolean stopRequested = new AtomicBoolean(false);
        private final ArrayBlockingQueue<AudioFrame> queue = new ArrayBlockingQueue<>(MAX_QUEUED_FRAMES);
        private final TsPacker packer;
        private final ByteArrayOutputStream wireBatch = new ByteArrayOutputStream(3072);
        private final long frameDuration90k;
        private long lastPts = -1;
        public final Thread writerThread;

        private TargetNode(String id, long sessionId, String ip, Socket s, int bias, int streamType) throws Exception {
            this.deviceId = id;
            this.sessionId = sessionId;
            this.peerIp = ip;
            this.sock = s;
            try { s.setTcpNoDelay(true); } catch (Exception ignored) {}
            try { s.setSendBufferSize(Math.max(s.getSendBufferSize(), 65536)); } catch (Exception ignored) {}
            this.out = s.getOutputStream();
            this.delayBiasMs = bias;
            this.packer = new TsPacker(streamType);
            this.frameDuration90k = streamType == STREAM_WFD_LPCM ? 900 : 1920;
            this.writerThread = new Thread(this::writeLoop, "MiPlay-RtpOut-" + id + "-" + sessionId);
            this.writerThread.setDaemon(true);
            this.writerThread.setPriority(Thread.NORM_PRIORITY + 2);
            this.writerThread.start();
        }

        private void writeLoop() {
            while (!stopRequested.get() && healthy) {
                try {
                    AudioFrame frame = queue.poll(200, TimeUnit.MILLISECONDS);
                    if (frame == null || stopRequested.get()) continue;
                    writeFrame(frame);
                } catch (InterruptedException e) {
                    break;
                } catch (Exception e) {
                    healthy = false;
                    break;
                }
            }
        }

        private void writeFrame(AudioFrame frame) throws Exception {
            long pts = Math.max(0, frame.pts + delayBiasMs * 90L);
            // A lower delay may make buffered access units overlap already-sent audio.
            // Drop those whole units before assigning transport continuity counters.
            if (lastPts >= 0 && pts < lastPts + frameDuration90k) {
                droppedFrames++;
                return;
            }
            lastPts = pts;
            byte[] ts = packer.pack(frame.data, pts);
            wireBatch.reset();
            int pktCount = 0;
            byte[] hdr = new byte[16];
            for (int off = 0; off < ts.length && !stopRequested.get(); off += 7 * 188) {
                int length = Math.min(7 * 188, ts.length - off);
                int size = 12 + length;
                int seq = rtpSeq++ & 0xFFFF;
                hdr[0] = 0x24;
                hdr[1] = 0x00;
                hdr[2] = (byte) (size >>> 8);
                hdr[3] = (byte) size;
                hdr[4] = (byte) 0x80;
                hdr[5] = (byte) 0xA1;
                hdr[6] = (byte) (seq >>> 8);
                hdr[7] = (byte) seq;
                hdr[8] = (byte) (pts >>> 24);
                hdr[9] = (byte) (pts >>> 16);
                hdr[10] = (byte) (pts >>> 8);
                hdr[11] = (byte) pts;
                hdr[12] = (byte) 0xDE;
                hdr[13] = (byte) 0xAD;
                hdr[14] = (byte) 0xBE;
                hdr[15] = (byte) 0xEF;
                wireBatch.write(hdr, 0, 16);
                wireBatch.write(ts, off, length);
                pktCount++;
            }
            if (!stopRequested.get() && wireBatch.size() > 0) {
                wireBatch.writeTo(out);
                out.flush();
                packetsSent += pktCount;
            }
        }

        private void enqueueFrame(AudioFrame frame) {
            if (!healthy || stopRequested.get()) return;
            while (!queue.offer(frame)) {
                if (queue.poll() != null) droppedFrames++;
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

    private static class TsPacker {
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

        byte[] pack(byte[] payload, long pts90k) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(188 * 8);
            if (frameCounter % 4 == 0) {
                appendPat(out);
                appendPmt(out);
                appendPcr(out, pts90k);
            }
            frameCounter++;

            int pesBody = 3 + 7 + payload.length;
            int pesLenField = Math.min(pesBody, 0xFFFF);
            byte[] pes = new byte[16 + payload.length];
            pes[0] = 0; pes[1] = 0; pes[2] = 1;
            pes[3] = (byte) (streamType == STREAM_WFD_LPCM ? 0xBD : 0xC0);
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

    private final Object lock = new Object();
    private final ConcurrentHashMap<String, TargetNode> targets = new ConcurrentHashMap<>();
    private final ArrayDeque<AudioFrame> pendingPcm = new ArrayDeque<>();
    private final MediaCodec.BufferInfo outputInfo = new MediaCodec.BufferInfo();
    private MediaCodec aacEncoder;
    private volatile boolean useLpcm = false;
    private volatile int sampleRate = 48000;
    private long lastOutputPts90k = -1L;
    private boolean reportedInputOverflow;
    private boolean reportedCodecError;

    /**
     * 配置编码器并返回实际生效的音频配置（SS-07：AAC 初始化失败回退 LPCM 时，外层 RTSP 与采集帧长立即同步）
     */
    public AudioConfig configure(boolean preferLpcm, int reqSampleRate) {
        synchronized (lock) {
            releaseEncoder();
            this.sampleRate = (reqSampleRate == 44100 && preferLpcm) ? 44100 : 48000;
            this.useLpcm = preferLpcm;
            this.lastOutputPts90k = -1L;
            this.reportedInputOverflow = false;
            this.reportedCodecError = false;
            if (!preferLpcm) {
                initAacEncoder(this.sampleRate);
            }
            int frameSamples = this.useLpcm ? (this.sampleRate == 44100 ? 441 : 480) : 1024;
            return new AudioConfig(this.useLpcm, this.sampleRate, frameSamples);
        }
    }

    public boolean isUsingLpcm() {
        return useLpcm;
    }

    private void initAacEncoder(int sr) {
        try {
            aacEncoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
            MediaFormat fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, sr, 2);
            fmt.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, 256000);
            fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384);
            aacEncoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            aacEncoder.start();
        } catch (Exception e) {
            releaseEncoder();
            this.useLpcm = true;
        }
    }

    private void releaseEncoder() {
        pendingPcm.clear();
        if (aacEncoder != null) {
            try { aacEncoder.stop(); } catch (Exception ignored) {}
            try { aacEncoder.release(); } catch (Exception ignored) {}
            aacEncoder = null;
        }
    }

    public void addTarget(String id, long sessionId, String ip, Socket sock, int delayBiasMs) {
        try {
            TargetNode prev = targets.get(id);
            if (prev != null && sessionId > 0 && prev.sessionId > sessionId) {
                try { sock.close(); } catch (Exception ignored) {}
                return;
            }
            int streamType = useLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC;
            TargetNode node = new TargetNode(id, sessionId, ip, sock, delayBiasMs, streamType);
            TargetNode replaced = targets.put(id, node);
            if (replaced != null) {
                replaced.stopAndClose();
            }
        } catch (Exception ignored) {}
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
            releaseEncoder();
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

    /**
     * SS-08: 将 MediaCodec 输出帧的 presentationTimeUs 转换为精确的 90kHz PTS，
     * 并在单次 drain 产生多帧输出时保证单调递增（48kHz AAC 帧步进 = 1920 ticks）。
     */
    public static long computeOutputPts90k(long outputPtsUs, long fallbackPts90k, long prevPts90k, int sampleRate) {
        long pts90k = (outputPtsUs >= 0) ? ((outputPtsUs * 90L + 500L) / 1000L) : fallbackPts90k;
        long frameStep90k = (1024L * 90000L) / Math.max(1, sampleRate); // 48kHz -> 1920
        if (prevPts90k >= 0) {
            pts90k = Math.max(pts90k, prevPts90k + frameStep90k);
        }
        return pts90k;
    }

    public void broadcastPcmFrame(byte[] stereoPcmLe, long basePts90k) {
        synchronized (lock) {
            if (useLpcm) {
                if (!targets.isEmpty()) fanoutEncodedFrame(wrapWfdLpcm(stereoPcmLe, sampleRate), basePts90k);
                return;
            }
            if (aacEncoder == null) return;
            if (targets.isEmpty() && pendingPcm.isEmpty()) return;

            try {
                drainAac(basePts90k);
                if (pendingPcm.size() == MAX_PENDING_PCM_FRAMES) {
                    pendingPcm.removeFirst();
                    if (!reportedInputOverflow) {
                        reportedInputOverflow = true;
                        Log.w("MiPlayRTP", "AAC input backpressure exceeded the PCM buffer");
                    }
                }
                pendingPcm.addLast(new AudioFrame(stereoPcmLe.clone(), basePts90k));
                while (!pendingPcm.isEmpty()) {
                    int index = aacEncoder.dequeueInputBuffer(0);
                    if (index < 0) break;
                    AudioFrame frame = pendingPcm.removeFirst();
                    ByteBuffer input = aacEncoder.getInputBuffer(index);
                    if (input == null || input.capacity() < frame.data.length) {
                        throw new IllegalStateException("AAC input buffer is too small");
                    }
                    input.clear();
                    input.put(frame.data);
                    aacEncoder.queueInputBuffer(index, 0, frame.data.length, frame.pts * 1000L / 90L, 0);
                }
                drainAac(basePts90k);
            } catch (RuntimeException error) {
                if (!reportedCodecError) {
                    reportedCodecError = true;
                    Log.e("MiPlayRTP", "AAC encoding failed", error);
                }
            }
        }
    }

    private void drainAac(long fallbackPts) {
        while (true) {
            int index = aacEncoder.dequeueOutputBuffer(outputInfo, 0);
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ||
                index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (index < 0) return;
            try {
                ByteBuffer output = aacEncoder.getOutputBuffer(index);
                if (output == null || outputInfo.size == 0 ||
                    (outputInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) continue;
                byte[] raw = new byte[outputInfo.size];
                output.position(outputInfo.offset);
                output.get(raw);
                long pts = computeOutputPts90k(outputInfo.presentationTimeUs, fallbackPts,
                                               lastOutputPts90k, sampleRate);
                lastOutputPts90k = pts;
                fanoutEncodedFrame(wrapAdts(raw, sampleRate), pts);
            } finally {
                aacEncoder.releaseOutputBuffer(index, false);
            }
        }
    }

    private void fanoutEncodedFrame(byte[] encodedFrame, long pts) {
        AudioFrame frame = new AudioFrame(encodedFrame, pts);
        for (TargetNode node : targets.values()) node.enqueueFrame(frame);
    }
}
