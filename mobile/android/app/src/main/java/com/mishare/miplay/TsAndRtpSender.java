package com.mishare.miplay;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 1. Android MediaCodec 硬件 ADTS AAC-LC (48kHz Stereo, 0xFF 0xF9 0x4C) & WFD LPCM 编码
 * 2. 标准 MPEG-TS 打包器 (PAT 0x0000 / PMT 0x0100 / PCR 0x1000 / Audio PES 0x1100)
 * 3. 一源多发 RTP-over-TCP 分发引擎 (SSRC=0xDEADBEEF, PT=33, 支持单设备 -300..+300ms 延迟微调)
 */
public class TsAndRtpSender {
    public static final int STREAM_ADTS_AAC = 0x0F;
    public static final int STREAM_WFD_LPCM = 0x83;

    public static class TargetNode {
        public final String deviceId;
        public final String peerIp;
        public final Socket sock;
        public final OutputStream out;
        public int rtpSeq = 0;
        public volatile int delayBiasMs = 0;
        public volatile long packetsSent = 0;
        public volatile boolean healthy = true;

        public TargetNode(String id, String ip, Socket s, int bias) throws Exception {
            this.deviceId = id;
            this.peerIp = ip;
            this.sock = s;
            this.out = s.getOutputStream();
            this.delayBiasMs = bias;
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
            pkt[15] = (byte) 0xE1; pkt[16] = 0x00; // PMT PID 0x0100
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
            pkt[13] = (byte) 0xF0; pkt[14] = 0x00; // PCR PID 0x1000
            pkt[15] = (byte) 0xF0; pkt[16] = 0x00;
            pkt[17] = (byte) streamType;
            pkt[18] = (byte) 0xF1; pkt[19] = 0x00; // Audio PID 0x1100
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
            if (frameCounter % 5 == 0) {
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
    private final ConcurrentHashMap<String, TsPacker> biasedPackers = new ConcurrentHashMap<>();
    private TsPacker sharedPacker = new TsPacker(STREAM_ADTS_AAC);
    private MediaCodec aacEncoder;
    private boolean useLpcm = false;
    private int sampleRate = 48000;

    public void configure(boolean preferLpcm, int sampleRate) {
        synchronized (lock) {
            releaseEncoder();
            this.useLpcm = preferLpcm;
            this.sampleRate = sampleRate;
            int st = preferLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC;
            sharedPacker = new TsPacker(st);
            biasedPackers.clear();
            if (!preferLpcm) {
                initAacEncoder(sampleRate);
            }
        }
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
            this.sharedPacker = new TsPacker(STREAM_WFD_LPCM);
        }
    }

    private void releaseEncoder() {
        if (aacEncoder != null) {
            try { aacEncoder.stop(); } catch (Exception ignored) {}
            try { aacEncoder.release(); } catch (Exception ignored) {}
            aacEncoder = null;
        }
    }

    public void addTarget(String id, String ip, Socket sock, int delayBiasMs) {
        try {
            removeTarget(id);
            TargetNode node = new TargetNode(id, ip, sock, delayBiasMs);
            targets.put(id, node);
            if (delayBiasMs != 0) {
                biasedPackers.put(id, new TsPacker(useLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC));
            }
        } catch (Exception ignored) {}
    }

    public void removeTarget(String id) {
        TargetNode old = targets.remove(id);
        biasedPackers.remove(id);
        if (old != null) {
            try { old.sock.close(); } catch (Exception ignored) {}
        }
    }

    public void setDelayBias(String id, int biasMs) {
        int clamped = Math.max(-300, Math.min(300, biasMs));
        TargetNode n = targets.get(id);
        if (n != null) {
            n.delayBiasMs = clamped;
            if (clamped != 0) {
                biasedPackers.computeIfAbsent(id, k -> new TsPacker(useLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC));
            }
        }
    }

    public void clearAll() {
        for (String id : new ArrayList<>(targets.keySet())) {
            removeTarget(id);
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
        int freqIdx = (sampleRate == 44100) ? 4 : 3; // 3 = 48000Hz
        byte[] out = new byte[frameLen];
        out[0] = (byte) 0xFF;
        out[1] = (byte) 0xF9; // MPEG-2, no CRC (与 K90 抓包 0xFF 0xF9 0x4C 一致!)
        out[2] = (byte) ((1 << 6) | (freqIdx << 2) | (2 >> 2));
        out[3] = (byte) (((2 & 3) << 6) | ((frameLen >> 11) & 3));
        out[4] = (byte) ((frameLen >> 3) & 0xFF);
        out[5] = (byte) (((frameLen & 7) << 5) | 0x1F);
        out[6] = (byte) 0xFC;
        System.arraycopy(rawAac, 0, out, 7, rawAac.length);
        return out;
    }

    private static byte[] wrapWfdLpcm(byte[] pcmLe) {
        byte[] out = new byte[4 + pcmLe.length];
        out[0] = (byte) 0xA0; out[1] = 0x06; out[2] = 0x00; out[3] = 0x11;
        for (int i = 0; i + 1 < pcmLe.length; i += 2) {
            out[4 + i] = pcmLe[i + 1];     // Big-Endian high byte
            out[4 + i + 1] = pcmLe[i];     // Big-Endian low byte
        }
        return out;
    }

    public void broadcastPcmFrame(byte[] stereoPcmLe, long basePts90k) {
        if (targets.isEmpty()) return;
        synchronized (lock) {
            if (useLpcm || aacEncoder == null) {
                byte[] lpcm = wrapWfdLpcm(stereoPcmLe);
                fanoutEncodedFrame(lpcm, basePts90k);
                return;
            }
            try {
                int inIdx = aacEncoder.dequeueInputBuffer(10000);
                if (inIdx >= 0) {
                    ByteBuffer inBuf = aacEncoder.getInputBuffer(inIdx);
                    if (inBuf != null) {
                        inBuf.clear();
                        inBuf.put(stereoPcmLe);
                        aacEncoder.queueInputBuffer(inIdx, 0, stereoPcmLe.length, basePts90k * 1000L / 90L, 0);
                    }
                }
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                while (true) {
                    int outIdx = aacEncoder.dequeueOutputBuffer(info, 0);
                    if (outIdx < 0) break;
                    ByteBuffer outBuf = aacEncoder.getOutputBuffer(outIdx);
                    if (outBuf != null && info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        byte[] raw = new byte[info.size];
                        outBuf.position(info.offset);
                        outBuf.get(raw);
                        byte[] adts = wrapAdts(raw, sampleRate);
                        fanoutEncodedFrame(adts, basePts90k);
                    }
                    aacEncoder.releaseOutputBuffer(outIdx, false);
                }
            } catch (Exception ignored) {}
        }
    }

    private void fanoutEncodedFrame(byte[] encodedFrame, long basePts90k) {
        byte[] sharedTs = null;
        for (Map.Entry<String, TargetNode> entry : targets.entrySet()) {
            TargetNode node = entry.getValue();
            if (!node.healthy) continue;

            byte[] tsBytes;
            long nodePts = basePts90k;
            if (node.delayBiasMs == 0) {
                if (sharedTs == null) sharedTs = sharedPacker.pack(encodedFrame, basePts90k);
                tsBytes = sharedTs;
            } else {
                long delta90k = (long) node.delayBiasMs * 90L;
                nodePts = Math.max(0L, basePts90k + delta90k);
                TsPacker bp = biasedPackers.computeIfAbsent(
                        node.deviceId, k -> new TsPacker(useLpcm ? STREAM_WFD_LPCM : STREAM_ADTS_AAC));
                tsBytes = bp.pack(encodedFrame, nodePts);
            }

            int maxChunk = 7 * 188;
            for (int off = 0; off < tsBytes.length; off += maxChunk) {
                int len = Math.min(maxChunk, tsBytes.length - off);
                int rtpLen = 12 + len;
                byte[] pkt = new byte[4 + rtpLen];
                pkt[0] = 0x24; pkt[1] = 0x00;
                pkt[2] = (byte) ((rtpLen >>> 8) & 0xFF);
                pkt[3] = (byte) (rtpLen & 0xFF);
                pkt[4] = (byte) 0x80; pkt[5] = (byte) 0xA1; // PT=33 MP2T
                int seq = node.rtpSeq++ & 0xFFFF;
                pkt[6] = (byte) ((seq >>> 8) & 0xFF);
                pkt[7] = (byte) (seq & 0xFF);
                int rts = (int) (nodePts & 0xFFFFFFFFL);
                pkt[8]  = (byte) ((rts >>> 24) & 0xFF);
                pkt[9]  = (byte) ((rts >>> 16) & 0xFF);
                pkt[10] = (byte) ((rts >>> 8) & 0xFF);
                pkt[11] = (byte) (rts & 0xFF);
                pkt[12] = (byte) 0xDE; pkt[13] = (byte) 0xAD;
                pkt[14] = (byte) 0xBE; pkt[15] = (byte) 0xEF;
                System.arraycopy(tsBytes, off, pkt, 16, len);
                try {
                    node.out.write(pkt);
                    node.out.flush();
                    node.packetsSent++;
                } catch (Exception e) {
                    node.healthy = false;
                    break;
                }
            }
        }
    }
}
