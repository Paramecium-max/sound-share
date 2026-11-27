package io.soundflow.sender;

import android.content.Context;
import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.math.BigInteger;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.Arrays;

import javax.crypto.Cipher;

/**
 * Pure-Java loopback ADB transport client (127.0.0.1:<port>) for non-root UID 2000
 * AudioPolicy daemon bootstrapping without requiring Shizuku or Magisk.
 */
public final class WirelessAdbClient implements AutoCloseable {
    private static final int A_CNXN = 0x4e584e43;
    private static final int A_AUTH = 0x48545541;
    private static final int A_OPEN = 0x4e45504f;
    private static final int A_OKAY = 0x59414b4f;
    private static final int A_CLSE = 0x45534c43;
    private static final int A_WRTE = 0x45545257;
    private static final int A_STLS = 0x534c5453;

    private static final int A_VERSION = 0x01000001;
    private static final int A_MAXDATA = 256 * 1024;
    private static final int AUTH_TOKEN = 1;
    private static final int AUTH_SIGNATURE = 2;
    private static final int AUTH_RSAPUBLICKEY = 3;

    private static final byte[] SHA1_DIGEST_INFO_PREFIX = new byte[]{
            0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
            0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
    };

    private static final class AdbMessage {
        final int command;
        final int arg0;
        final int arg1;
        final byte[] payload;

        AdbMessage(int command, int arg0, int arg1, byte[] payload) {
            this.command = command;
            this.arg0 = arg0;
            this.arg1 = arg1;
            this.payload = payload;
        }
    }

    private final Socket socket;
    private final InputStream in;
    private final OutputStream out;
    private int nextLocalId = 1;

    private WirelessAdbClient(Socket socket) throws IOException {
        this.socket = socket;
        this.in = socket.getInputStream();
        this.out = socket.getOutputStream();
    }

