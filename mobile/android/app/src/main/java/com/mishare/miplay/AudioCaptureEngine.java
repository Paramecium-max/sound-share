package com.mishare.miplay;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.net.Uri;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Android 三模式音源采集引擎（已彻底消除零休眠死循环与 CPU 满载卡死风险）：
 * 0 = 全屋同步校准测试音 (零权限)
 * 1 = 本地无损 WAV 播放器
 * 2 = Android 10+ MediaProjection + AudioPlaybackCapture 系统全局抓音
 */
public class AudioCaptureEngine {
    public static final int MODE_TEST_TONE = 0;
    public static final int MODE_WAV_FILE = 1;
    public static final int MODE_SYSTEM_LOOPBACK = 2;

    public interface Callbacks {
        void onPcmFrame(byte[] stereoPcmLe, int sampleFrames);
        void onSpectrum(float[] bands16);
    }

    private final AtomicInteger mode = new AtomicInteger(MODE_TEST_TONE);
    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile String trackTitle = "全屋同步校准测试音 (A4/C#5/E5)";
    private volatile String trackArtist = "MiPlay Android Core";
    private volatile byte[] wavData = null;
    private volatile int wavCursor = 0;
    private volatile MediaProjection mediaProjection = null;
    private Thread worker;
    private Callbacks cbs;
    private int sampleRate = 48000;
    private int frameSamples = 1024;

    public int getMode() { return mode.get(); }
    public String getTrackTitle() { return trackTitle; }
    public String getTrackArtist() { return trackArtist; }

    public void setMediaProjection(MediaProjection mp) {
        this.mediaProjection = mp;
        if (mp != null) {
            setMode(MODE_SYSTEM_LOOPBACK, "Android AudioPlaybackCapture");
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
        } else if (newMode == MODE_SYSTEM_LOOPBACK) {
            trackTitle = (mediaProjection != null) ? "手机全局音频抓取中" : "等待授予全局抓音权限";
            trackArtist = "AudioPlaybackCapture";
        } else if (newMode == MODE_WAV_FILE && param != null && !param.isEmpty()) {
            trackTitle = param;
            trackArtist = "Local WAV Lossless";
        }
    }

    public boolean loadWavFromUri(Context ctx, Uri uri, String displayName) {
        try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
            if (in == null) return false;
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            byte[] all = bos.toByteArray();
            if (all.length <= 44) return false;
            for (int i = 12; i + 8 < all.length; ) {
                String id = new String(all, i, 4);
                int sz = (all[i + 4] & 0xFF) | ((all[i + 5] & 0xFF) << 8)
                       | ((all[i + 6] & 0xFF) << 16) | ((all[i + 7] & 0xFF) << 24);
                if ("data".equals(id)) {
                    int dataOff = i + 8;
                    int copyLen = Math.min(sz, all.length - dataOff);
                    if (copyLen <= 0) return false;
                    byte[] pcm = new byte[copyLen];
                    System.arraycopy(all, dataOff, pcm, 0, copyLen);
                    wavData = pcm;
                    wavCursor = 0;
                    setMode(MODE_WAV_FILE, displayName != null ? displayName : "Local.wav");
                    return true;
                }
                if (sz < 0) break;
                i += 8 + sz;
            }
            return false;
        } catch (Exception e) {
            return false;
        }
    }

    public void start(int sampleRate, int frameSamples, Callbacks cbs) {
        if (running.get()) return;
        this.sampleRate = sampleRate;
        this.frameSamples = frameSamples;
        this.cbs = cbs;
        running.set(true);
        worker = new Thread(this::runLoop, "MiPlay-AudioEngine");
        // 不使用 MAX_PRIORITY，防止任何异常抢占 Android 系统主线程导致卡死
        worker.setPriority(Thread.NORM_PRIORITY + 1);
        worker.start();
    }

    public void stop() {
        if (!running.getAndSet(false)) return;
        if (worker != null) {
            worker.interrupt();
            try { worker.join(500); } catch (Exception ignored) {}
            worker = null;
        }
    }

    private AudioRecord createPlaybackRecorder() {
        MediaProjection mp = mediaProjection;
        if (mp == null) return null;
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
            return rec;
        } catch (Throwable e) {
            return null;
        }
    }

    private void runLoop() {
        final int frameBytes = frameSamples * 4;
        final byte[] pcm = new byte[frameBytes];
        final long frameUs = (frameSamples * 1_000_000L) / sampleRate;
        long nextTickUs = TimerService.monotonicNowUs();
        long sampleIdx = 0;
        long lastSpecUs = 0;
        AudioRecord recorder = null;
        int consecutiveReadErrors = 0;

        while (running.get()) {
            int m = mode.get();
            if (m != MODE_SYSTEM_LOOPBACK && recorder != null) {
                try { recorder.stop(); recorder.release(); } catch (Exception ignored) {}
                recorder = null;
                consecutiveReadErrors = 0;
            }

            boolean usedBlockingCapture = false;
            if (m == MODE_SYSTEM_LOOPBACK) {
                if (recorder == null && mediaProjection != null) {
                    recorder = createPlaybackRecorder();
                }
                if (recorder != null) {
                    int read = 0;
                    boolean err = false;
                    while (read < frameBytes && running.get()) {
                        int r = recorder.read(pcm, read, frameBytes - read);
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
                            try { recorder.stop(); recorder.release(); } catch (Exception ignored) {}
                            recorder = null;
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

            if (cbs != null) {
                cbs.onPcmFrame(pcm, frameSamples);
                long nowSpec = TimerService.monotonicNowUs();
                // 严格限频：最多每 120ms (约 8fps) 推送一次频谱，绝不灌爆 WebView
                if (nowSpec - lastSpecUs >= 120_000L) {
                    lastSpecUs = nowSpec;
                    cbs.onSpectrum(computeSpectrum(pcm, frameSamples));
                }
            }

            // 无论何种模式，只要不是正常阻塞读取到的音频帧，一律强制按帧节拍休眠，彻底杜绝 100% CPU 空转！
            if (!usedBlockingCapture) {
                nextTickUs += frameUs;
                long now = TimerService.monotonicNowUs();
                long waitUs = nextTickUs - now;
                if (waitUs > 1000L) {
                    try { Thread.sleep(waitUs / 1000L, (int) ((waitUs % 1000L) * 1000L)); }
                    catch (InterruptedException ignored) { break; }
                } else {
                    if (now - nextTickUs > 100_000L) nextTickUs = now;
                    try { Thread.sleep(5); } catch (InterruptedException ignored) { break; }
                }
            } else {
                nextTickUs = TimerService.monotonicNowUs();
            }
        }

        if (recorder != null) {
            try { recorder.stop(); recorder.release(); } catch (Exception ignored) {}
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
