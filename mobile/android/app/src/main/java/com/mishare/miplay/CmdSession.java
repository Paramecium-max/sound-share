package com.mishare.miplay;

import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import org.json.JSONObject;

/**
 * TCP 8899 二进制控制通道（严格对齐实机 9 字节头 + 0x0028/0x0029 挑战认证 + 0x1400~0x1403 Safety 双向加密）
 * 帧头格式: [0x24('$')][cmd:u16be][seq:u16be][plen:u32be]
 */
public class CmdSession {
    private static final String TAG = "MiPlayCmd8899";

    public static final int CMD_OPEN                 = 0x0000;
    public static final int CMD_CLOSE                = 0x0002;
    public static final int CMD_PAUSE                = 0x0004;
    public static final int CMD_PAUSE_ACK            = 0x0005;
    public static final int CMD_RESUME               = 0x0006;
    public static final int CMD_RESUME_ACK           = 0x0007;
    public static final int CMD_SET_VOLUME           = 0x000C;
    public static final int CMD_SET_MEDIA_INFO       = 0x0012;
    public static final int CMD_HEARTBEAT            = 0x001A;
    public static final int CMD_GET_DEVICE_INFO      = 0x001E;
    public static final int CMD_NOTIFY               = 0x0022;
    public static final int CMD_CHALLENGE            = 0x0028;
    public static final int CMD_CHALLENGE_ACK        = 0x0029;
    public static final int CMD_GET_VERSION          = 0x0036;
    public static final int CMD_GET_VERSION_ACK      = 0x0037;
    public static final int CMD_SET_PLAY_SOURCE      = 0x0040;
    public static final int CMD_SET_POSITION         = 0x0056;
    public static final int CMD_SET_POSITION_ACK     = 0x0057;
    public static final int CMD_SET_LOCAL_DEV_INFO   = 0x0058;
    public static final int CMD_SAFETY_INFO          = 0x1400;
    public static final int CMD_SAFETY_INFO_ACK      = 0x1401;
    public static final int CMD_SAFETY_AUTH          = 0x1402;
    public static final int CMD_SAFETY_AUTH_ACK      = 0x1403;

    public enum RemoteControlAction {
        UNKNOWN,
        PAUSE,
        RESUME,
        NEXT,
        PREVIOUS,
        SEEK,
        SET_VOLUME
    }

    public static final class RemoteControlEvent {
        public final RemoteControlAction action;
        public final long positionMs;
        public final int volume;
        public final String rawKey;

        public RemoteControlEvent(RemoteControlAction action, long positionMs, int volume, String rawKey) {
            this.action = action;
            this.positionMs = positionMs;
            this.volume = volume;
            this.rawKey = rawKey == null ? "" : rawKey;
        }
    }

    public static final class MediaInfo {
        public String title = "";
        public String artist = "";
        public String album = "";
        public long durationMs = 0L;
        public long positionMs = 0L;
        public String lyrics = "";
        public String lyricLine = "";
        public String sourceName = "MiPlay Android";
        public int metaChangeType = 0;
    }

    public interface Callbacks {
        void onVersion(String remoteVersion);
        void onStateChanged(boolean connected);
        void onStatus(String status);
        default void onRemoteControl(RemoteControlEvent ev) {}
    }

    private static class SafetyCipher {
        private final byte[] key;
        private final byte[] iv;

        SafetyCipher(byte[] key16, byte[] iv16) {
            this.key = Arrays.copyOf(key16, 16);
            this.iv = Arrays.copyOf(iv16, 16);
        }

        SafetyCipher copy() {
            return new SafetyCipher(this.key, this.iv);
        }

        byte[] encryptBlob(byte[] plaintext) throws Exception {
            int pad = 16 - (plaintext.length % 16);
            byte[] padded = new byte[plaintext.length + pad];
            System.arraycopy(plaintext, 0, padded, 0, plaintext.length);

            Cipher cipher = Cipher.getInstance("AES/CBC/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(iv));
            byte[] ct = cipher.doFinal(padded);
            System.arraycopy(ct, ct.length - 16, iv, 0, 16);

            int crc = crc32Mpeg2(ct);
            byte[] blob = new byte[9 + ct.length];
            blob[0] = 0x00;
            blob[1] = 0x07;
            blob[2] = 0x01;
            blob[3] = (byte) 0xE0;
            blob[4] = (byte) pad;
            blob[5] = (byte) (crc & 0xFF);
            blob[6] = (byte) ((crc >>> 8) & 0xFF);
            blob[7] = (byte) ((crc >>> 16) & 0xFF);
            blob[8] = (byte) ((crc >>> 24) & 0xFF);
            System.arraycopy(ct, 0, blob, 9, ct.length);
            return blob;
        }

        byte[] decryptBlob(byte[] blob) {
            if (blob == null || blob.length < 25) return null;
            int hlen = ((blob[0] & 0xFF) << 8) | (blob[1] & 0xFF);
            int ver = blob[2] & 0xFF;
            int flags = blob[3] & 0xFF;
            int pad = blob[4] & 0xFF;
            int expectedCrc = (blob[5] & 0xFF)
                    | ((blob[6] & 0xFF) << 8)
                    | ((blob[7] & 0xFF) << 16)
                    | ((blob[8] & 0xFF) << 24);
            int ctLen = blob.length - 9;
            if (hlen != 7 || ver != 1 || flags != 0xE0 || pad < 1 || pad > 16 || (ctLen % 16) != 0) {
                return null;
            }
            byte[] ct = Arrays.copyOfRange(blob, 9, blob.length);
            if (crc32Mpeg2(ct) != expectedCrc) return null;
            try {
                Cipher cipher = Cipher.getInstance("AES/CBC/NoPadding");
                cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(iv));
                byte[] plain = cipher.doFinal(ct);
                for (int i = 0; i < pad; i++) {
                    if (plain[plain.length - 1 - i] != 0) return null;
                }
                System.arraycopy(ct, ct.length - 16, iv, 0, 16);
                return Arrays.copyOf(plain, plain.length - pad);
            } catch (Exception e) {
                return null;
            }
        }
    }

