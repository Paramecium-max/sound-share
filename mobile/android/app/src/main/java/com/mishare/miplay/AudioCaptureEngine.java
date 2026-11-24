package com.mishare.miplay;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.net.Uri;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Android 三模式音源采集引擎：
 * - SS-09: 严格 RIFF/WAV 解析（验证 fmt、支持 16/24 位 PCM 与 32 位 Float、单/双声道、重采样、奇数块填充、64MB 上限；失败保留原状态）
 * - SS-11: 单一抓音任务串行启停 + 可取消 AudioRecord + MediaProjection 撤销立即释放并回退
 */
public class AudioCaptureEngine {
    public static final int MODE_TEST_TONE = 0;
    public static final int MODE_WAV_FILE = 1;
    public static final int MODE_SYSTEM_LOOPBACK = 2;
    private static final int MAX_WAV_BYTES = 64 * 1024 * 1024;

    public interface Callbacks {
        void onPcmFrame(byte[] stereoPcmLe, int sampleFrames);
        void onSpectrum(float[] bands16);
    }

    private final Object lifecycleLock = new Object();
    private final AtomicInteger mode = new AtomicInteger(MODE_TEST_TONE);
    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile String trackTitle = "全屋同步校准测试音 (A4/C#5/E5)";
    private volatile String trackArtist = "MiPlay Android Core";
    private volatile String lastError = "";
    private volatile byte[] wavData = null;
    private volatile int wavCursor = 0;
    private volatile MediaProjection mediaProjection = null;
    private volatile AudioRecord activeRecorder = null;
    private Thread worker;
    private Callbacks cbs;
    private int sampleRate = 48000;
    private int frameSamples = 1024;

    public int getMode() { return mode.get(); }
    public String getTrackTitle() { return trackTitle; }
    public String getTrackArtist() { return trackArtist; }
    public String getLastError() { return lastError; }

    public void setMediaProjection(MediaProjection mp) {
        synchronized (lifecycleLock) {
            this.mediaProjection = mp;
            if (mp != null) {
                lastError = "";
                setMode(MODE_SYSTEM_LOOPBACK, "Android AudioPlaybackCapture");
            } else {
                releaseActiveRecorder();
            }
        }
    }

    public void revokeMediaProjection(String reason) {
        synchronized (lifecycleLock) {
            this.mediaProjection = null;
            releaseActiveRecorder();
            if (mode.get() == MODE_SYSTEM_LOOPBACK) {
                setMode(MODE_TEST_TONE, "");
            }
            if (reason != null && !reason.isEmpty()) {
                lastError = reason;
            }
        }
    }

    public boolean hasMediaProjection() {
        return mediaProjection != null;
    }

    public void setMode(int newMode, String param) {
        mode.set(newMode);
        if (newMode == MODE_TEST_TONE) {
            trackTitle = "全屋同步校准测试音 (A4/C#5/E5)";
            trackArtist = "MiPlay Sync Synth";
            lastError = "";
        } else if (newMode == MODE_SYSTEM_LOOPBACK) {
            trackTitle = (mediaProjection != null) ? "手机全局音频抓取中" : "等待授予全局抓音权限";
            trackArtist = "AudioPlaybackCapture";
        } else if (newMode == MODE_WAV_FILE && param != null && !param.isEmpty()) {
            trackTitle = param;
            trackArtist = "Local WAV Lossless";
            lastError = "";
        }
    }