    public static int detectTcpPort() {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method get = sp.getMethod("get", String.class, String.class);
            String val = (String) get.invoke(null, "service.adb.tcp.port", "");
            if (val != null) {
                int port = Integer.parseInt(val.trim());
                if (port > 0 && port <= 65535) return port;
            }
        } catch (Throwable ignored) {}
        return 5555;
    }

    public static WirelessAdbClient connect(Context ctx, String host, int port) throws Exception {
        KeyPair keyPair = loadOrCreateKeyPair(ctx);
        Socket sock = new Socket();
        try {
            sock.connect(new InetSocketAddress(host, port), 3500);
            sock.setTcpNoDelay(true);
            sock.setSoTimeout(35000);
            WirelessAdbClient client = new WirelessAdbClient(sock);
            client.handshake(keyPair);
            return client;
        } catch (Throwable t) {
            try { sock.close(); } catch (Throwable ignored) {}
            if (t instanceof Exception) throw (Exception) t;
            throw new IOException(t);
        }
    }

    private void handshake(KeyPair keyPair) throws Exception {
        sendMessage(A_CNXN, A_VERSION, A_MAXDATA, "host::soundflow\0".getBytes(StandardCharsets.UTF_8));
        boolean sentSignature = false;
        boolean sentPublicKey = false;
        while (true) {
            AdbMessage msg = readMessage();
            if (msg.command == A_CNXN) {
                return;
            }
            if (msg.command == A_STLS) {
                throw new IOException("目标端口需要 Android 无线调试 TLS 配对；请先在电脑或本机执行 adb tcpip 5555 开启 5555 端口");
            }
            if (msg.command == A_AUTH && msg.arg0 == AUTH_TOKEN) {
                if (!sentSignature) {
                    sentSignature = true;
                    byte[] sig = signToken(keyPair.getPrivate(), msg.payload);
                    sendMessage(A_AUTH, AUTH_SIGNATURE, 0, sig);
                } else if (!sentPublicKey) {
                    sentPublicKey = true;
                    byte[] pub = encodeAndroidAdbPublicKey((RSAPublicKey) keyPair.getPublic());
                    sendMessage(A_AUTH, AUTH_RSAPUBLICKEY, 0, pub);
                } else {
                    throw new IOException("ADB 调试授权被拒绝，请在系统弹窗点击「允许」并勾选一律允许");
                }
                continue;
            }
            throw new IOException("ADB 握手收到非预期帧: 0x" + Integer.toHexString(msg.command));
        }
    }

    public String execShell(String command, int timeoutMs) throws IOException {
        socket.setSoTimeout(Math.max(1000, timeoutMs));
        int localId = nextLocalId++;
        byte[] dest = ("shell:" + command + "\0").getBytes(StandardCharsets.UTF_8);
        sendMessage(A_OPEN, localId, 0, dest);

        int remoteId = -1;
        ByteArrayOutputStream outBuf = new ByteArrayOutputStream();
        while (true) {
            AdbMessage msg = readMessage();
            if (msg.command == A_OKAY && msg.arg1 == localId) {
                if (remoteId < 0) remoteId = msg.arg0;
            } else if (msg.command == A_WRTE && msg.arg1 == localId) {
                if (remoteId < 0) remoteId = msg.arg0;
                if (msg.payload.length > 0) outBuf.write(msg.payload);
                sendMessage(A_OKAY, localId, remoteId, new byte[0]);
            } else if (msg.command == A_CLSE && msg.arg1 == localId) {
                if (remoteId >= 0) {
                    try { sendMessage(A_CLSE, localId, remoteId, new byte[0]); } catch (IOException ignored) {}
                }
                break;
            }
        }
        return outBuf.toString(StandardCharsets.UTF_8.name());
    }

    private void sendMessage(int command, int arg0, int arg1, byte[] payload) throws IOException {
        byte[] data = payload != null ? payload : new byte[0];
        int checksum = 0;
        for (byte b : data) checksum += (b & 0xFF);
        ByteBuffer hdr = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        hdr.putInt(command);
        hdr.putInt(arg0);
        hdr.putInt(arg1);
        hdr.putInt(data.length);
        hdr.putInt(checksum);
        hdr.putInt(command ^ 0xFFFFFFFF);
        out.write(hdr.array());
        if (data.length > 0) out.write(data);
        out.flush();
    }

    private AdbMessage readMessage() throws IOException {
        byte[] hdrBytes = new byte[24];
        readFully(hdrBytes, 24);
        ByteBuffer hdr = ByteBuffer.wrap(hdrBytes).order(ByteOrder.LITTLE_ENDIAN);
        int command = hdr.getInt();
        int arg0 = hdr.getInt();
        int arg1 = hdr.getInt();
        int len = hdr.getInt();
        hdr.getInt(); // checksum
        int magic = hdr.getInt();
        if (magic != (command ^ 0xFFFFFFFF) || len < 0 || len > 1024 * 1024) {
            throw new IOException("ADB 协议帧头校验失败");
        }
        byte[] payload = new byte[len];
        if (len > 0) readFully(payload, len);
        return new AdbMessage(command, arg0, arg1, payload);
    }

    private void readFully(byte[] dst, int len) throws IOException {
        int off = 0;
        while (off < len) {
            int n = in.read(dst, off, len - off);
            if (n < 0) throw new EOFException("ADB 连接已断开");
            off += n;
        }
    }

    private static byte[] signToken(PrivateKey privateKey, byte[] token) throws Exception {
        byte[] digestInfo = new byte[SHA1_DIGEST_INFO_PREFIX.length + token.length];
        System.arraycopy(SHA1_DIGEST_INFO_PREFIX, 0, digestInfo, 0, SHA1_DIGEST_INFO_PREFIX.length);
        System.arraycopy(token, 0, digestInfo, SHA1_DIGEST_INFO_PREFIX.length, token.length);
        Cipher cipher = Cipher.getInstance("RSA/ECB/PKCS1Padding");
        cipher.init(Cipher.ENCRYPT_MODE, privateKey);
        return cipher.doFinal(digestInfo);
    }

    private static byte[] encodeAndroidAdbPublicKey(RSAPublicKey pub) {
        final int keyWords = 64; // 2048-bit RSA = 64 uint32 words
        BigInteger n = pub.getModulus();
        BigInteger r32 = BigInteger.ONE.shiftLeft(32);
        BigInteger r = BigInteger.ONE.shiftLeft(keyWords * 32);
        BigInteger rr = r.modPow(BigInteger.valueOf(2), n);
        int n0inv = r32.subtract(n.mod(r32).modInverse(r32)).intValue();

        ByteBuffer buf = ByteBuffer.allocate(524).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(keyWords);
        buf.putInt(n0inv);
        writeBigIntLeWords(buf, n, keyWords);
        writeBigIntLeWords(buf, rr, keyWords);
        buf.putInt(pub.getPublicExponent().intValue());

        String b64 = Base64.encodeToString(buf.array(), Base64.NO_WRAP);
        return (b64 + " soundflow@local\0").getBytes(StandardCharsets.US_ASCII);
    }

    private static void writeBigIntLeWords(ByteBuffer buf, BigInteger val, int words) {
        byte[] be = val.toByteArray();
        int len = words * 4;
        byte[] padded = new byte[len];
        if (be.length >= len) {
            System.arraycopy(be, be.length - len, padded, 0, len);
        } else {
            System.arraycopy(be, 0, padded, len - be.length, be.length);
        }
        // Reverse entire big-endian byte array into little-endian uint32 words
        for (int i = len - 1; i >= 0; i -= 4) {
            int word = ((padded[i] & 0xFF))
                    | ((padded[i - 1] & 0xFF) << 8)
                    | ((padded[i - 2] & 0xFF) << 16)
                    | ((padded[i - 3] & 0xFF) << 24);
            buf.putInt(word);
        }
    }

    private static synchronized KeyPair loadOrCreateKeyPair(Context ctx) throws Exception {
        File dir = new File(ctx.getFilesDir(), "adb_keys");
        if (!dir.exists()) dir.mkdirs();
        File privFile = new File(dir, "adbkey.pk8");
        File pubFile = new File(dir, "adbkey.x509");
        if (privFile.isFile() && pubFile.isFile()) {
            try {
                KeyFactory kf = KeyFactory.getInstance("RSA");
                PrivateKey priv = kf.generatePrivate(new PKCS8EncodedKeySpec(readFile(privFile)));
                RSAPublicKey pub = (RSAPublicKey) kf.generatePublic(new X509EncodedKeySpec(readFile(pubFile)));
                return new KeyPair(pub, priv);
            } catch (Throwable ignored) {}
        }
        KeyPairGenerator kpg = KeyPairGenerator.getInstance("RSA");
        kpg.initialize(2048);
        KeyPair kp = kpg.generateKeyPair();
        writeFile(privFile, kp.getPrivate().getEncoded());
        writeFile(pubFile, kp.getPublic().getEncoded());
        return kp;
    }

    private static byte[] readFile(File f) throws IOException {
        try (FileInputStream fis = new FileInputStream(f)) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[2048];
            int n;
            while ((n = fis.read(buf)) > 0) bos.write(buf, 0, n);
            return bos.toByteArray();
        }
    }

    private static void writeFile(File f, byte[] data) throws IOException {
        try (FileOutputStream fos = new FileOutputStream(f)) {
            fos.write(data);
        }
    }

    @Override
    public void close() {
        try { socket.close(); } catch (IOException ignored) {}
    }
}