    private final String ip;
    private final int port;
    private String localIp = "";
    private int localPort = 0;

    private final AtomicBoolean aborted = new AtomicBoolean(false);
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicBoolean connected = new AtomicBoolean(false);
    private final AtomicBoolean safetyActive = new AtomicBoolean(false);
    private final AtomicBoolean handshakeReady = new AtomicBoolean(false);
    private final AtomicInteger seq = new AtomicInteger(1);
    private final Object sendLock = new Object();

    private volatile Socket socket;
    private volatile OutputStream out;
    private Thread recvTh;
    private Thread hbTh;
    private Callbacks cbs;

    private volatile CountDownLatch legacyLatch;
    private volatile CountDownLatch versionLatch;
    private volatile CountDownLatch safetyInfoLatch;
    private volatile CountDownLatch safetyAuthLatch;
    private volatile boolean legacyAuthDone = false;
    private volatile boolean versionAckReceived = false;
    private volatile boolean safetyInfoOk = false;
    private volatile boolean peerChallengeAcked = false;
    private volatile boolean localChallengeVerified = false;
    private volatile String failureReason = "";

    private String authKey = "";
    private SafetyCipher encCipher;
    private SafetyCipher decCipher;
    private SafetyCipher decCandType1;
    private SafetyCipher decCandType2;

    private final String bindIp;

    public CmdSession(String ip, int port, String bindIp) {
        this.ip = ip;
        this.port = port;
        this.bindIp = bindIp;
    }

    public boolean isConnected() {
        return connected.get();
    }

    private void emitStatus(String s) {
        Log.i(TAG, "[" + ip + ":" + port + "] " + s);
        if (cbs != null) cbs.onStatus(s);
    }

    private void markDisconnected(String reason) {
        boolean wasConn = connected.getAndSet(false);
        if (failureReason.isEmpty() && reason != null && !reason.isEmpty()) {
            failureReason = reason;
        }
        releaseLatches();
        if (wasConn) {
            if (reason != null && !reason.isEmpty()) emitStatus(reason);
            if (cbs != null) cbs.onStateChanged(false);
        }
    }

    private void releaseLatches() {
        if (legacyLatch != null) legacyLatch.countDown();
        if (versionLatch != null) versionLatch.countDown();
        if (safetyInfoLatch != null) safetyInfoLatch.countDown();
        if (safetyAuthLatch != null) safetyAuthLatch.countDown();
    }

