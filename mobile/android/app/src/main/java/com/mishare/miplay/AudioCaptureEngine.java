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
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;

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
        void onPcmFrame(byte[] stereoPcmLe, int sampleFrames, long samplePosition);
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
    private final AtomicBoolean wavPaused = new AtomicBoolean(false);
    private volatile long wavDurationMs = 0L;
    private volatile long wavPositionMs = 0L;
    private volatile String wavLrcRaw = "";
    private volatile String wavLyricLine = "";
    private volatile java.util.List<LrcLine> wavLrcLines = java.util.Collections.emptyList();
    private volatile RecorderSource recorderSource;
    private final Object captureLock = new Object();
    private long captureGeneration;
    private Recorder activeRecorder;
    private long sessionOriginUs;
    private Thread worker;
    private Thread spectrumWorker;
    private final Object spectrumNotify = new Object();
    private final AtomicReference<float[]> pendingSpectrum = new AtomicReference<>(null);
    private Callbacks cbs;
    private int sampleRate = 48000;
    private int frameSamples = 1024;

    interface Recorder {
        void start();
        int read(byte[] pcm, int offset, int length);
        void stop();
        void release();
    }

    interface RecorderSource {
        Recorder create(int sampleRate, int frameSamples);
    }

    private final LongSupplier clock;
    private final Runnable audioPriority;

    public AudioCaptureEngine() {
        this(TimerService::monotonicNowUs, () ->
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO));
    }

    AudioCaptureEngine(LongSupplier clock, Runnable audioPriority) {
        this.clock = clock;
        this.audioPriority = audioPriority;
    }

    public int getMode() { return mode.get(); }
    public String getTrackTitle() { return trackTitle; }
    public String getTrackArtist() { return trackArtist; }
    public String getLastError() { return lastError; }
    public boolean isWavPaused() { return wavPaused.get(); }
    public long getWavDurationMs() { return wavDurationMs; }
    public long getWavPositionMs() { return wavPositionMs; }
    public String getWavLrcRaw() { return wavLrcRaw; }
    public String getWavLyricLine() { return wavLyricLine; }

    public static final class LrcLine {
        public final long timeMs;
        public final String text;
        public LrcLine(long timeMs, String text) {
            this.timeMs = timeMs;
            this.text = text;
        }
    }

    public static java.util.List<LrcLine> parseLrcLines(String rawText) {
        java.util.List<LrcLine> result = new java.util.ArrayList<>();
        if (rawText == null || rawText.isEmpty()) return result;
        String cleaned = rawText.startsWith("\uFEFF") ? rawText.substring(1) : rawText;
        long offsetMs = 0L;
        java.util.List<LrcLine> rawEntries = new java.util.ArrayList<>();
        for (String line : cleaned.split("\\r?\\n")) {
            if (line.isEmpty()) continue;
            java.util.List<Long> times = new java.util.ArrayList<>();
            int pos = 0;
            while (pos < line.length() && line.charAt(pos) == '[') {
                int close = line.indexOf(']', pos + 1);
                if (close < 0) break;
                String tag = line.substring(pos + 1, close);
                long ts = parseLrcTagMs(tag);
                if (ts >= 0L) {
                    times.add(ts);
                } else if (tag.toLowerCase(java.util.Locale.US).startsWith("offset:")) {
                    try { offsetMs = Long.parseLong(tag.substring(7).trim()); } catch (NumberFormatException ignored) {}
                }
                pos = close + 1;
            }
            if (times.isEmpty()) continue;
            String lyric = line.substring(pos).trim();
            for (long t : times) rawEntries.add(new LrcLine(t, lyric));
        }
        for (LrcLine e : rawEntries) {
            result.add(new LrcLine(Math.max(0L, e.timeMs - offsetMs), e.text));
        }
        result.sort(java.util.Comparator.comparingLong(a -> a.timeMs));
        return result;
    }

    public static String lrcLineAt(java.util.List<LrcLine> lines, long positionMs) {
        if (lines == null || lines.isEmpty()) return "";
        int lo = 0, hi = lines.size();
        while (lo < hi) {
            int mid = (lo + hi) >>> 1;
            if (lines.get(mid).timeMs <= positionMs) lo = mid + 1;
            else hi = mid;
        }
        return lo > 0 ? lines.get(lo - 1).text : "";
    }

    private static long parseLrcTagMs(String tag) {
        int colon = tag.indexOf(':');
        if (colon <= 0 || colon + 1 >= tag.length()) return -1L;
        try {
            long mins = Long.parseLong(tag.substring(0, colon));
            if (mins < 0 || mins > 9999) return -1L;
            String rest = tag.substring(colon + 1);
            int dot = rest.indexOf('.');
            if (dot < 0) dot = rest.indexOf(':');
            long secs;
            long fracMs = 0L;
            if (dot < 0) {
                secs = Long.parseLong(rest);
            } else {
                secs = Long.parseLong(rest.substring(0, dot));
                String f = rest.substring(dot + 1);
                if (f.isEmpty()) return -1L;
                if (f.length() == 1) fracMs = Long.parseLong(f) * 100L;
                else if (f.length() == 2) fracMs = Long.parseLong(f) * 10L;
                else fracMs = Long.parseLong(f.substring(0, 3));
            }
            if (secs < 0 || secs >= 60) return -1L;
            return (mins * 60L + secs) * 1000L + fracMs;
        } catch (NumberFormatException e) {
            return -1L;
        }
    }

    public void setWavPaused(boolean paused) {
        wavPaused.set(paused);
    }

    public void seekWavMs(long positionMs) {
        synchronized (captureLock) {
            byte[] wd = wavData;
            if (wd == null || wd.length < 4 || sampleRate <= 0) return;
            long totalFrames = wd.length / 4L;
            long targetFrame = (Math.max(0L, positionMs) * sampleRate) / 1000L;
            if (targetFrame >= totalFrames) targetFrame = Math.max(0L, totalFrames - 1L);
            wavCursor = (int) (targetFrame * 4L);
            wavPositionMs = (targetFrame * 1000L) / sampleRate;
            wavLyricLine = lrcLineAt(wavLrcLines, wavPositionMs);
        }
    }

    public void setWavLrcText(String rawLrc) {
        String text = rawLrc == null ? "" : (rawLrc.startsWith("\uFEFF") ? rawLrc.substring(1) : rawLrc);
        java.util.List<LrcLine> parsed = parseLrcLines(text);
        synchronized (captureLock) {
            wavLrcRaw = text;
            wavLrcLines = parsed;
            wavLyricLine = lrcLineAt(parsed, wavPositionMs);
        }
    }

    public void setMediaProjection(MediaProjection mp) {
        setCaptureSource(mp == null ? null : (rate, samples) -> buildPlaybackRecorder(mp, rate, samples));
    }

    void setCaptureSource(RecorderSource source) {
        synchronized (captureLock) {
            recorderSource = source;
            invalidateCapture();
            if (source != null) {
                lastError = "";
                updateMode(MODE_SYSTEM_LOOPBACK, "Android AudioPlaybackCapture");
            }
        }
    }

    public void revokeMediaProjection(String reason) {
        synchronized (captureLock) {
            recorderSource = null;
            invalidateCapture();
            if (mode.get() == MODE_SYSTEM_LOOPBACK) updateMode(MODE_TEST_TONE, "");
            if (reason != null && !reason.isEmpty()) lastError = reason;
        }
    }

    public boolean hasMediaProjection() {
        return recorderSource != null;
    }

    public void setMode(int newMode, String param) {
        synchronized (captureLock) {
            invalidateCapture();
            updateMode(newMode, param);
        }
    }

    private void updateMode(int newMode, String param) {
        mode.set(newMode);
        if (newMode == MODE_TEST_TONE) {
            trackTitle = "全屋同步校准测试音 (A4/C#5/E5)";
            trackArtist = "MiPlay Sync Synth";
            lastError = "";
        } else if (newMode == MODE_SYSTEM_LOOPBACK) {
            trackTitle = (recorderSource != null) ? "手机全局音频抓取中" : "等待授予全局抓音权限";
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
            synchronized (captureLock) {
                invalidateCapture();
                wavData = converted;
                wavCursor = 0;
                wavPaused.set(false);
                wavDurationMs = (converted.length / 4L) * 1000L / targetRate;
                wavPositionMs = 0L;
                wavLyricLine = lrcLineAt(wavLrcLines, 0L);
                updateMode(MODE_WAV_FILE, (displayName != null && !displayName.isEmpty()) ? displayName : "Local.wav");
            }
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

    // Called under captureLock. The worker retains release ownership until read returns.
    private void invalidateCapture() {
        captureGeneration++;
        pendingSpectrum.set(null);
        if (activeRecorder != null) {
            try { activeRecorder.stop(); } catch (RuntimeException ignored) {}
            activeRecorder = null;
        }
    }

    public void start(int sampleRate, int frameSamples, long sessionOriginUs, Callbacks cbs) {
        synchronized (lifecycleLock) {
            stopLocked();
            this.sampleRate = sampleRate;
            this.frameSamples = frameSamples;
            this.sessionOriginUs = sessionOriginUs;
            this.cbs = cbs;
            running.set(true);
            spectrumWorker = new Thread(this::runSpectrumLoop, "MiPlay-Spectrum");
            spectrumWorker.setDaemon(true);
            spectrumWorker.setPriority(Thread.NORM_PRIORITY - 1);
            spectrumWorker.start();
            worker = new Thread(this::runLoop, "MiPlay-AudioEngine");
            worker.setPriority(Thread.MAX_PRIORITY);
            worker.start();
        }
    }

    public void stop() {
        synchronized (lifecycleLock) {
            stopLocked();
        }
    }

    private static void joinWorker(Thread thread) {
        if (thread == null) return;
        thread.interrupt();
        boolean interrupted = false;
        while (thread.isAlive()) {
            try {
                thread.join();
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    private void stopLocked() {
        synchronized (captureLock) {
            running.set(false);
            invalidateCapture();
        }
        joinWorker(worker);
        joinWorker(spectrumWorker);
        worker = null;
        spectrumWorker = null;
    }

    private void runSpectrumLoop() {
        while (running.get()) {
            float[] bands = pendingSpectrum.getAndSet(null);
            if (bands == null) {
                synchronized (spectrumNotify) {
                    if (!running.get()) break;
                    bands = pendingSpectrum.getAndSet(null);
                    if (bands == null) {
                        try { spectrumNotify.wait(250); } catch (InterruptedException ignored) { break; }
                        continue;
                    }
                }
            }
            Callbacks listener = this.cbs;
            if (listener != null && running.get()) {
                try { listener.onSpectrum(bands); } catch (Throwable ignored) {}
            }
        }
    }

    private static Recorder buildPlaybackRecorder(MediaProjection mp, int rate, int frameSamples) {
        AudioPlaybackCaptureConfiguration cfg = new AudioPlaybackCaptureConfiguration.Builder(mp)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build();
        AudioFormat fmt = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build();
        int minBuf = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) minBuf = frameSamples * 4 * 8;
        AudioRecord audio = new AudioRecord.Builder()
                .setAudioFormat(fmt)
                .setBufferSizeInBytes(Math.max(minBuf * 2, frameSamples * 4 * 8))
                .setAudioPlaybackCaptureConfig(cfg)
                .build();
        return new Recorder() {
            @Override public void start() {
                if (audio.getState() != AudioRecord.STATE_INITIALIZED) throw new IllegalStateException("AudioRecord 初始化失败");
                audio.startRecording();
            }
            @Override public int read(byte[] pcm, int offset, int length) {
                return audio.read(pcm, offset, length, AudioRecord.READ_BLOCKING);
            }
            @Override public void stop() { audio.stop(); }
            @Override public void release() { audio.release(); }
        };
    }

    private Recorder createPlaybackRecorder(RecorderSource source, long generation) {
        Recorder rec = null;
        boolean published = false;
        try {
            rec = source.create(sampleRate, frameSamples);
            rec.start();
            synchronized (captureLock) {
                if (!running.get() || generation != captureGeneration) return null;
                activeRecorder = rec;
                published = true;
                return rec;
            }
        } catch (RuntimeException e) {
            return null;
        } finally {
            if (!published && rec != null) {
                try { rec.stop(); } catch (RuntimeException ignored) {}
                rec.release();
            }
        }
    }

    private void runLoop() {
        audioPriority.run();
        final int frameBytes = frameSamples * 4;
        final byte[] pcm = new byte[frameBytes];
        final long tickOriginUs = sessionOriginUs;
        long tickSamples = 0L;
        long sampleIdx = 0;
        long lastSpecUs = 0;
        int consecutiveReadErrors = 0;
        short tailL = 0, tailR = 0;

        Recorder recorder = null;
        long previousGeneration = -1;
        try {
            while (running.get()) {
                int m;
                long generation;
                RecorderSource source;
                byte[] waveData;
                int waveCursor;
                synchronized (captureLock) {
                    m = mode.get();
                    generation = captureGeneration;
                    source = recorderSource;
                    waveData = wavData;
                    waveCursor = wavCursor;
                    if (recorder != null && recorder != activeRecorder) {
                        recorder.release();
                        recorder = null;
                    }
                }
                if (generation != previousGeneration) {
                    consecutiveReadErrors = 0;
                    tailL = tailR = 0;
                    previousGeneration = generation;
                }
                if (m != MODE_SYSTEM_LOOPBACK || recorder == null) {
                    long nowSamples = Math.max(0, clock.getAsLong() - tickOriginUs) * sampleRate / 1_000_000L;
                    long dueSamples = (nowSamples / frameSamples) * frameSamples;
                    if (dueSamples > tickSamples) {
                        sampleIdx += dueSamples - tickSamples;
                        tickSamples = dueSamples;
                    }
                }
                boolean usedBlockingCapture = false;
                if (m == MODE_SYSTEM_LOOPBACK) {
                    if (recorder == null && source != null) {
                        recorder = createPlaybackRecorder(source, generation);
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
                            int alignedSampleOff = (read / 4) * 4;
                            if (alignedSampleOff >= 4) {
                                tailL = (short) ((pcm[alignedSampleOff - 4] & 0xFF) | (pcm[alignedSampleOff - 3] << 8));
                                tailR = (short) ((pcm[alignedSampleOff - 2] & 0xFF) | (pcm[alignedSampleOff - 1] << 8));
                            }
                            for (int off = alignedSampleOff; off < frameBytes; off += 4) {
                                tailL = (short) ((tailL * 240) / 256);
                                tailR = (short) ((tailR * 240) / 256);
                                pcm[off] = (byte) (tailL & 0xFF);
                                pcm[off + 1] = (byte) ((tailL >>> 8) & 0xFF);
                                pcm[off + 2] = (byte) (tailR & 0xFF);
                                pcm[off + 3] = (byte) ((tailR >>> 8) & 0xFF);
                            }
                            consecutiveReadErrors++;
                            if (consecutiveReadErrors >= 5) {
                                synchronized (captureLock) {
                                    if (activeRecorder == recorder) {
                                        try { activeRecorder.stop(); } catch (RuntimeException ignored) {}
                                        activeRecorder = null;
                                    }
                                }
                                consecutiveReadErrors = 0;
                            }
                        } else {
                            tailL = (short) ((pcm[frameBytes - 4] & 0xFF) | (pcm[frameBytes - 3] << 8));
                            tailR = (short) ((pcm[frameBytes - 2] & 0xFF) | (pcm[frameBytes - 1] << 8));
                            consecutiveReadErrors = 0;
                            usedBlockingCapture = true;
                        }
                    } else {
                        Arrays.fill(pcm, (byte) 0);
                    }
                } else if (m == MODE_WAV_FILE && waveData != null && waveData.length > 0) {
                    if (wavPaused.get()) {
                        Arrays.fill(pcm, (byte) 0);
                    } else {
                        byte[] wd = waveData;
                        int cur = waveCursor;
                        for (int i = 0; i < frameBytes; i++) {
                            pcm[i] = wd[cur];
                            cur++;
                            if (cur >= wd.length) cur = 0;
                        }
                        long posMs = ((cur / 4L) * 1000L) / Math.max(1, sampleRate);
                        String line = lrcLineAt(wavLrcLines, posMs);
                        synchronized (captureLock) {
                            if (generation == captureGeneration) {
                                wavCursor = cur;
                                wavPositionMs = posMs;
                                wavLyricLine = line;
                            }
                        }
                    }
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

                synchronized (captureLock) {
                    if (!running.get()) break;
                    if (generation != captureGeneration) continue;
                    if (cbs != null) {
                        cbs.onPcmFrame(pcm, frameSamples, tickSamples);
                        long nowSpec = clock.getAsLong();
                        if (nowSpec - lastSpecUs >= 120_000L) {
                            lastSpecUs = nowSpec;
                            pendingSpectrum.set(computeSpectrum(pcm, frameSamples));
                            synchronized (spectrumNotify) {
                                spectrumNotify.notify();
                            }
                        }
                    }
                }

                tickSamples += frameSamples;
                if (!usedBlockingCapture) {
                    long nextTickUs = tickOriginUs + (tickSamples * 1_000_000L) / sampleRate;
                    long waitUs = nextTickUs - clock.getAsLong();
                    if (waitUs > 500L) {
                        try { Thread.sleep(waitUs / 1000L, (int) ((waitUs % 1000L) * 1000L)); }
                        catch (InterruptedException ignored) { break; }
                    }
                }
            }
        } finally {
            synchronized (captureLock) {
                if (recorder != null && activeRecorder == recorder) invalidateCapture();
            }
            if (recorder != null) recorder.release();
        }
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