    private static int readU16Le(byte[] b, int off) {
        return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8);
    }

    private static long readU32Le(byte[] b, int off) {
        return (b[off] & 0xFFL)
                | ((b[off + 1] & 0xFFL) << 8)
                | ((b[off + 2] & 0xFFL) << 16)
                | ((b[off + 3] & 0xFFL) << 24);
    }

    private static void readFully(InputStream in, byte[] dst, int len) throws Exception {
        int off = 0;
        while (off < len) {
            int r = in.read(dst, off, len - off);
            if (r < 0) throw new EOFException("WAV 数据截断");
            off += r;
        }
    }

    private static void skipFully(InputStream in, long n) throws Exception {
        long rem = n;
        byte[] scratch = new byte[4096];
        while (rem > 0) {
            int r = in.read(scratch, 0, (int) Math.min(scratch.length, rem));
            if (r < 0) throw new EOFException("WAV 块偏移超出文件末尾");
            rem -= r;
        }
    }

    /**
     * SS-09: 流式按 RIFF 块解析并校验 WAV，支持 16/24 位 PCM 及 32 位 Float、单/双声道、任意采样率重采样至当前 sampleRate；
     * 失败时保留上一个有效状态并记录 lastError。
     */
    public String loadWavFromUri(Context ctx, Uri uri, String displayName) {
        try (InputStream rawIn = ctx.getContentResolver().openInputStream(uri)) {
            if (rawIn == null) {
                lastError = "无法打开所选音频文件";
                return lastError;
            }
            BufferedInputStream in = new BufferedInputStream(rawIn, 16384);
            byte[] riff = new byte[12];
            readFully(in, riff, 12);
            if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F'
                    || riff[8] != 'W' || riff[9] != 'A' || riff[10] != 'V' || riff[11] != 'E') {
                lastError = "所选文件不是有效的 RIFF/WAV 格式";
                return lastError;
            }

            boolean hasFmt = false;
            int fmtTag = 0, channels = 0, srcRate = 0, blockAlign = 0, bits = 0;
            byte[] rawData = null;

            byte[] chdr = new byte[8];
            while (true) {
                int r = in.read(chdr, 0, 8);
                if (r < 0) break;
                if (r < 8) {
                    int off = r;
                    while (off < 8) {
                        int m = in.read(chdr, off, 8 - off);
                        if (m < 0) throw new EOFException("WAV 块头不完整");
                        off += m;
                    }
                }
                String cid = new String(chdr, 0, 4, "US-ASCII");
                long sz = readU32Le(chdr, 4);
                if (sz > MAX_WAV_BYTES) {
                    lastError = "WAV 块超过 64 MB 大小上限";
                    return lastError;
                }
                int pad = (int) (sz & 1L);

                if ("fmt ".equals(cid)) {
                    if (sz < 16) {
                        lastError = "WAV fmt 格式块不完整";
                        return lastError;
                    }
                    byte[] fmt = new byte[(int) sz];
                    readFully(in, fmt, (int) sz);
                    if (pad == 1) skipFully(in, 1);
                    fmtTag = readU16Le(fmt, 0);
                    channels = readU16Le(fmt, 2);
                    srcRate = (int) readU32Le(fmt, 4);
                    blockAlign = readU16Le(fmt, 12);
                    bits = readU16Le(fmt, 14);
                    if (fmtTag == 0xFFFE && fmt.length >= 40) {
                        fmtTag = readU16Le(fmt, 24);
                    }
                    hasFmt = true;
                } else if ("data".equals(cid)) {
                    if (sz == 0) {
                        lastError = "WAV data 数据块为空";
                        return lastError;
                    }
                    rawData = new byte[(int) sz];
                    readFully(in, rawData, (int) sz);
                    if (hasFmt) break;
                    if (pad == 1) skipFully(in, 1);
                } else {
                    skipFully(in, sz + pad);
                }
            }

            if (!hasFmt || rawData == null) {
                lastError = !hasFmt ? "WAV 缺少 fmt 格式块" : "WAV 缺少 data 音频数据块";
                return lastError;
            }
            if (channels != 1 && channels != 2) {
                lastError = "仅支持单声道或立体声 WAV（当前: " + channels + " 声道）";
                return lastError;
            }
            if (srcRate < 8000 || srcRate > 192000) {
                lastError = "不支持的 WAV 采样率: " + srcRate + " Hz";
                return lastError;
            }
            boolean isPcm16 = (fmtTag == 1 && bits == 16);
            boolean isPcm24 = (fmtTag == 1 && bits == 24);
            boolean isFlt32 = (fmtTag == 3 && bits == 32);
            if (!isPcm16 && !isPcm24 && !isFlt32) {
                lastError = "仅支持 16/24 位 PCM 或 32 位浮点 WAV";
                return lastError;
            }
            int expectedAlign = channels * (bits / 8);
            if (expectedAlign == 0 || (blockAlign != 0 && blockAlign != expectedAlign)) {
                lastError = "WAV blockAlign 与位宽/声道不匹配";
                return lastError;
            }
            if ((rawData.length % expectedAlign) != 0) {
                lastError = "WAV data 长度未按采样帧对齐";
                return lastError;
            }

            int srcFrames = rawData.length / expectedAlign;
            short[] stereoSrc = new short[srcFrames * 2];
            int bps = bits / 8;
            for (int i = 0; i < srcFrames; i++) {
                int base = i * expectedAlign;
                short l = decodeSample(rawData, base, isPcm16, isPcm24);
                short r = (channels == 2) ? decodeSample(rawData, base + bps, isPcm16, isPcm24) : l;
                stereoSrc[i * 2] = l;
                stereoSrc[i * 2 + 1] = r;
            }

            int targetRate = (sampleRate > 0) ? sampleRate : 48000;
            byte[] converted;
            if (srcRate == targetRate) {
                converted = new byte[srcFrames * 4];
                ByteBuffer bb = ByteBuffer.wrap(converted).order(ByteOrder.LITTLE_ENDIAN);
                for (short s : stereoSrc) bb.putShort(s);
            } else {
                double ratio = (double) srcRate / (double) targetRate;
                int dstFrames = Math.max(1, (int) Math.round(srcFrames / ratio));
                converted = new byte[dstFrames * 4];
                ByteBuffer bb = ByteBuffer.wrap(converted).order(ByteOrder.LITTLE_ENDIAN);
                for (int o = 0; o < dstFrames; o++) {
                    double pos = o * ratio;
                    int i0 = Math.min(srcFrames - 1, (int) pos);
                    int i1 = Math.min(srcFrames - 1, i0 + 1);
                    double frac = pos - i0;
                    double l = stereoSrc[i0 * 2] * (1.0 - frac) + stereoSrc[i1 * 2] * frac;
                    double r = stereoSrc[i0 * 2 + 1] * (1.0 - frac) + stereoSrc[i1 * 2 + 1] * frac;
                    bb.putShort((short) Math.max(-32768, Math.min(32767, Math.round(l))));
                    bb.putShort((short) Math.max(-32768, Math.min(32767, Math.round(r))));
                }
            }

            // 全部验证与转换通过后才原子提交新曲目状态
            wavData = converted;
            wavCursor = 0;
            lastError = "";
            setMode(MODE_WAV_FILE, (displayName != null && !displayName.isEmpty()) ? displayName : "Local.wav");
            return null;
        } catch (Exception e) {
            lastError = "WAV 加载失败: " + e.getMessage();
            return lastError;
        }
    }

    private static short decodeSample(byte[] raw, int off, boolean isPcm16, boolean isPcm24) {
        if (isPcm16) {
            return (short) ((raw[off] & 0xFF) | (raw[off + 1] << 8));
        }
        if (isPcm24) {
            int v = (raw[off] & 0xFF) | ((raw[off + 1] & 0xFF) << 8) | (raw[off + 2] << 16);
            return (short) (v >> 8);
        }
        int bits = (raw[off] & 0xFF)
                | ((raw[off + 1] & 0xFF) << 8)
                | ((raw[off + 2] & 0xFF) << 16)
                | ((raw[off + 3] & 0xFF) << 24);
        float f = Float.intBitsToFloat(bits);
        if (!Float.isFinite(f)) f = 0.0f;
        f = Math.max(-1.0f, Math.min(1.0f, f));
        return (short) Math.round(f * 32767.0f);
    }

    private void releaseActiveRecorder() {
        AudioRecord rec = activeRecorder;
        activeRecorder = null;
        if (rec != null) {
            try { rec.stop(); } catch (Exception ignored) {}
            try { rec.release(); } catch (Exception ignored) {}
        }
    }

    public void start(int sampleRate, int frameSamples, Callbacks cbs) {
        synchronized (lifecycleLock) {
            // SS-11: 确保旧 worker 完全退出后才启动新 worker，绝不同时存在两个抓音线程
            stopLocked();
            this.sampleRate = sampleRate;
            this.frameSamples = frameSamples;
            this.cbs = cbs;
            running.set(true);
            worker = new Thread(this::runLoop, "MiPlay-AudioEngine");
            worker.setPriority(Thread.NORM_PRIORITY + 1);
            worker.start();
        }
    }

    public void stop() {
        synchronized (lifecycleLock) {
            stopLocked();
        }
    }

    private void stopLocked() {
        running.set(false);
        // 主动停止并释放正在阻塞 read() 的 AudioRecord，确保 worker 立即被唤醒退出
        releaseActiveRecorder();
        Thread w = worker;
        if (w != null) {
            w.interrupt();
            while (w.isAlive()) {
                try {
                    w.join(500);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    break;
                }
                if (w.isAlive()) {
                    releaseActiveRecorder();
                    w.interrupt();
                }
            }
            worker = null;
        }
    }

    private AudioRecord createPlaybackRecorder() {
        MediaProjection mp = mediaProjection;
        if (mp == null || !running.get()) return null;
        try {
            AudioPlaybackCaptureConfiguration cfg = new AudioPlaybackCaptureConfiguration.Builder(mp)
                    .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(AudioAttributes.USAGE_GAME)
                    .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                    .build();
            AudioFormat fmt = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                    .build();
            int minBuf = AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            if (minBuf <= 0) minBuf = frameSamples * 4 * 4;
            AudioRecord rec = new AudioRecord.Builder()
                    .setAudioFormat(fmt)
                    .setBufferSizeInBytes(Math.max(minBuf, frameSamples * 4 * 4))
                    .setAudioPlaybackCaptureConfig(cfg)
                    .build();
            if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
                rec.release();
                return null;
            }
            rec.startRecording();
            activeRecorder = rec;
            return rec;
        } catch (Throwable e) {
            return null;
        }
    }

    private void runLoop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
        final int frameBytes = frameSamples * 4;
        final byte[] pcm = new byte[frameBytes];
        final long frameUs = (frameSamples * 1_000_000L) / sampleRate;
        long nextTickUs = TimerService.monotonicNowUs();
        long sampleIdx = 0;
        long lastSpecUs = 0;
        int consecutiveReadErrors = 0;

        while (running.get()) {
            int m = mode.get();
            if (m != MODE_SYSTEM_LOOPBACK && activeRecorder != null) {
                releaseActiveRecorder();
                consecutiveReadErrors = 0;
            }

            boolean usedBlockingCapture = false;
            if (m == MODE_SYSTEM_LOOPBACK) {
                AudioRecord recorder = activeRecorder;
                if (recorder == null && mediaProjection != null) {
                    recorder = createPlaybackRecorder();
                }
                if (recorder != null) {
                    int read = 0;
                    boolean err = false;
                    while (read < frameBytes && running.get()) {
                        int r;
                        try {
                            r = recorder.read(pcm, read, frameBytes - read);
                        } catch (Exception ex) {
                            err = true;
                            break;
                        }
                        if (r <= 0) {
                            err = true;
                            break;
                        }
                        read += r;
                    }
                    if (err) {
                        Arrays.fill(pcm, read, frameBytes, (byte) 0);
                        consecutiveReadErrors++;
                        if (consecutiveReadErrors >= 5) {
                            releaseActiveRecorder();
                            consecutiveReadErrors = 0;
                        }
                    } else {
                        consecutiveReadErrors = 0;
                        usedBlockingCapture = true;
                    }
                } else {
                    Arrays.fill(pcm, (byte) 0);
                }
            } else if (m == MODE_WAV_FILE && wavData != null && wavData.length > 0) {
                byte[] wd = wavData;
                int cur = wavCursor;
                for (int i = 0; i < frameBytes; i++) {
                    pcm[i] = wd[cur];
                    cur++;
                    if (cur >= wd.length) cur = 0;
                }
                wavCursor = cur;
            } else {
                ByteBuffer bb = ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN);
                for (int i = 0; i < frameSamples; i++) {
                    double t = (double) (sampleIdx++) / sampleRate;
                    double env = 0.18 + 0.10 * Math.sin(2.0 * Math.PI * t);
                    double sig = 0.5 * Math.sin(2.0 * Math.PI * 440.0 * t)
                               + 0.3 * Math.sin(2.0 * Math.PI * 554.37 * t)
                               + 0.2 * Math.sin(2.0 * Math.PI * 659.25 * t);
                    short s = (short) (sig * env * 32767.0);
                    bb.putShort(s);
                    bb.putShort(s);
                }
            }

            if (!running.get()) break;
            if (mode.get() != m) {
                nextTickUs = TimerService.monotonicNowUs();
                continue;
            }
            if (cbs != null) {
                cbs.onPcmFrame(pcm, frameSamples);
                long nowSpec = TimerService.monotonicNowUs();
                if (nowSpec - lastSpecUs >= 120_000L) {
                    lastSpecUs = nowSpec;
                    cbs.onSpectrum(computeSpectrum(pcm, frameSamples));
                }
            }

            if (!usedBlockingCapture) {
                nextTickUs += frameUs;
                long now = TimerService.monotonicNowUs();
                long waitUs = nextTickUs - now;
                if (waitUs > 1000L) {
                    try { Thread.sleep(waitUs / 1000L, (int) ((waitUs % 1000L) * 1000L)); }
                    catch (InterruptedException ignored) { break; }
                } else if (now - nextTickUs > 100_000L) {
                    nextTickUs = now;
                }
            } else {
                nextTickUs = TimerService.monotonicNowUs();
            }
        }

        releaseActiveRecorder();
    }

    private static float[] computeSpectrum(byte[] pcm, int frames) {
        float[] bands = new float[16];
        int step = Math.max(1, frames / 16);
        for (int b = 0; b < 16; b++) {
            double sum = 0;
            int cnt = 0;
            for (int i = b * step; i < Math.min(frames, (b + 1) * step); i++) {
                int off = i * 4;
                short l = (short) ((pcm[off] & 0xFF) | (pcm[off + 1] << 8));
                short r = (short) ((pcm[off + 2] & 0xFF) | (pcm[off + 3] << 8));
                double v = (l + r) * 0.5 / 32768.0;
                sum += v * v;
                cnt++;
            }
            bands[b] = Math.min(1.0f, (float) (cnt > 0 ? Math.sqrt(sum / cnt) * 3.5 : 0.0));
        }
        return bands;
    }
}