    public boolean connect(Callbacks cbs, java.util.function.BooleanSupplier cancelled) {
        if (cancelled.getAsBoolean()) return false;
        disconnect();
        aborted.set(false);
        this.cbs = cbs;
        emitStatus("8899 TCP 建连中...");
        try {
            Socket s = new Socket();
            socket = s;
            if (aborted.get() || cancelled.getAsBoolean()) {
                try { s.close(); } catch (Exception ignored) {}
                return false;
            }
            s.setTcpNoDelay(true);
            s.bind(new InetSocketAddress(bindIp, 0));
            s.connect(new InetSocketAddress(ip, port), 2500);
            if (aborted.get() || cancelled.getAsBoolean()) {
                try { s.close(); } catch (Exception ignored) {}
                return false;
            }
            s.setSoTimeout(300);
            localIp = s.getLocalAddress().getHostAddress();
            localPort = s.getLocalPort();
            out = s.getOutputStream();

            legacyLatch = new CountDownLatch(1);
            versionLatch = new CountDownLatch(1);
            safetyInfoLatch = new CountDownLatch(1);
            safetyAuthLatch = new CountDownLatch(1);
            legacyAuthDone = false;
            versionAckReceived = false;
            safetyInfoOk = false;
            peerChallengeAcked = false;
            localChallengeVerified = false;
            failureReason = "";
            safetyActive.set(false);
            handshakeReady.set(false);
            encCipher = null;
            decCipher = null;

            running.set(true);
            connected.set(true);
            seq.set(1);
            if (cbs != null) cbs.onStateChanged(true);

            recvTh = new Thread(this::recvLoop, "MiPlay-CmdRecv-" + ip);
            recvTh.start();

            // 1. 等待 0x0028 挑战认证
            emitStatus("8899 等待 0x0028 挑战认证...");
            legacyLatch.await(2000, TimeUnit.MILLISECONDS);
            if (!connected.get()) {
                emitStatus(failureReason.isEmpty() ? "8899 挑战认证阶段连接断开" : failureReason);
                disconnect();
                return false;
            }

            sendGetVersion("3.1.6030516");
            if (!legacyAuthDone) {
                versionLatch.await(1000, TimeUnit.MILLISECONDS);
            }
            if (!connected.get() || (!legacyAuthDone && !versionAckReceived)) {
                emitStatus(failureReason.isEmpty()
                        ? "8899 接收端无应用层响应（请确认接收端在前台运行）" : failureReason);
                disconnect();
                return false;
            }

            // 2. 协商 Safety 安全通道
            emitStatus("8899 协商 Safety 安全通道...");
            String offer = "{\"aesIvTypes\":\"3\",\"aesKeyTypes\":\"1\",\"authAlgorithmTypes\":\"4\","
                    + "\"authKeyTypes\":\"1\",\"integrityTypes\":\"1\"}";
            sendCmd(CMD_SAFETY_INFO, encodeSafetyEnvelope(offer.getBytes(StandardCharsets.UTF_8), false), false);

            safetyInfoLatch.await(1500, TimeUnit.MILLISECONDS);
            if (!connected.get()) {
                emitStatus(failureReason.isEmpty() ? "8899 SafetyInfo 协商阶段连接断开" : failureReason);
                disconnect();
                return false;
            }

            if (safetyInfoOk) {
                emitStatus("8899 正在完成 Safety 双向认证...");
                safetyAuthLatch.await(2500, TimeUnit.MILLISECONDS);
                if (!connected.get() || !peerChallengeAcked || !localChallengeVerified) {
                    String err = !failureReason.isEmpty() ? failureReason
                            : (!connected.get() ? "8899 SafetyAuth 阶段被远端重置/断开 (RST)"
                            : (!peerChallengeAcked ? "8899 SafetyAuth 未能解密对端 0x1402"
                            : "8899 SafetyAuth 等待对端 0x1403 确认超时"));
                    emitStatus(err);
                    disconnect();
                    return false;
                }
                safetyActive.set(true);
                handshakeReady.set(true);
                emitStatus("8899 Safety 双向加密认证通过");
            } else {
                handshakeReady.set(true);
                emitStatus("8899 Legacy 认证通过（明文通道）");
            }

            if (aborted.get() || !connected.get()) {
                disconnect();
                return false;
            }
            hbTh = new Thread(this::heartbeatLoop, "MiPlay-CmdHB-" + ip);
            hbTh.start();
            return true;
        } catch (Exception e) {
            if (!aborted.get()) emitStatus("8899 连接失败: " + e.getMessage());
            disconnect();
            return false;
        }
    }

    public void closeGracefully() {
        if (connected.get()) {
            sendCloseDevice();
            Socket s = socket;
            if (s != null) {
                try { s.shutdownOutput(); } catch (Exception ignored) {}
            }
            try { Thread.sleep(40); } catch (InterruptedException ignored) {}
        }
        disconnect();
    }

    public void disconnect() {
        aborted.set(true);
        running.set(false);
        boolean wasConn = connected.getAndSet(false);
        safetyActive.set(false);
        handshakeReady.set(false);
        releaseLatches();
        Socket s = socket;
        socket = null;
        if (s != null) {
            try { s.close(); } catch (Exception ignored) {}
        }
        if (wasConn && cbs != null) cbs.onStateChanged(false);
    }

    public boolean sendGetVersion(String ver) {
        byte[] vb = ver.getBytes(StandardCharsets.UTF_8);
        byte[] p = new byte[vb.length + 1];
        System.arraycopy(vb, 0, p, 0, vb.length);
        return sendCmd(CMD_GET_VERSION, p, false);
    }

    public boolean sendOpenDevice(String senderIp, int rtspPort) {
        if (!handshakeReady.get()) return false;
        sendCmd(CMD_SET_LOCAL_DEV_INFO, "{\"sourceName\":\"MiPlay Android\"}".getBytes(StandardCharsets.UTF_8), true);
        sendCmd(CMD_SET_PLAY_SOURCE, "{\"playSource\":1}".getBytes(StandardCharsets.UTF_8), true);

        String url = "wfd://" + senderIp + ":" + rtspPort + "?mirrorMode=1";
        byte[] ub = url.getBytes(StandardCharsets.UTF_8);
        byte[] p = new byte[ub.length + 1];
        System.arraycopy(ub, 0, p, 0, ub.length);
        boolean ok = sendCmd(CMD_OPEN, p, true);
        if (ok) emitStatus("已发 Open(" + senderIp + ":" + rtspPort + ")，等待设备反连 RTSP...");
        return ok;
    }

    public boolean sendCloseDevice() {
        return sendCmd(CMD_CLOSE, new byte[0], true);
    }

    public boolean sendHeartbeat() {
        return sendCmd(CMD_HEARTBEAT, new byte[0], true);
    }

    public boolean sendSetVolume(int vol) {
        int v = Math.max(0, Math.min(100, vol));
        // 4 字节大端整数 (41% -> 00 00 00 29)，兼容 SoundFlow 1.1.2 与小米全系接收端
        byte[] payload = new byte[]{
                (byte) ((v >>> 24) & 0xFF),
                (byte) ((v >>> 16) & 0xFF),
                (byte) ((v >>> 8) & 0xFF),
                (byte) (v & 0xFF)
        };
        return sendCmd(CMD_SET_VOLUME, payload, true);
    }

    public boolean sendPause() {
        return sendCmd(CMD_PAUSE, new byte[0], true);
    }

    public boolean sendResume() {
        return sendCmd(CMD_RESUME, new byte[0], true);
    }

    public boolean sendSetPositionMs(long positionMs) {
        long p = Math.max(0L, positionMs);
        byte[] payload = new byte[8];
        for (int i = 0; i < 8; i++) {
            payload[i] = (byte) ((p >>> (8 * i)) & 0xFFL);
        }
        return sendCmd(CMD_SET_POSITION, payload, true);
    }

    public boolean sendSetMediaInfo(String title, String artist, String album, long durationMs) {
        MediaInfo info = new MediaInfo();
        info.title = title == null ? "" : title;
        info.artist = artist == null ? "" : artist;
        info.album = album == null ? "" : album;
        info.durationMs = Math.max(0L, durationMs);
        return sendSetMediaInfo(info);
    }

    public boolean sendSetMediaInfo(MediaInfo info) {
        try {
            String t = info.title == null ? "" : info.title;
            String a = info.artist == null ? "" : info.artist;
            String al = info.album == null ? "" : info.album;
            String lrc = (info.lyrics != null && !info.lyrics.isEmpty())
                    ? info.lyrics
                    : (info.lyricLine != null ? info.lyricLine : "");
            String line = info.lyricLine == null ? "" : info.lyricLine;
            String src = (info.sourceName == null || info.sourceName.isEmpty()) ? "MiPlay Android" : info.sourceName;

            JSONObject obj = new JSONObject();
            obj.put("title", t);
            obj.put("mTitle", t);
            obj.put("artist", a);
            obj.put("mArtist", a);
            obj.put("album", al);
            obj.put("mAlbum", al);
            obj.put("duration_ms", Math.max(0L, info.durationMs));
            obj.put("durationMs", Math.max(0L, info.durationMs));
            obj.put("mDuration", Math.max(0L, info.durationMs));
            obj.put("position_ms", Math.max(0L, info.positionMs));
            obj.put("positionMs", Math.max(0L, info.positionMs));
            obj.put("mPosition", Math.max(0L, info.positionMs));
            obj.put("sourceName", src);
            obj.put("source_name", src);
            obj.put("mMetaChangeType", String.valueOf(info.metaChangeType));
            obj.put("metaChangeType", String.valueOf(info.metaChangeType));
            obj.put("metadata_change_type", info.metaChangeType);
            if (!lrc.isEmpty()) {
                obj.put("mLrc", lrc);
                obj.put("lyrics", lrc);
                obj.put("lrc", lrc);
            }
            if (!line.isEmpty()) {
                obj.put("lyric_line", line);
                obj.put("lyricLine", line);
                obj.put("currentLyric", line);
            }
            return sendCmd(CMD_SET_MEDIA_INFO, obj.toString().getBytes(StandardCharsets.UTF_8), true);
        } catch (Exception e) {
            return false;
        }
    }

    public static byte[] encodeNotifyControlKey(String key) {
        byte[] k = key.getBytes(StandardCharsets.US_ASCII);
        byte[] out = new byte[k.length + 3];
        out[0] = (byte) k.length;
        System.arraycopy(k, 0, out, 1, k.length);
        out[k.length + 1] = 0x00;
        out[k.length + 2] = 0x01;
        return out;
    }

    public static byte[] encodeNotifySeekMs(long positionMs) {
        byte[] k = "key-seek".getBytes(StandardCharsets.US_ASCII);
        byte[] out = new byte[18];
        out[0] = 8;
        System.arraycopy(k, 0, out, 1, 8);
        out[9] = 0x09;
        for (int i = 0; i < 8; i++) {
            out[10 + i] = (byte) ((positionMs >>> ((7 - i) * 8)) & 0xFFL);
        }
        return out;
    }

    public static java.util.List<RemoteControlEvent> decodeRemoteControlEvents(int cmdType, byte[] payload) {
        java.util.List<RemoteControlEvent> out = new java.util.ArrayList<>();
        if (payload == null) payload = new byte[0];
        if (cmdType == CMD_NOTIFY) {
            parseNotifyTlvStream(payload, 0, payload.length, out);
            return out;
        }
        if (cmdType == CMD_PAUSE) {
            out.add(new RemoteControlEvent(RemoteControlAction.PAUSE, 0L, -1, "cmd-pause"));
            return out;
        }
        if (cmdType == CMD_RESUME) {
            out.add(new RemoteControlEvent(RemoteControlAction.RESUME, 0L, -1, "cmd-resume"));
            return out;
        }
        if (cmdType == CMD_SET_POSITION && payload.length >= 8) {
            long le = 0L, be = 0L;
            for (int i = 0; i < 8; i++) {
                le |= ((payload[i] & 0xFFL) << (8 * i));
                be = (be << 8) | (payload[i] & 0xFFL);
            }
            long maxMs = 86_400_000L;
            long pos = (le >= 0L && le <= maxMs) ? le : ((be >= 0L && be <= maxMs) ? be : le);
            out.add(new RemoteControlEvent(RemoteControlAction.SEEK, Math.max(0L, pos), -1, "cmd-setposition"));
            return out;
        }
        return out;
    }

    private static void parseNotifyTlvStream(byte[] data, int off, int end, java.util.List<RemoteControlEvent> out) {
        while (off + 2 <= end) {
            int klen = data[off++] & 0xFF;
            if (klen == 0 || off + klen + 1 > end) break;
            String key = new String(data, off, klen, StandardCharsets.UTF_8);
            off += klen;
            int vtype = data[off++] & 0xFF;

            long intVal = 0L;
            boolean hasInt = false;
            String strVal = "";

            if (vtype == 0x00 || vtype == 0x01 || vtype == 0x02 || vtype == 0x03) {
                if (off + 1 > end) break;
                intVal = data[off++] & 0xFFL;
                hasInt = true;
            } else if (vtype == 0x04 || vtype == 0x05 || vtype == 0x06 || vtype == 0x07) {
                if (off + 4 <= end) {
                    intVal = ((data[off] & 0xFFL) << 24) | ((data[off + 1] & 0xFFL) << 16)
                            | ((data[off + 2] & 0xFFL) << 8) | (data[off + 3] & 0xFFL);
                    off += 4;
                    hasInt = true;
                } else if (off + 2 <= end) {
                    intVal = ((data[off] & 0xFFL) << 8) | (data[off + 1] & 0xFFL);
                    off += 2;
                    hasInt = true;
                } else {
                    break;
                }
            } else if (vtype == 0x08 || vtype == 0x09) {
                if (off + 8 > end) break;
                for (int i = 0; i < 8; i++) intVal = (intVal << 8) | (data[off + i] & 0xFFL);
                off += 8;
                hasInt = true;
            } else if (vtype == 0x14 || vtype == 0x1E) {
                if (off + 4 > end) break;
                int vlen = ((data[off] & 0xFF) << 24) | ((data[off + 1] & 0xFF) << 16)
                        | ((data[off + 2] & 0xFF) << 8) | (data[off + 3] & 0xFF);
                off += 4;
                if (vlen < 0 || off + vlen > end) break;
                strVal = new String(data, off, vlen, StandardCharsets.UTF_8);
                off += vlen;
            } else if (vtype == 0x16) {
                if (off + 4 > end) break;
                int vlen = ((data[off] & 0xFF) << 24) | ((data[off + 1] & 0xFF) << 16)
                        | ((data[off + 2] & 0xFF) << 8) | (data[off + 3] & 0xFF);
                off += 4;
                if (vlen < 0 || off + vlen > end) break;
                parseNotifyTlvStream(data, off, off + vlen, out);
                off += vlen;
                continue;
            } else {
                break;
            }

            String k = key.toLowerCase(java.util.Locale.US);
            if ("key-pause".equals(k) || "pause".equals(k)) {
                out.add(new RemoteControlEvent(RemoteControlAction.PAUSE, 0L, -1, key));
            } else if ("key-resume".equals(k) || "key-play".equals(k) || "resume".equals(k) || "play".equals(k)) {
                out.add(new RemoteControlEvent(RemoteControlAction.RESUME, 0L, -1, key));
            } else if ("key-next".equals(k) || "next".equals(k)) {
                out.add(new RemoteControlEvent(RemoteControlAction.NEXT, 0L, -1, key));
            } else if ("key-prev".equals(k) || "key-previous".equals(k) || "prev".equals(k) || "previous".equals(k)) {
                out.add(new RemoteControlEvent(RemoteControlAction.PREVIOUS, 0L, -1, key));
            } else if ("key-seek".equals(k) || "seek".equals(k)) {
                long pos = 0L;
                if (hasInt) pos = Math.max(0L, intVal);
                else if (!strVal.isEmpty()) {
                    try { pos = Math.max(0L, Long.parseLong(strVal.trim())); } catch (NumberFormatException ignored) {}
                }
                out.add(new RemoteControlEvent(RemoteControlAction.SEEK, pos, -1, key));
            } else if ("volume".equals(k)) {
                int vol = -1;
                if (hasInt && intVal >= 0L && intVal <= 100L) vol = (int) intVal;
                else if (!strVal.isEmpty()) {
                    try {
                        int v = Integer.parseInt(strVal.trim());
                        if (v >= 0 && v <= 100) vol = v;
                    } catch (NumberFormatException ignored) {}
                }
                if (vol >= 0) out.add(new RemoteControlEvent(RemoteControlAction.SET_VOLUME, 0L, vol, key));
            }
        }
    }

    private boolean sendRawLocked(int cmdType, int seqNum, byte[] payload) {
        if (!connected.get() || out == null) return false;
        try {
            int plen = payload.length;
            byte[] frame = new byte[9 + plen];
            frame[0] = 0x24;
            frame[1] = (byte) ((cmdType >>> 8) & 0xFF);
            frame[2] = (byte) (cmdType & 0xFF);
            frame[3] = (byte) ((seqNum >>> 8) & 0xFF);
            frame[4] = (byte) (seqNum & 0xFF);
            frame[5] = (byte) ((plen >>> 24) & 0xFF);
            frame[6] = (byte) ((plen >>> 16) & 0xFF);
            frame[7] = (byte) ((plen >>> 8) & 0xFF);
            frame[8] = (byte) (plen & 0xFF);
            if (plen > 0) System.arraycopy(payload, 0, frame, 9, plen);
            out.write(frame);
            out.flush();
            return true;
        } catch (Exception e) {
            markDisconnected("8899 发送失败: " + e.getMessage());
            return false;
        }
    }

    private boolean sendCmd(int cmdType, byte[] rawPayload, boolean allowEncrypt) {
        if (!connected.get()) return false;
        synchronized (sendLock) {
            try {
                int s = seq.getAndIncrement() & 0xFFFF;
                byte[] payload = rawPayload;
                if (allowEncrypt && safetyActive.get() && encCipher != null) {
                    payload = encCipher.encryptBlob(rawPayload);
                }
                return sendRawLocked(cmdType, s, payload);
            } catch (Exception e) {
                markDisconnected("8899 加密发送失败: " + e.getMessage());
                return false;
            }
        }
    }

    private void handleIncomingFrame(int cmd, int frameSeq, byte[] payload) {
        try {
            if (cmd == CMD_CHALLENGE) {
                if (handshakeReady.get()) return;
                byte[] resp = computeLegacyChallengeResponse(payload);
                synchronized (sendLock) {
                    sendRawLocked(CMD_CHALLENGE_ACK, frameSeq, resp);
                }
                legacyAuthDone = true;
                if (legacyLatch != null) legacyLatch.countDown();
                return;
            }

            if (cmd == CMD_GET_VERSION_ACK) {
                versionAckReceived = true;
                if (versionLatch != null) versionLatch.countDown();
                if (cbs != null) {
                    String v = new String(payload, StandardCharsets.UTF_8).replace("\0", "");
                    cbs.onVersion(v);
                }
                return;
            }

            if (cmd == CMD_SAFETY_INFO_ACK) {
                if (handshakeReady.get()) return;
                String jsonStr = decodeSafetyEnvelope(payload);
                if (jsonStr == null) {
                    safetyInfoOk = false;
                    if (safetyInfoLatch != null) safetyInfoLatch.countDown();
                    return;
                }
                JSONObject obj = new JSONObject(jsonStr);
                if (!"0".equals(obj.optString("result", ""))) {
                    safetyInfoOk = false;
                    if (safetyInfoLatch != null) safetyInfoLatch.countDown();
                    return;
                }
                authKey = deriveType1AuthKey(ip, port, localIp, localPort);
                byte[] keyBytes = authKey.getBytes(StandardCharsets.US_ASCII);
                byte[] key16 = Arrays.copyOfRange(keyBytes, 0, 16);
                byte[] ivT1 = Arrays.copyOfRange(keyBytes, 0, 16);
                byte[] ivT2 = Arrays.copyOfRange(keyBytes, 16, 32);

                // 真机实证：即使 0x1401 协商 aesIvType="2"，小米原生/SoundFlow 均以 ivT1 (authKey[0:16]) 为初始 CBC IV
                synchronized (sendLock) {
                    encCipher = new SafetyCipher(key16, ivT1);
                    decCandType1 = new SafetyCipher(key16, ivT1);
                    decCandType2 = new SafetyCipher(key16, ivT2);
                    decCipher = null;

                    String localMsg = md5Hex(String.valueOf(TimerService.monotonicNowUs()));
                    String chJson = "{\"authMsg\":\"" + localMsg + "\"}";
                    byte[] env = encodeSafetyEnvelope(chJson.getBytes(StandardCharsets.UTF_8), false);
                    byte[] blob = encCipher.encryptBlob(env);
                    sendRawLocked(CMD_SAFETY_AUTH, seq.getAndIncrement() & 0xFFFF, blob);
                }
                safetyInfoOk = true;
                if (safetyInfoLatch != null) safetyInfoLatch.countDown();
                return;
            }

            if (cmd == CMD_SAFETY_AUTH || cmd == CMD_SAFETY_AUTH_ACK) {
                String jsonStr = null;
                if (decCipher != null) {
                    byte[] p = decCipher.decryptBlob(payload);
                    if (p != null) jsonStr = decodeSafetyEnvelope(p);
                } else if (decCandType1 != null && decCandType2 != null) {
                    SafetyCipher c1 = decCandType1.copy();
                    byte[] p1 = c1.decryptBlob(payload);
                    String j1 = (p1 != null) ? decodeSafetyEnvelope(p1) : null;
                    if (j1 != null) {
                        decCipher = c1;
                        jsonStr = j1;
                    } else {
                        SafetyCipher c2 = decCandType2.copy();
                        byte[] p2 = c2.decryptBlob(payload);
                        String j2 = (p2 != null) ? decodeSafetyEnvelope(p2) : null;
                        if (j2 != null) {
                            decCipher = c2;
                            jsonStr = j2;
                        }
                    }
                }
                if (jsonStr == null) {
                    if (failureReason.isEmpty()) failureReason = "8899 SafetyAuth 解密或 TLV 解析失败";
                    return;
                }
                JSONObject obj = new JSONObject(jsonStr);
                if (cmd == CMD_SAFETY_AUTH) {
                    String peerMsg = obj.optString("authMsg", "");
                    if (!peerMsg.isEmpty()) {
                        String digest = hmacSha256Hex(authKey.getBytes(StandardCharsets.US_ASCII),
                                peerMsg.getBytes(StandardCharsets.UTF_8));
                        String ackJson = "{\"result\":\"0\",\"authMsgAck\":\"" + digest + "\"}";
                        byte[] env = encodeSafetyEnvelope(ackJson.getBytes(StandardCharsets.UTF_8), true);
                        synchronized (sendLock) {
                            byte[] blob = encCipher.encryptBlob(env);
                            sendRawLocked(CMD_SAFETY_AUTH_ACK, frameSeq, blob);
                        }
                        peerChallengeAcked = true;
                        if (peerChallengeAcked && localChallengeVerified && safetyAuthLatch != null) {
                            safetyAuthLatch.countDown();
                        }
                    }
                } else {
                    String res = obj.optString("result", "");
                    if ("0".equals(res) || "1".equals(res)) {
                        localChallengeVerified = true;
                        if (peerChallengeAcked && localChallengeVerified && safetyAuthLatch != null) {
                            safetyAuthLatch.countDown();
                        }
                    }
                }
                return;
            }

            // 业务阶段入向帧：先按状态化 Safety CBC 解密（避免遗漏解密导致后续 CBC IV 偏移）
            byte[] plain = payload;
            if (safetyActive.get() && decCipher != null && payload != null && payload.length >= 25) {
                byte[] dec = decCipher.decryptBlob(payload);
                if (dec != null) plain = dec;
            }

            if (cmd == CMD_PAUSE || cmd == CMD_RESUME || cmd == CMD_SET_POSITION) {
                int ackCmd = cmd + 1;
                synchronized (sendLock) {
                    byte[] ackPayload = new byte[0];
                    if (safetyActive.get() && encCipher != null) {
                        ackPayload = encCipher.encryptBlob(ackPayload);
                    }
                    sendRawLocked(ackCmd, frameSeq, ackPayload);
                }
            }

            if (cbs != null) {
                for (RemoteControlEvent ev : decodeRemoteControlEvents(cmd, plain)) {
                    cbs.onRemoteControl(ev);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Frame handle error: " + e.getMessage());
        }
    }

    private void recvLoop() {
        ByteArrayOutputStream acc = new ByteArrayOutputStream();
        byte[] tmp = new byte[4096];
        try {
            InputStream in = socket.getInputStream();
            while (running.get()) {
                try {
                    int n = in.read(tmp);
                    if (n < 0) {
                        markDisconnected("8899 连接被远端关闭 (FIN)");
                        break;
                    }
                    if (n == 0) continue;
                    acc.write(tmp, 0, n);
                    byte[] data = acc.toByteArray();
                    int off = 0;
                    while (off + 9 <= data.length) {
                        if (data[off] != 0x24) { off++; continue; }
                        int cmd = ((data[off + 1] & 0xFF) << 8) | (data[off + 2] & 0xFF);
                        int fseq = ((data[off + 3] & 0xFF) << 8) | (data[off + 4] & 0xFF);
                        long plen = ((long) (data[off + 5] & 0xFF) << 24)
                                | ((long) (data[off + 6] & 0xFF) << 16)
                                | ((long) (data[off + 7] & 0xFF) << 8)
                                | (data[off + 8] & 0xFF);
                        if (plen > 4 * 1024 * 1024L) { off++; continue; }
                        if (off + 9 + plen > data.length) break;
                        byte[] payload = Arrays.copyOfRange(data, off + 9, off + 9 + (int) plen);
                        handleIncomingFrame(cmd, fseq, payload);
                        off += 9 + (int) plen;
                    }
                    acc.reset();
                    if (off < data.length) acc.write(data, off, data.length - off);
                } catch (java.net.SocketTimeoutException ignored) {
                }
            }
        } catch (Exception e) {
            if (running.get()) {
                markDisconnected("8899 连接被远端重置/断开: " + e.getMessage());
            }
        }
    }

    private void heartbeatLoop() {
        int tick = 0;
        while (running.get() && connected.get()) {
            if (tick > 0 && tick % 50 == 0) {
                if (!sendHeartbeat()) break;
            }
            tick++;
            try { Thread.sleep(100); } catch (InterruptedException ignored) { break; }
        }
    }

    private static byte[] encodeSafetyEnvelope(byte[] payload, boolean isAck) {
        byte[] tag = (isAck ? "ack" : "cmd").getBytes(StandardCharsets.US_ASCII);
        byte[] out = new byte[1 + tag.length + 1 + 4 + payload.length];
        out[0] = (byte) tag.length;
        System.arraycopy(tag, 0, out, 1, tag.length);
        int o = 1 + tag.length;
        out[o++] = 0x1E;
        int n = payload.length;
        out[o++] = (byte) ((n >>> 24) & 0xFF);
        out[o++] = (byte) ((n >>> 16) & 0xFF);
        out[o++] = (byte) ((n >>> 8) & 0xFF);
        out[o++] = (byte) (n & 0xFF);
        System.arraycopy(payload, 0, out, o, n);
        return out;
    }

    private static String decodeSafetyEnvelope(byte[] data) {
        if (data == null || data.length < 9) return null;
        int tlen = data[0] & 0xFF;
        int hlen = 1 + tlen + 1 + 4;
        if (data.length < hlen || data[1 + tlen] != 0x1E) return null;
        int plen = ((data[2 + tlen] & 0xFF) << 24)
                | ((data[3 + tlen] & 0xFF) << 16)
                | ((data[4 + tlen] & 0xFF) << 8)
                | (data[5 + tlen] & 0xFF);
        if (plen < 0 || hlen + plen > data.length) return null;
        return new String(data, hlen, plen, StandardCharsets.UTF_8);
    }

    private static byte[] computeLegacyChallengeResponse(byte[] challenge) throws Exception {
        String keyHex = md5Hex("0.0.0.0");
        Mac mac = Mac.getInstance("HmacSHA1");
        mac.init(new SecretKeySpec(keyHex.getBytes(StandardCharsets.US_ASCII), "HmacSHA1"));
        return bytesToHex(mac.doFinal(challenge)).getBytes(StandardCharsets.US_ASCII);
    }

    private static String deriveType1AuthKey(String serverIp, int serverPort, String clientIp, int clientPort) throws Exception {
        String raw = serverIp + serverPort + clientIp + clientPort;
        StringBuilder sb = new StringBuilder(raw.length());
        for (int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            if (c >= '0' && c <= '9') sb.append((char) ('a' + (c - '0')));
            else sb.append(c);
        }
        return md5Hex(sb.toString());
    }

    private static String md5Hex(String s) throws Exception {
        MessageDigest md = MessageDigest.getInstance("MD5");
        return bytesToHex(md.digest(s.getBytes(StandardCharsets.UTF_8)));
    }

    private static String hmacSha256Hex(byte[] key, byte[] data) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return bytesToHex(mac.doFinal(data));
    }

    private static String bytesToHex(byte[] b) {
        char[] hex = "0123456789abcdef".toCharArray();
        char[] out = new char[b.length * 2];
        for (int i = 0; i < b.length; i++) {
            int v = b[i] & 0xFF;
            out[i * 2] = hex[v >>> 4];
            out[i * 2 + 1] = hex[v & 0x0F];
        }
        return new String(out);
    }

    private static int crc32Mpeg2(byte[] data) {
        int crc = 0xFFFFFFFF;
        for (byte value : data) {
            crc ^= (value & 0xFF) << 24;
            for (int b = 0; b < 8; b++) {
                crc = ((crc & 0x80000000) != 0) ? ((crc << 1) ^ 0x04C11DB7) : (crc << 1);
            }
        }
        return crc;
    }
}
