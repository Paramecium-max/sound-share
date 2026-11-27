package io.soundflow.receiver;

import android.content.Context;
import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.MediaDataSource;
import android.media.MediaPlayer;
import android.media.audiofx.Visualizer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Lightweight standalone UPnP/DLNA MediaRenderer (SSDP + HTTP/SOAP + MediaPlayer).
 * Allows non-Xiaomi phones, PCs, and standard music apps to cast LAN audio streams.
 */
public final class DlnaRendererServer {
    public interface Listener {
        void onDlnaPreparing(long session, String title, String artist, String album);
        void onDlnaState(long session, boolean playing, String transportState, String title, String artist, String album, long durationMs, long positionMs);
        void onDlnaVolume(int percent);
        void onDlnaError(String msg);
        void onDlnaLog(String msg);
    }

    private static final String TAG = "SoundFlowDLNA";
    private static final String SSDP_ADDR = "239.255.255.250";
    private static final int SSDP_PORT = 1900;
    private static final long VISUALIZER_OUTPUT_ESTIMATE_NANOS = 60_000_000L;
    private static final int HTTP_REQ_DEADLINE_MS = 4000;
    private static final int MEDIA_CONNECT_TIMEOUT_MS = 4000;
    private static final int MEDIA_READ_TIMEOUT_MS = 5000;
    private static final int MAX_REDIRECTS = 3;
    public static final int HTTP_PORT = 8901;

    private final Context context;
    private final Listener listener;
    private final String udn;
    private final String friendlyName;
    private final String localIp;
    private final String ifaceName;
    private static final AtomicLong NEXT_SESSION = new AtomicLong();
    private final HandlerThread playerThread = new HandlerThread("soundflow-dlna-player");
    private final Handler playerHandler;
    private volatile long session;
    private volatile long sampledDuration;
    private volatile long sampledPosition;
    private volatile SeekRequest pendingSeek;

    private static final class SeekRequest {
        final long session;
        final long targetMs;
        final long requestedAt;

        SeekRequest(long session, long targetMs) {
            this.session = session;
            this.targetMs = targetMs;
            this.requestedAt = SystemClock.elapsedRealtime();
        }
    }

    private static final class PrepJob {
        final long session;
        final String uri;

        PrepJob(long session, String uri) {
            this.session = session;
            this.uri = uri;
        }
    }

    private volatile long sampledAt;
    private final ExecutorService listeners = Executors.newFixedThreadPool(2);
    private final ExecutorService httpWorkers = new ThreadPoolExecutor(
            2, 4, 30L, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(32),
            new ThreadPoolExecutor.AbortPolicy()
    );
    // Bounded single-slot latest-wins media preparation worker so DNS/HTTP probing never blocks playerHandler.
    private final AtomicReference<PrepJob> latestPrepJob = new AtomicReference<>();
    private final ExecutorService prepExecutor = new ThreadPoolExecutor(
            1, 1, 0L, TimeUnit.SECONDS, new ArrayBlockingQueue<>(1),
            task -> daemonThread(task, "soundflow-dlna-prep"),
            new ThreadPoolExecutor.DiscardOldestPolicy());
    private static final ExecutorService DNS_WORKERS = new ThreadPoolExecutor(
            0, 2, 20L, TimeUnit.SECONDS, new SynchronousQueue<>(),
            task -> daemonThread(task, "soundflow-dlna-dns"));

    private static Thread daemonThread(Runnable task, String name) {
        Thread thread = new Thread(task, name);
        thread.setDaemon(true);
        return thread;
    }

    private static InetAddress[] resolveMediaHost(String host) throws IOException {
        final Future<InetAddress[]> lookup;
        try {
            lookup = DNS_WORKERS.submit(() -> InetAddress.getAllByName(host));
        } catch (RejectedExecutionException e) {
            throw new IOException("Media resolver busy", e);
        }
        try {
            return lookup.get(MEDIA_CONNECT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            throw new SocketTimeoutException("Media DNS deadline exceeded");
        } catch (ExecutionException e) {
            throw new IOException("Unable to resolve media host", e.getCause());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Media resolution interrupted", e);
        } finally {
            lookup.cancel(true);
        }
    }
    private final AtomicReference<PinnedHttpMediaDataSource> activeDataSource = new AtomicReference<>();

    private volatile boolean running = false;
    private boolean closed;
    private final Set<Socket> openClients = ConcurrentHashMap.newKeySet();
    private volatile boolean playing = false;
    private volatile boolean playWhenPrepared;
    private volatile String currentUri = "";
    private volatile String title = "";
    private volatile String artist = "";
    private volatile String album = "";
    private volatile int volumePercent = 50;

    private ServerSocket httpServer;
    private MulticastSocket ssdpSocket;
    private MediaPlayer player;
    private volatile boolean mediaPrepared = false;
    private volatile int audioSessionId;
    private final Object visualizerLock = new Object();
    private Visualizer visualizer;
    private boolean visualizerUnavailable;
    private final byte[] fft = new byte[1024];

    private int onsetSampleRate = 48000;

    /** Lazily attaches Visualizer to this player's output session only; never opens the microphone. */
    private Visualizer ensureVisualizerLocked() {
        if (!playing || audioSessionId <= 0
                || context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            return null;
        }
        if (visualizer == null && !visualizerUnavailable) {
            visualizer = new Visualizer(audioSessionId);
            visualizer.setCaptureSize(fft.length);
            visualizer.setScalingMode(Visualizer.SCALING_MODE_AS_PLAYED);
            visualizer.setEnabled(true);
            onsetSampleRate = Math.max(8000, visualizer.getSamplingRate() / 1000);
            listener.onDlnaLog("DLNA 实频谱已启用，音频会话 " + audioSessionId);
        }
        return visualizer;
    }

    public static final class Spectrum {
        public final float[] bins = new float[512];
        public int sampleRate;
        public int packed;
        public long capturedNanos;
    }

    public boolean captureSpectrum(Spectrum result, boolean needOnsets, boolean needMeter) {
        synchronized (visualizerLock) {
            try {
                Visualizer v = ensureVisualizerLocked();
                if (v == null || v.getFft(fft) != Visualizer.SUCCESS) return false;
                result.capturedNanos = System.nanoTime() + VISUALIZER_OUTPUT_ESTIMATE_NANOS;
                result.sampleRate = onsetSampleRate;
                result.bins[0] = Math.abs(fft[0]) / 128f;
                int low = 0, mid = 0, high = 0;
                int lc = 0, mc = 0, hc = 0;
                double hzPerBin = (double) onsetSampleRate / fft.length;
                for (int k = 1; k < fft.length / 2; k++) {
                    int re = fft[2 * k], im = fft[2 * k + 1];
                    int power = re * re + im * im;
                    if (needOnsets) result.bins[k] = (float) Math.sqrt(power) / 128f;
                    if (needMeter) {
                        double hz = k * hzPerBin;
                        if (hz < 250) { low += power; lc++; }
                        else if (hz < 2000) { mid += power; mc++; }
                        else { high += power; hc++; }
                    }
                }
                if (needMeter) {
                    int b = Math.min(255, (int) (Math.sqrt((double) low / Math.max(1, lc)) * 12));
                    int m = Math.min(255, (int) (Math.sqrt((double) mid / Math.max(1, mc)) * 18));
                    int h = Math.min(255, (int) (Math.sqrt((double) high / Math.max(1, hc)) * 24));
                    result.packed = (Math.max(b, Math.max(m, h)) << 24) | (b << 16) | (m << 8) | h;
                }
                return true;
            } catch (RuntimeException e) {
                releaseVisualizerLocked();
                visualizerUnavailable = true;
                listener.onDlnaLog("DLNA 频谱不可用，音频播放继续: " + e.getMessage());
                return false;
            }
        }
    }

    private void releaseVisualizerLocked() {
        if (visualizer != null) {
            try { visualizer.release(); } catch (RuntimeException ignored) {}
            visualizer = null;
        }
    }

    public void releaseVisualizer() {
        synchronized (visualizerLock) {
            releaseVisualizerLocked();
        }
    }

    private void cancelActiveDataSource() {
        latestPrepJob.set(null);
        PinnedHttpMediaDataSource ds = activeDataSource.getAndSet(null);
        if (ds != null) {
            ds.cancelNow();
        }
    }

    private synchronized long invalidatePlayback(boolean autoPlay) {
        session = NEXT_SESSION.incrementAndGet();
        playWhenPrepared = autoPlay;
        cancelActiveDataSource();
        return session;
    }

    public void stopPlayback() {
        long retired = invalidatePlayback(false);
        playerHandler.post(() -> {
            if (session == retired) releasePlayer();
        });
    }

    public DlnaRendererServer(Context context, String udnSeed, String friendlyName, String localIp, String ifaceName, int volumePercent, Listener listener) {
        this.context = context;
        this.udn = "uuid:" + UUID.nameUUIDFromBytes(("soundflow-dlna:" + udnSeed).getBytes(StandardCharsets.UTF_8));
        this.friendlyName = friendlyName;
        this.localIp = localIp;
        this.ifaceName = ifaceName;
        this.volumePercent = Math.max(0, Math.min(100, volumePercent));
        this.listener = listener;
        playerThread.start();
        playerHandler = new Handler(playerThread.getLooper());
    }

    public boolean start() {
        if (running) return true;
        for (int attempt = 0; attempt < 3; attempt++) {
            try {
                httpServer = new ServerSocket();
                httpServer.setReuseAddress(true);
                httpServer.bind(new InetSocketAddress(InetAddress.getByName(localIp), HTTP_PORT));
                running = true;
                listeners.execute(this::runHttpLoop);
                listeners.execute(this::runSsdpLoop);
                playerHandler.post(positionTicker);
                listener.onDlnaLog("DLNA/UPnP MediaRenderer listening on " + localIp + ":" + HTTP_PORT);
                return true;
            } catch (IOException e) {
                try { if (httpServer != null) httpServer.close(); } catch (IOException ignored) {}
                httpServer = null;
                if (attempt < 2) {
                    try { Thread.sleep(120); } catch (InterruptedException ignored) {}
                } else {
                    listener.onDlnaError("DLNA HTTP 端口 " + HTTP_PORT + " 启动失败: " + e.getMessage());
                    stop();
                    return false;
                }
            }
        }
        return false;
    }

    public synchronized void stop() {
        if (closed) return;
        closed = true;
        running = false;
        invalidatePlayback(false);
        prepExecutor.shutdownNow();
        playerHandler.removeCallbacks(positionTicker);
        sendNotifyNow("ssdp:byebye");
        try { if (httpServer != null) httpServer.close(); } catch (IOException ignored) {}
        try { if (ssdpSocket != null) ssdpSocket.close(); } catch (Throwable ignored) {}
        for (Socket socket : openClients) {
            try { socket.close(); } catch (IOException ignored) {}
        }
        listeners.shutdownNow();
        httpWorkers.shutdownNow();
        playerHandler.removeCallbacksAndMessages(null);
        playerHandler.post(() -> {
            releasePlayer();
            playerThread.quitSafely();
        });
    }

    public void setVolumeFromSystem(int percent) {
        this.volumePercent = Math.max(0, Math.min(100, percent));
    }

    public void togglePauseResume(boolean pause) {
        playWhenPrepared = !pause;
        long expectedSession = session;
        playerHandler.post(() -> {
            if (session != expectedSession || player == null || !mediaPrepared) return;
            try {
                if (pause && player.isPlaying()) {
                    player.pause();
                    playing = false;
                    emitState("PAUSED_PLAYBACK");
                } else if (!pause && !player.isPlaying()) {
                    player.start();
                    playing = true;
                    emitState("PLAYING");
                }
            } catch (IllegalStateException ignored) {}
        });
    }

    private final Runnable positionTicker = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            if (playing && player != null && mediaPrepared) {
                try {
                    if (player.isPlaying()) {
                        emitState("PLAYING");
                    }
                } catch (IllegalStateException ignored) {}
            }
            playerHandler.postDelayed(this, 1000);
        }
    };

    private void runHttpLoop() {
        while (running) {
            try {
                Socket client = httpServer.accept();
                openClients.add(client);
                if (!running) { client.close(); openClients.remove(client); break; }
                try {
                    httpWorkers.execute(() -> handleHttp(client));
                } catch (RejectedExecutionException rejected) {
                    try { client.close(); } catch (IOException ignored) {}
                    openClients.remove(client);
                }
            } catch (IOException e) {
                if (running) Log.w(TAG, "HTTP accept", e);
                break;
            }
        }
    }

    private void runSsdpLoop() {
        try {
            NetworkInterface nif = NetworkInterface.getByName(ifaceName);
            synchronized (this) {
                if (!running) return;
                ssdpSocket = new MulticastSocket((java.net.SocketAddress) null);
                ssdpSocket.setReuseAddress(true);
                ssdpSocket.bind(new InetSocketAddress(SSDP_PORT));
                ssdpSocket.setSoTimeout(3000);
                InetAddress group = InetAddress.getByName(SSDP_ADDR);
                if (nif != null) {
                    ssdpSocket.joinGroup(new InetSocketAddress(group, SSDP_PORT), nif);
                } else {
                    ssdpSocket.joinGroup(group);
                }
            }
            sendNotify("ssdp:alive");
            long lastAlive = System.currentTimeMillis();
            byte[] buf = new byte[4096];
            while (running) {
                if (System.currentTimeMillis() - lastAlive > 25000) {
                    sendNotify("ssdp:alive");
                    lastAlive = System.currentTimeMillis();
                }
                try {
                    DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                    ssdpSocket.receive(pkt);
                    String msg = new String(pkt.getData(), 0, pkt.getLength(), StandardCharsets.UTF_8);
                    if (msg.startsWith("M-SEARCH")) {
                        String st = extractHeader(msg, "ST");
                        if (st != null && isMatchingSearchTarget(st)) {
                            sendSearchResponse(pkt.getAddress(), pkt.getPort(), st);
                        }
                    }
                } catch (SocketTimeoutException ignored) {
                } catch (IOException e) {
                    if (running) Log.w(TAG, "SSDP recv", e);
                }
            }
        } catch (IOException e) {
            listener.onDlnaLog("SSDP 组播监听未开启: " + e.getMessage());
        }
    }

    private boolean isMatchingSearchTarget(String st) {
        return "ssdp:all".equalsIgnoreCase(st)
                || "upnp:rootdevice".equalsIgnoreCase(st)
                || st.contains("MediaRenderer")
                || st.contains("AVTransport")
                || st.contains("RenderingControl")
                || st.equalsIgnoreCase(udn);
    }

    private void sendNotify(String nts) {
        if (ssdpSocket == null || !running) return;
        sendNotifyNow(nts);
    }

    private void sendNotifyNow(String nts) {
        if (ssdpSocket == null) return;
        String[] types = {
                "upnp:rootdevice",
                udn,
                "urn:schemas-upnp-org:device:MediaRenderer:1",
                "urn:schemas-upnp-org:service:AVTransport:1",
                "urn:schemas-upnp-org:service:RenderingControl:1"
        };
        try {
            InetAddress group = InetAddress.getByName(SSDP_ADDR);
            for (String nt : types) {
                String usn = nt.equals(udn) ? udn : udn + "::" + nt;
                String pkt = "NOTIFY * HTTP/1.1\r\n"
                        + "HOST: 239.255.255.250:1900\r\n"
                        + "CACHE-CONTROL: max-age=1800\r\n"
                        + "LOCATION: http://" + localIp + ":" + HTTP_PORT + "/description.xml\r\n"
                        + "NT: " + nt + "\r\n"
                        + "NTS: " + nts + "\r\n"
                        + "SERVER: Android/15 UPnP/1.1 SoundFlow/1.0\r\n"
                        + "USN: " + usn + "\r\n\r\n";
                byte[] data = pkt.getBytes(StandardCharsets.UTF_8);
                ssdpSocket.send(new DatagramPacket(data, data.length, group, SSDP_PORT));
            }
        } catch (Throwable ignored) {}
    }

    private void sendSearchResponse(InetAddress addr, int port, String st) {
        if (ssdpSocket == null) return;
        String target = "ssdp:all".equalsIgnoreCase(st) ? "urn:schemas-upnp-org:device:MediaRenderer:1" : st;
        String usn = target.equals(udn) ? udn : udn + "::" + target;
        String msg = "HTTP/1.1 200 OK\r\n"
                + "CACHE-CONTROL: max-age=1800\r\n"
                + "EXT:\r\n"
                + "LOCATION: http://" + localIp + ":" + HTTP_PORT + "/description.xml\r\n"
                + "SERVER: Android/15 UPnP/1.1 SoundFlow/1.0\r\n"
                + "ST: " + target + "\r\n"
                + "USN: " + usn + "\r\n\r\n";
        try {
            byte[] data = msg.getBytes(StandardCharsets.UTF_8);
            ssdpSocket.send(new DatagramPacket(data, data.length, addr, port));
        } catch (IOException ignored) {}
    }

    private static int remainingTimeoutMs(long deadlineNano) throws SocketTimeoutException {
        long remMs = (deadlineNano - System.nanoTime()) / 1_000_000L;
        if (remMs <= 0L) {
            throw new SocketTimeoutException("HTTP request total deadline exceeded");
        }
        return (int) Math.min(HTTP_REQ_DEADLINE_MS, Math.max(1L, remMs));
    }

    private void handleHttp(Socket client) {
        final long deadlineNano = System.nanoTime() + HTTP_REQ_DEADLINE_MS * 1_000_000L;
        try (Socket sock = client) {
            sock.setSoTimeout(remainingTimeoutMs(deadlineNano));
            InputStream in = new BufferedInputStream(sock.getInputStream(), 8192);
            ByteArrayOutputStream hdrBuf = new ByteArrayOutputStream();
            int b, prev = 0, step = 0;
            while (true) {
                if ((hdrBuf.size() & 63) == 0) {
                    sock.setSoTimeout(remainingTimeoutMs(deadlineNano));
                }
                b = in.read();
                if (b == -1) break;
                hdrBuf.write(b);
                if (b == '\n' && prev == '\r') {
                    step++;
                    if (step == 2) break;
                } else if (b != '\r') {
                    step = 0;
                }
                prev = b;
                if (hdrBuf.size() > 16384) return;
            }
            if (step != 2) return;
            String headerText = hdrBuf.toString(StandardCharsets.UTF_8);
            if (headerText.isEmpty()) return;
            String[] lines = headerText.split("\r\n");
            String[] req = lines[0].split(" ");
            if (req.length < 2) return;
            String method = req[0];
            String path = req[1];
            Map<String, String> headers = new HashMap<>();
            for (int i = 1; i < lines.length; i++) {
                int c = lines[i].indexOf(':');
                if (c > 0) headers.put(lines[i].substring(0, c).trim().toLowerCase(Locale.ROOT), lines[i].substring(c + 1).trim());
            }
            if (headers.containsKey("transfer-encoding")) return;
            int contentLen = 0;
            try {
                if (headers.containsKey("content-length")) {
                    int parsedLen = Integer.parseInt(headers.get("content-length"));
                    if (parsedLen < 0 || parsedLen > 262144) return;
                    contentLen = parsedLen;
                }
            } catch (NumberFormatException badLen) {
                return;
            }
            byte[] bodyBytes = new byte[contentLen];
            int read = 0;
            while (read < contentLen) {
                sock.setSoTimeout(remainingTimeoutMs(deadlineNano));
                int n = in.read(bodyBytes, read, contentLen - read);
                if (n <= 0) return;
                read += n;
            }
            String body = new String(bodyBytes, 0, read, StandardCharsets.UTF_8);

            sock.setSoTimeout(1500);
            if ("GET".equalsIgnoreCase(method) && path.startsWith("/description.xml")) {
                writeHttp(sock.getOutputStream(), "200 OK", "text/xml; charset=\"utf-8\"", descriptionXml());
            } else if ("GET".equalsIgnoreCase(method) && path.contains("scpd.xml")) {
                writeHttp(sock.getOutputStream(), "200 OK", "text/xml; charset=\"utf-8\"", path.contains("Rendering") ? renderingScpdXml() : avTransportScpdXml());
            } else if ("POST".equalsIgnoreCase(method) && path.contains("AVTransport")) {
                String action = headers.getOrDefault("soapaction", "");
                writeHttp(sock.getOutputStream(), "200 OK", "text/xml; charset=\"utf-8\"", handleAvTransport(action, body));
            } else if ("POST".equalsIgnoreCase(method) && path.contains("RenderingControl")) {
                String action = headers.getOrDefault("soapaction", "");
                writeHttp(sock.getOutputStream(), "200 OK", "text/xml; charset=\"utf-8\"", handleRenderingControl(action, body));
            } else if ("POST".equalsIgnoreCase(method) && path.contains("ConnectionManager")) {
                writeHttp(sock.getOutputStream(), "200 OK", "text/xml; charset=\"utf-8\"", soapResp("ConnectionManager", "GetProtocolInfo", "<Source></Source><Sink>http-get:*:audio/*:*</Sink>"));
            } else {
                writeHttp(sock.getOutputStream(), "200 OK", "text/plain", "SoundFlow Receiver");
            }
        } catch (IOException ignored) {
        } finally {
            openClients.remove(client);
        }
    }

    private void enqueuePrepareAsync(long targetSession, String uri) {
        cancelActiveDataSource();
        PrepJob job = new PrepJob(targetSession, uri);
        latestPrepJob.set(job);
        try {
            prepExecutor.execute(() -> runPrepareJob(job));
        } catch (RejectedExecutionException ignored) {}
    }

    private void runPrepareJob(PrepJob job) {
        if (!running || latestPrepJob.get() != job || session != job.session) return;
        Set<String> localAddrs = collectAllLocalAddresses(localIp);
        PinnedHttpMediaDataSource ds = new PinnedHttpMediaDataSource(job.uri, localAddrs);
        activeDataSource.set(ds);
        if (!running || latestPrepJob.get() != job || session != job.session) {
            ds.cancelNow();
            activeDataSource.compareAndSet(ds, null);
            return;
        }
        try {
            ds.openInitial();
        } catch (IOException e) {
            ds.cancelNow();
            activeDataSource.compareAndSet(ds, null);
            if (running && session == job.session) {
                playerHandler.post(() -> {
                    if (session != job.session) return;
                    releasePlayer();
                    listener.onDlnaError("DLNA 媒体地址未通过安全连接校验或不可达: " + e.getMessage());
                    emitState("STOPPED");
                });
            }
            return;
        }
        playerHandler.post(() -> {
            if (!running || session != job.session) {
                ds.cancelNow();
                activeDataSource.compareAndSet(ds, null);
                return;
            }
            attachPreparedDataSource(job, ds);
        });
    }

    private String handleAvTransport(String action, String body) {
        if (action.contains("SetAVTransportURI")) {
            String uri = xmlTag(body, "CurrentURI");
            Set<String> localAddrs = collectAllLocalAddresses(localIp);
            if (!isAllowedMediaUriWithLocals(uri, localAddrs)) {
                listener.onDlnaLog("DLNA 拒绝非 HTTP(S) 或受限目标 URI");
                return soapResp("AVTransport", "SetAVTransportURI", "");
            }
            String meta = unescapeXml(xmlTag(body, "CurrentURIMetaData"));
            final long targetSession = invalidatePlayback(true);
            playerHandler.post(() -> {
                if (!running || session != targetSession) return;
                releasePlayerKeepDataSource();
                if (!uri.equals(currentUri)) { title = ""; artist = ""; album = ""; }
                currentUri = uri;
                parseDidlMetadata(meta);
                listener.onDlnaPreparing(targetSession, title, artist, album);
                listener.onDlnaLog("DLNA SetURI: " + title);
                enqueuePrepareAsync(targetSession, uri);
            });
            return soapResp("AVTransport", "SetAVTransportURI", "");
        }
        if (action.contains("Play")) {
            long expectedSession = session;
            playerHandler.post(() -> {
                if (session != expectedSession) return;
                playWhenPrepared = true;
                if (player != null && mediaPrepared) {
                    try {
                        player.start();
                        playing = true;
                        emitState("PLAYING");
                    } catch (IllegalStateException ignored) {}
                } else if (player == null && latestPrepJob.get() == null && !currentUri.isEmpty()) {
                    long targetSession = invalidatePlayback(true);
                    listener.onDlnaPreparing(targetSession, title, artist, album);
                    enqueuePrepareAsync(targetSession, currentUri);
                }
            });
            return soapResp("AVTransport", "Play", "");
        }
        if (action.contains("Pause")) {
            togglePauseResume(true);
            return soapResp("AVTransport", "Pause", "");
        }
        if (action.contains("Stop")) {
            long stoppedSession = invalidatePlayback(false);
            playerHandler.post(() -> {
                if (session != stoppedSession) return;
                releasePlayer();
                playing = false;
                emitState("STOPPED");
            });
            return soapResp("AVTransport", "Stop", "");
        }
        if (action.contains("Seek")) {
            long targetMs = Math.min(Integer.MAX_VALUE, Math.max(0L, parseDlnaTimeMs(xmlTag(body, "Target"))));
            if (sampledDuration > 0) targetMs = Math.min(sampledDuration, targetMs);
            if (mediaPrepared) {
                SeekRequest seek = new SeekRequest(session, targetMs);
                pendingSeek = seek;
                playerHandler.post(() -> {
                    if (pendingSeek != seek || session != seek.session || player == null || !mediaPrepared) return;
                    try {
                        player.seekTo(seek.targetMs, MediaPlayer.SEEK_CLOSEST);
                    } catch (IllegalStateException e) {
                        if (pendingSeek == seek) pendingSeek = null;
                    }
                });
            }
            return soapResp("AVTransport", "Seek", "");
        }
        if (action.contains("GetTransportInfo")) {
            String st = playing ? "PLAYING" : (mediaPrepared ? "PAUSED_PLAYBACK" : (latestPrepJob.get() != null ? "TRANSITIONING" : "STOPPED"));
            return soapResp("AVTransport", "GetTransportInfo",
                    "<CurrentTransportState>" + st + "</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>");
        }
        if (action.contains("GetPositionInfo")) {
            long dur = sampledDuration;
            SeekRequest seek = pendingSeek;
            boolean awaitingSeek = seek != null && seek.session == session
                    && SystemClock.elapsedRealtime() - seek.requestedAt < 4_000L;
            long pos = awaitingSeek ? seek.targetMs : sampledPosition
                    + (playing ? Math.max(0, SystemClock.elapsedRealtime() - sampledAt) : 0);
            if (dur > 0) pos = Math.min(dur, pos);
            return soapResp("AVTransport", "GetPositionInfo",
                    "<Track>1</Track><TrackDuration>" + formatDlnaTime(dur) + "</TrackDuration><TrackMetaData></TrackMetaData>"
                            + "<TrackURI>" + escapeXml(currentUri) + "</TrackURI><RelTime>" + formatDlnaTime(pos) + "</RelTime><AbsTime>" + formatDlnaTime(pos) + "</AbsTime><RelCount>0</RelCount><AbsCount>0</AbsCount>");
        }
        return soapResp("AVTransport", "Unknown", "");
    }

    private String handleRenderingControl(String action, String body) {
        if (action.contains("GetVolume")) {
            return soapResp("RenderingControl", "GetVolume", "<CurrentVolume>" + volumePercent + "</CurrentVolume>");
        }
        if (action.contains("SetVolume")) {
            try {
                int v = Integer.parseInt(xmlTag(body, "DesiredVolume").trim());
                volumePercent = Math.max(0, Math.min(100, v));
                listener.onDlnaVolume(volumePercent);
            } catch (NumberFormatException ignored) {}
            return soapResp("RenderingControl", "SetVolume", "");
        }
        if (action.contains("GetMute")) {
            return soapResp("RenderingControl", "GetMute", "<CurrentMute>0</CurrentMute>");
        }
        return soapResp("RenderingControl", "OK", "");
    }

    private void attachPreparedDataSource(PrepJob job, PinnedHttpMediaDataSource ds) {
        long expectedSession = job.session;
        if (!running || session != expectedSession || latestPrepJob.get() != job) {
            ds.cancelNow();
            return;
        }
        releasePlayerKeepDataSource();
        try {
            player = new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            player.setDataSource(ds);
            player.setOnPreparedListener(mp -> {
                if (!running || player != mp || session != expectedSession) {
                    return;
                }
                latestPrepJob.compareAndSet(job, null);
                audioSessionId = mp.getAudioSessionId();
                mediaPrepared = true;
                try {
                    if (playWhenPrepared) mp.start();
                    playing = playWhenPrepared;
                    emitState(playing ? "PLAYING" : "PAUSED_PLAYBACK");
                } catch (IllegalStateException e) {
                    releasePlayer();
                    listener.onDlnaError("DLNA 无法启动播放");
                    emitState("STOPPED");
                }
            });
            player.setOnSeekCompleteListener(mp -> {
                if (player != mp) return;
                SeekRequest seek = pendingSeek;
                long actual = getPositionSafe();
                if (seek == null || Math.abs(actual - seek.targetMs) <= 1000L) {
                    emitState(playing ? "PLAYING" : "PAUSED_PLAYBACK");
                    if (pendingSeek == seek) pendingSeek = null;
                }
            });
            player.setOnCompletionListener(mp -> {
                if (player != mp) return;
                playing = false;
                emitState("STOPPED");
            });
            player.setOnErrorListener((mp, what, extra) -> {
                if (player != mp || session != expectedSession) return true;
                releasePlayer();
                listener.onDlnaError("DLNA 音频流播放失败 (" + what + "/" + extra + ")");
                emitState("STOPPED");
                return true;
            });
            player.prepareAsync();
        } catch (Throwable e) {
            ds.cancelNow();
            releasePlayer();
            listener.onDlnaError("DLNA 打开媒体流失败: " + e.getMessage());
            emitState("STOPPED");
        }
    }

    private void releasePlayerKeepDataSource() {
        synchronized (visualizerLock) {
            audioSessionId = 0;
            releaseVisualizerLocked();
            visualizerUnavailable = false;
        }
        mediaPrepared = false;
        playing = false;
        pendingSeek = null;
        sampledDuration = 0;
        sampledPosition = 0;
        sampledAt = SystemClock.elapsedRealtime();
        if (player != null) {
            try { player.stop(); } catch (Throwable ignored) {}
            try { player.release(); } catch (Throwable ignored) {}
            player = null;
        }
    }

    private void releasePlayer() {
        cancelActiveDataSource();
        releasePlayerKeepDataSource();
    }

    private long getDurationSafe() {
        try { return (player != null && mediaPrepared) ? Math.max(0, player.getDuration()) : 0; }
        catch (IllegalStateException e) { return 0; }
    }

    private long getPositionSafe() {
        try { return (player != null && mediaPrepared) ? Math.max(0, player.getCurrentPosition()) : 0; }
        catch (IllegalStateException e) { return 0; }
    }

    private void emitState(String state) {
        sampledDuration = getDurationSafe();
        sampledPosition = getPositionSafe();
        sampledAt = SystemClock.elapsedRealtime();
        listener.onDlnaState(session, playing, state, title, artist, album, sampledDuration, sampledPosition);
    }

    private void parseDidlMetadata(String xml) {
        if (xml == null || xml.isEmpty()) return;
        String t = xmlTag(xml, "dc:title");
        String a = xmlTag(xml, "upnp:artist");
        if (a.isEmpty()) a = xmlTag(xml, "dc:creator");
        String al = xmlTag(xml, "upnp:album");
        if (!t.isEmpty()) title = t;
        if (!a.isEmpty()) artist = a;
        if (!al.isEmpty()) album = al;
    }

    private String descriptionXml() {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                + "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                + "<specVersion><major>1</major><minor>0</minor></specVersion>"
                + "<device>"
                + "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
                + "<friendlyName>" + escapeXml(friendlyName) + " (DLNA)</friendlyName>"
                + "<manufacturer>SoundFlow</manufacturer><modelName>SoundFlow Audio Renderer</modelName>"
                + "<UDN>" + udn + "</UDN>"
                + "<serviceList>"
                + "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId><controlURL>/AVTransport/control</controlURL><eventSubURL>/AVTransport/event</eventSubURL><SCPDURL>/AVTransport/scpd.xml</SCPDURL></service>"
                + "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId><controlURL>/RenderingControl/control</controlURL><eventSubURL>/RenderingControl/event</eventSubURL><SCPDURL>/RenderingControl/scpd.xml</SCPDURL></service>"
                + "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><controlURL>/ConnectionManager/control</controlURL><eventSubURL>/ConnectionManager/event</eventSubURL><SCPDURL>/ConnectionManager/scpd.xml</SCPDURL></service>"
                + "</serviceList></device></root>";
    }

    private static String avTransportScpdXml() {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><actionList><action><name>SetAVTransportURI</name></action><action><name>Play</name></action><action><name>Pause</name></action><action><name>Stop</name></action><action><name>Seek</name></action><action><name>GetTransportInfo</name></action><action><name>GetPositionInfo</name></action></actionList></scpd>";
    }

    private static String renderingScpdXml() {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><actionList><action><name>GetVolume</name></action><action><name>SetVolume</name></action><action><name>GetMute</name></action></actionList></scpd>";
    }

    private static String soapResp(String service, String action, String body) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                + "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                + "<s:Body><u:" + action + "Response xmlns:u=\"urn:schemas-upnp-org:service:" + service + ":1\">"
                + body + "</u:" + action + "Response></s:Body></s:Envelope>";
    }

    private static void writeHttp(OutputStream out, String status, String ctype, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        String hdr = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype + "\r\nContent-Length: " + bytes.length + "\r\nConnection: close\r\n\r\n";
        out.write(hdr.getBytes(StandardCharsets.UTF_8));
        out.write(bytes);
        out.flush();
    }

    private static String extractHeader(String msg, String name) {
        for (String line : msg.split("\r\n")) {
            if (line.toLowerCase(Locale.ROOT).startsWith(name.toLowerCase(Locale.ROOT) + ":")) {
                return line.substring(name.length() + 1).trim();
            }
        }
        return null;
    }

    private static String canonicalIpKey(InetAddress addr) {
        if (addr == null) return "";
        if (addr instanceof Inet6Address) {
            byte[] b = addr.getAddress();
            if (b != null && b.length == 16) {
                boolean v4Mapped = true;
                for (int i = 0; i < 10; i++) {
                    if (b[i] != 0) { v4Mapped = false; break; }
                }
                if (v4Mapped && b[10] == (byte) 0xff && b[11] == (byte) 0xff) {
                    return (b[12] & 0xff) + "." + (b[13] & 0xff) + "." + (b[14] & 0xff) + "." + (b[15] & 0xff);
                }
            }
        }
        String raw = addr.getHostAddress();
        if (raw == null) return "";
        int zone = raw.indexOf('%');
        return (zone >= 0 ? raw.substring(0, zone) : raw).toLowerCase(Locale.ROOT);
    }

    static Set<String> collectAllLocalAddresses(String extraSelfIp) {
        Set<String> out = new HashSet<>();
        if (extraSelfIp != null && !extraSelfIp.trim().isEmpty()) {
            out.add(extraSelfIp.trim().toLowerCase(Locale.ROOT));
            try {
                out.add(canonicalIpKey(InetAddress.getByName(extraSelfIp.trim())));
            } catch (Exception ignored) {}
        }
        try {
            Enumeration<NetworkInterface> ifaces = NetworkInterface.getNetworkInterfaces();
            while (ifaces != null && ifaces.hasMoreElements()) {
                NetworkInterface nif = ifaces.nextElement();
                Enumeration<InetAddress> addrs = nif.getInetAddresses();
                while (addrs != null && addrs.hasMoreElements()) {
                    String key = canonicalIpKey(addrs.nextElement());
                    if (!key.isEmpty()) out.add(key);
                }
            }
        } catch (Exception ignored) {}
        return out;
    }

    private static boolean isPrivateLanAddress(InetAddress addr) {
        if (addr instanceof Inet4Address) {
            byte[] b = addr.getAddress();
            int b0 = b[0] & 0xff, b1 = b[1] & 0xff;
            return b0 == 10
                    || (b0 == 172 && (b1 >= 16 && b1 <= 31))
                    || (b0 == 192 && b1 == 168)
                    || (b0 == 100 && (b1 & 0xc0) == 64);
        }
        if (addr instanceof Inet6Address) {
            byte[] b = addr.getAddress();
            if (b.length == 16) {
                boolean v4Mapped = true;
                for (int i = 0; i < 10; i++) {
                    if (b[i] != 0) { v4Mapped = false; break; }
                }
                if (v4Mapped && b[10] == (byte) 0xff && b[11] == (byte) 0xff) {
                    int b0 = b[12] & 0xff, b1 = b[13] & 0xff;
                    return b0 == 10
                            || (b0 == 172 && (b1 >= 16 && b1 <= 31))
                            || (b0 == 192 && b1 == 168)
                            || (b0 == 100 && (b1 & 0xc0) == 64);
                }
                return (b[0] & 0xfe) == 0xfc;
            }
        }
        return false;
    }

    static boolean isAllowedInetAddress(InetAddress addr, Set<String> localAddrs) {
        if (addr == null) return false;
        if (addr.isLoopbackAddress() || addr.isAnyLocalAddress()
                || addr.isLinkLocalAddress() || addr.isMulticastAddress()) {
            return false;
        }
        String key = canonicalIpKey(addr);
        if (!key.isEmpty() && localAddrs != null && localAddrs.contains(key)) {
            return false;
        }
        if (addr instanceof Inet4Address) {
            byte[] b = addr.getAddress();
            if (b.length != 4) return false;
            int b0 = b[0] & 0xff;
            if (b0 == 0 || b0 == 127 || b0 >= 224) return false;
            if ((b[0] & 0xff) == 255 && (b[1] & 0xff) == 255
                    && (b[2] & 0xff) == 255 && (b[3] & 0xff) == 255) {
                return false;
            }
        } else if (addr instanceof Inet6Address) {
            byte[] b = addr.getAddress();
            if (b.length != 16) return false;
            boolean v4Mapped = true;
            for (int i = 0; i < 10; i++) {
                if (b[i] != 0) { v4Mapped = false; break; }
            }
            if (v4Mapped && b[10] == (byte) 0xff && b[11] == (byte) 0xff) {
                int b0 = b[12] & 0xff, b1 = b[13] & 0xff;
                if (b0 == 0 || b0 == 127 || b0 >= 224 || (b0 == 169 && b1 == 254)) {
                    return false;
                }
            }
            // Reject documentation prefix 2001:db8::/32
            if ((b[0] & 0xff) == 0x20 && (b[1] & 0xff) == 0x01
                    && (b[2] & 0xff) == 0x0d && (b[3] & 0xff) == 0xb8) {
                return false;
            }
        }
        return true;
    }

    static boolean isAllowedMediaUri(String raw, String selfIp) {
        return isAllowedMediaUriWithLocals(raw, collectAllLocalAddresses(selfIp));
    }

    static boolean isAllowedMediaUriWithLocals(String raw, Set<String> localAddrs) {
        if (raw == null || raw.isEmpty() || raw.length() > 4096) return false;
        String trimmed = raw.trim();
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (c <= 0x20 || c == 0x7f || c == '\\') return false;
        }
        try {
            URI u = URI.create(trimmed);
            String scheme = u.getScheme();
            if (scheme == null || (!"http".equalsIgnoreCase(scheme) && !"https".equalsIgnoreCase(scheme))) {
                return false;
            }
            if (u.getRawUserInfo() != null) return false;
            String host = u.getHost();
            if (host == null || host.isEmpty()) return false;
            String cleanHost = host.startsWith("[") && host.endsWith("]")
                    ? host.substring(1, host.length() - 1) : host;
            String lower = cleanHost.toLowerCase(Locale.ROOT);
            if ("localhost".equals(lower) || lower.endsWith(".localhost") || lower.endsWith(".localdomain")
                    || "ip6-localhost".equals(lower) || "ip6-loopback".equals(lower)
                    || "::1".equals(lower) || "0:0:0:0:0:0:0:1".equals(lower)
                    || "::".equals(lower) || "0.0.0.0".equals(lower)
                    || "255.255.255.255".equals(lower)
                    || lower.startsWith("0.") || lower.startsWith("127.") || lower.startsWith("169.254.")
                    || lower.startsWith("fe80:") || lower.startsWith("::ffff:127.")
                    || lower.startsWith("::ffff:169.254.") || lower.startsWith("::ffff:0.")
                    || (localAddrs != null && localAddrs.contains(lower))) {
                return false;
            }
            if (u.getPort() == 0) return false;
            // Literal IP syntax check without DNS lookup
            if (lower.contains(":") || lower.matches("^\\d+\\.\\d+\\.\\d+\\.\\d+$")) {
                InetAddress literal = InetAddress.getByName(cleanHost);
                if (!isAllowedInetAddress(literal, localAddrs)) return false;
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Controlled MediaDataSource that pins EVERY TCP/TLS connection and HTTP Range request
     * directly to validated InetAddress endpoints, eliminating TOCTOU / DNS rebinding and
     * uncontrolled MediaPlayer redirects while supporting immediate cancellation.
     */
    static final class PinnedHttpMediaDataSource extends MediaDataSource {
        interface ResolverHook {
            InetAddress[] resolve(String host) throws IOException;
        }

        private static final int MAX_CACHE_WINDOW = 256 * 1024;
        private static final long MAX_MEDIA_BYTES = 128L * 1024 * 1024;
        private static final Pattern CONTENT_RANGE = Pattern.compile("bytes (\\d+)-(\\d+)/(\\d+|\\*)");

        private final String initialUri;
        private final Set<String> localAddrs;
        private final ResolverHook resolver;
        private final AtomicBoolean cancelled = new AtomicBoolean(false);
        private final Object ioLock = new Object();

        private volatile Socket activeSocket;
        private InputStream activeStream;
        private String pinnedFinalUri;
        private InetAddress pinnedFinalAddr;

        private long streamCursor = -1L;
        private volatile long totalLength = -1L;
        private byte[] windowBuf = new byte[0];
        private long windowStart = 0L;
        private int windowLen = 0;

        PinnedHttpMediaDataSource(String initialUri, Set<String> localAddrs) {
            this(initialUri, localAddrs, DlnaRendererServer::resolveMediaHost);
        }

        PinnedHttpMediaDataSource(String initialUri, Set<String> localAddrs, ResolverHook resolver) {
            this.initialUri = initialUri;
            this.localAddrs = localAddrs != null ? localAddrs : Collections.emptySet();
            this.resolver = resolver;
        }

        void cancelNow() {
            cancelled.set(true);
            Socket s = activeSocket;
            activeSocket = null;
            if (s != null) {
                try { s.close(); } catch (IOException ignored) {}
            }
        }

        private void closeStreamLocked() {
            InputStream in = activeStream;
            activeStream = null;
            Socket s = activeSocket;
            activeSocket = null;
            if (in != null) {
                try { in.close(); } catch (IOException ignored) {}
            }
            if (s != null) {
                try { s.close(); } catch (IOException ignored) {}
            }
            streamCursor = -1L;
        }

        void openInitial() throws IOException {
            synchronized (ioLock) {
                if (cancelled.get()) throw new IOException("cancelled");
                openAtOffsetLocked(0L, true);
            }
        }

        private Socket connectPinnedSocket(InetAddress verifiedAddr, int port, boolean https, String tlsHost) throws IOException {
            if (!isAllowedInetAddress(verifiedAddr, collectAllLocalAddresses(null))) {
                throw new IOException("Pinned address is now a local interface");
            }
            Socket raw = new Socket();
            activeSocket = raw;
            try {
                if (cancelled.get()) throw new IOException("cancelled");
                raw.connect(new InetSocketAddress(verifiedAddr, port), MEDIA_CONNECT_TIMEOUT_MS);
                raw.setSoTimeout(MEDIA_READ_TIMEOUT_MS);
                if (!https) return raw;

                SSLSocket ssl = (SSLSocket) ((SSLSocketFactory) SSLSocketFactory.getDefault())
                        .createSocket(raw, tlsHost, port, true);
                activeSocket = ssl;
                if (cancelled.get()) throw new IOException("cancelled");
                ssl.setSoTimeout(MEDIA_READ_TIMEOUT_MS);
                SSLParameters params = ssl.getSSLParameters();
                params.setEndpointIdentificationAlgorithm("HTTPS");
                ssl.setSSLParameters(params);
                ssl.startHandshake();
                return ssl;
            } catch (IOException e) {
                closeStreamLocked();
                try { raw.close(); } catch (IOException ignored) {}
                throw e;
            }
        }

        private static String readHeaderBlock(Socket socket, InputStream in) throws IOException {
            ByteArrayOutputStream hdr = new ByteArrayOutputStream(512);
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(MEDIA_READ_TIMEOUT_MS);
            int prev = 0, step = 0;
            while (step < 2) {
                socket.setSoTimeout(remainingTimeoutMs(deadline));
                int b = in.read();
                if (b < 0) throw new EOFException("Incomplete HTTP response header");
                hdr.write(b);
                if (b == '\n' && prev == '\r') {
                    step++;
                } else if (b != '\r') {
                    step = 0;
                }
                prev = b;
                if (hdr.size() > 32768) throw new IOException("HTTP response header too large");
            }
            socket.setSoTimeout(MEDIA_READ_TIMEOUT_MS);
            return hdr.toString(StandardCharsets.ISO_8859_1.name());
        }

        private static final class ChunkedBodyInputStream extends InputStream {
            private final InputStream in;
            private long remainingInChunk = 0L;
            private boolean eof = false;

            ChunkedBodyInputStream(InputStream in) {
                this.in = in;
            }

            private String readCrLfLine() throws IOException {
                ByteArrayOutputStream line = new ByteArrayOutputStream(32);
                int prev = 0, b;
                while ((b = in.read()) != -1) {
                    if (b == '\n' && prev == '\r') {
                        byte[] raw = line.toByteArray();
                        return new String(raw, 0, Math.max(0, raw.length - 1), StandardCharsets.US_ASCII);
                    }
                    line.write(b);
                    prev = b;
                    if (line.size() > 2048) throw new IOException("Malformed chunk header");
                }
                throw new EOFException("Unexpected EOF in chunked encoding");
            }

            private void nextChunk() throws IOException {
                String line = readCrLfLine().trim();
                int semi = line.indexOf(';');
                if (semi >= 0) line = line.substring(0, semi).trim();
                if (line.isEmpty()) throw new IOException("Empty chunk size");
                try {
                    remainingInChunk = Long.parseLong(line, 16);
                } catch (NumberFormatException e) {
                    throw new IOException("Invalid chunk size", e);
                }
                if (remainingInChunk < 0L || remainingInChunk > 128L * 1024L * 1024L) {
                    throw new IOException("Chunk size out of bounds");
                }
                if (remainingInChunk == 0L) {
                    eof = true;
                    // Drain trailing headers until empty line
                    while (!readCrLfLine().isEmpty()) { /* drain */ }
                }
            }

            @Override
            public int read() throws IOException {
                byte[] one = new byte[1];
                int n = read(one, 0, 1);
                return n <= 0 ? -1 : (one[0] & 0xff);
            }

            @Override
            public int read(byte[] b, int off, int len) throws IOException {
                if (len == 0) return 0;
                if (eof) return -1;
                if (remainingInChunk == 0L) {
                    nextChunk();
                    if (eof) return -1;
                }
                int toRead = (int) Math.min(len, remainingInChunk);
                int n = in.read(b, off, toRead);
                if (n <= 0) throw new EOFException("Truncated chunk payload");
                remainingInChunk -= n;
                if (remainingInChunk == 0L) {
                    int r = in.read();
                    int lf = in.read();
                    if (r != '\r' || lf != '\n') throw new IOException("Missing CRLF after chunk");
                }
                return n;
            }

            @Override
            public void close() throws IOException {
                in.close();
            }
        }

        private static final class BoundedBodyInputStream extends InputStream {
            private final InputStream in;
            private long remaining;

            BoundedBodyInputStream(InputStream in, long length) {
                this.in = in;
                this.remaining = Math.max(0L, length);
            }

            @Override
            public int read() throws IOException {
                if (remaining <= 0L) return -1;
                int b = in.read();
                if (b >= 0) remaining--;
                return b;
            }

            @Override
            public int read(byte[] b, int off, int len) throws IOException {
                if (len == 0) return 0;
                if (remaining <= 0L) return -1;
                int n = in.read(b, off, (int) Math.min(len, remaining));
                if (n > 0) remaining -= n;
                return n;
            }

            @Override
            public void close() throws IOException {
                in.close();
            }
        }

        private void openAtOffsetLocked(long offset, boolean resolveRedirects) throws IOException {
            closeStreamLocked();
            try {
                openStreamLocked(offset, resolveRedirects);
            } catch (IOException e) {
                closeStreamLocked();
                throw e;
            }
        }

        private void openStreamLocked(long offset, boolean resolveRedirects) throws IOException {
            String current = (resolveRedirects || pinnedFinalUri == null) ? initialUri : pinnedFinalUri;
            InetAddress previous = null;

            for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
                if (cancelled.get()) throw new IOException("cancelled");
                if (!isAllowedMediaUriWithLocals(current, localAddrs)) {
                    throw new IOException("Blocked unsafe DLNA media URI");
                }
                URI parsed = URI.create(current.trim());
                boolean https = "https".equalsIgnoreCase(parsed.getScheme());
                String host = parsed.getHost();
                if (host == null || host.isEmpty()) throw new IOException("Missing host");
                String cleanHost = host.startsWith("[") && host.endsWith("]")
                        ? host.substring(1, host.length() - 1) : host;
                int defaultPort = https ? 443 : 80;
                int port = parsed.getPort() > 0 ? parsed.getPort() : defaultPort;

                InetAddress targetAddr;
                if (!resolveRedirects && pinnedFinalAddr != null && current.equals(pinnedFinalUri)) {
                    if (!isAllowedInetAddress(pinnedFinalAddr, localAddrs)) {
                        throw new IOException("Pinned address failed policy recheck");
                    }
                    targetAddr = pinnedFinalAddr;
                } else {
                    InetAddress[] resolved = resolver.resolve(cleanHost);
                    if (resolved == null || resolved.length == 0) {
                        throw new IOException("Unable to resolve media host");
                    }
                    boolean privateAddress = isPrivateLanAddress(resolved[0]);
                    for (InetAddress cand : resolved) {
                        if (!isAllowedInetAddress(cand, localAddrs)) {
                            throw new IOException("Media host resolved to restricted/local address");
                        }
                        if (isPrivateLanAddress(cand) != privateAddress) {
                            throw new IOException("Mixed public and private addresses");
                        }
                    }
                    targetAddr = resolved[0];
                    if (previous != null && (privateAddress != isPrivateLanAddress(previous)
                            || (privateAddress && !canonicalIpKey(previous).equals(canonicalIpKey(targetAddr))))) {
                        throw new IOException("Redirect crossed a network boundary or changed private host");
                    }
                    previous = targetAddr;
                }

                String rawPath = parsed.getRawPath();
                if (rawPath == null || rawPath.isEmpty()) rawPath = "/";
                String reqTarget = parsed.getRawQuery() != null ? (rawPath + "?" + parsed.getRawQuery()) : rawPath;
                String hostHdr = (port == defaultPort) ? host : (host + ":" + port);

                Socket sock = connectPinnedSocket(targetAddr, port, https, cleanHost);
                OutputStream out = sock.getOutputStream();
                StringBuilder req = new StringBuilder(256);
                req.append("GET ").append(reqTarget).append(" HTTP/1.1\r\n");
                req.append("Host: ").append(hostHdr).append("\r\n");
                req.append("User-Agent: SoundFlow-DLNA/1.2\r\n");
                req.append("Accept: */*\r\n");
                if (offset > 0L) {
                    req.append("Range: bytes=").append(offset).append("-\r\n");
                }
                req.append("Connection: close\r\n\r\n");
                out.write(req.toString().getBytes(StandardCharsets.US_ASCII));
                out.flush();

                InputStream rawIn = new BufferedInputStream(sock.getInputStream(), 16384);
                String hdrText = readHeaderBlock(sock, rawIn);
                String[] lines = hdrText.split("\r\n");
                if (lines.length == 0) {
                    closeStreamLocked();
                    throw new IOException("Empty HTTP status line");
                }
                String[] statusParts = lines[0].split(" ", 3);
                if (statusParts.length < 2) {
                    closeStreamLocked();
                    throw new IOException("Malformed HTTP status line");
                }
                int status;
                try {
                    status = Integer.parseInt(statusParts[1]);
                } catch (NumberFormatException e) {
                    closeStreamLocked();
                    throw new IOException("Invalid HTTP status code");
                }

                Map<String, String> headers = new HashMap<>();
                for (int i = 1; i < lines.length; i++) {
                    int idx = lines[i].indexOf(':');
                    if (idx > 0) {
                        headers.put(
                                lines[i].substring(0, idx).trim().toLowerCase(Locale.ROOT),
                                lines[i].substring(idx + 1).trim());
                    }
                }

                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    closeStreamLocked();
                    if (!resolveRedirects || hop == MAX_REDIRECTS) {
                        throw new IOException("Too many or unexpected DLNA redirects");
                    }
                    String loc = headers.get("location");
                    if (loc == null || loc.isEmpty()) {
                        throw new IOException("Redirect missing Location");
                    }
                    try {
                        current = parsed.resolve(loc).toString();
                    } catch (IllegalArgumentException e) {
                        throw new IOException("Invalid redirect Location", e);
                    }
                    continue;
                }

                if (status != 200 && status != 206) {
                    closeStreamLocked();
                    throw new IOException("HTTP status " + status);
                }

                long contentLength = -1L;
                try {
                    String declaredLength = headers.get("content-length");
                    if (declaredLength != null) {
                        contentLength = Long.parseLong(declaredLength);
                        if (contentLength < 0 || contentLength > MAX_MEDIA_BYTES) {
                            throw new IOException("Invalid DLNA content length");
                        }
                    }
                    if (status == 206) {
                        Matcher range = CONTENT_RANGE.matcher(headers.getOrDefault("content-range", ""));
                        if (!range.matches()) throw new IOException("Invalid DLNA Content-Range");
                        long start = Long.parseLong(range.group(1));
                        long end = Long.parseLong(range.group(2));
                        if (start != offset || end < start || end >= MAX_MEDIA_BYTES
                                || (contentLength >= 0 && contentLength != end - start + 1)) {
                            throw new IOException("DLNA response does not match the requested range");
                        }
                        if (!"*".equals(range.group(3))) {
                            long total = Long.parseLong(range.group(3));
                            if (total <= end || total > MAX_MEDIA_BYTES || (totalLength >= 0 && total != totalLength)) {
                                throw new IOException("Invalid DLNA media size");
                            }
                            totalLength = total;
                        }
                        contentLength = end - start + 1;
                    } else if (contentLength >= 0) {
                        totalLength = contentLength;
                    }
                } catch (NumberFormatException e) {
                    throw new IOException("Invalid DLNA media size", e);
                }
                streamCursor = status == 206 ? offset : 0L;

                String te = headers.getOrDefault("transfer-encoding", "");
                if (te.toLowerCase(Locale.ROOT).contains("chunked")) {
                    activeStream = new ChunkedBodyInputStream(rawIn);
                } else if (contentLength >= 0L) {
                    activeStream = new BoundedBodyInputStream(rawIn, contentLength);
                } else {
                    activeStream = rawIn;
                }

                pinnedFinalUri = current.trim();
                pinnedFinalAddr = targetAddr;

                // If caller requested offset > 0 but server ignored Range and returned 200 from 0, skip ahead boundedly.
                if (offset > 0L && streamCursor == 0L) {
                    if (offset > 8L * 1024L * 1024L) {
                        closeStreamLocked();
                        throw new IOException("Server does not support HTTP Range for large seek");
                    }
                    byte[] skipBuf = new byte[16384];
                    while (streamCursor < offset) {
                        if (cancelled.get()) throw new IOException("cancelled");
                        int want = (int) Math.min(skipBuf.length, offset - streamCursor);
                        int n = activeStream.read(skipBuf, 0, want);
                        if (n <= 0) {
                            closeStreamLocked();
                            throw new EOFException("EOF while advancing non-Range stream");
                        }
                        streamCursor += n;
                    }
                }
                return;
            }
            throw new IOException("Too many redirects");
        }

        private void cacheTailLocked(long startPos, byte[] data, int off, int len) {
            if (len <= 0) return;
            if (windowBuf.length < MAX_CACHE_WINDOW) {
                windowBuf = new byte[MAX_CACHE_WINDOW];
            }
            if (len >= MAX_CACHE_WINDOW) {
                System.arraycopy(data, off + len - MAX_CACHE_WINDOW, windowBuf, 0, MAX_CACHE_WINDOW);
                windowStart = startPos + len - MAX_CACHE_WINDOW;
                windowLen = MAX_CACHE_WINDOW;
                return;
            }
            if (windowLen > 0 && startPos == windowStart + windowLen) {
                int overflow = (windowLen + len) - MAX_CACHE_WINDOW;
                if (overflow > 0) {
                    System.arraycopy(windowBuf, overflow, windowBuf, 0, windowLen - overflow);
                    windowStart += overflow;
                    windowLen -= overflow;
                }
                System.arraycopy(data, off, windowBuf, windowLen, len);
                windowLen += len;
            } else {
                System.arraycopy(data, off, windowBuf, 0, len);
                windowStart = startPos;
                windowLen = len;
            }
        }

        @Override
        public int readAt(long position, byte[] buffer, int offset, int size) throws IOException {
            if (position < 0L || buffer == null || offset < 0 || size < 0 || offset > buffer.length || size > buffer.length - offset) {
                throw new IllegalArgumentException("Invalid readAt arguments");
            }
            if (size == 0) return 0;
            synchronized (ioLock) {
                if (cancelled.get()) return -1;
                if (position >= MAX_MEDIA_BYTES || (totalLength >= 0L && position >= totalLength)) return -1;

                // Serve from sliding window cache if available (avoids reconnects on parser header re-reads).
                if (windowLen > 0 && position >= windowStart && position < windowStart + windowLen) {
                    int rel = (int) (position - windowStart);
                    int fromCache = Math.min(size, windowLen - rel);
                    System.arraycopy(windowBuf, rel, buffer, offset, fromCache);
                    return fromCache;
                }

                if (activeStream == null || streamCursor != position) {
                    if (activeStream != null && position > streamCursor && position - streamCursor <= 65536L) {
                        byte[] skip = new byte[8192];
                        while (streamCursor < position) {
                            if (cancelled.get()) return -1;
                            int want = (int) Math.min(skip.length, position - streamCursor);
                            int n = activeStream.read(skip, 0, want);
                            if (n <= 0) {
                                closeStreamLocked();
                                return -1;
                            }
                            cacheTailLocked(streamCursor, skip, 0, n);
                            streamCursor += n;
                        }
                    } else {
                        openAtOffsetLocked(position, pinnedFinalAddr == null);
                    }
                }

                int readTotal = 0;
                while (readTotal < size) {
                    if (cancelled.get()) return -1;
                    int allowed = (int) Math.min(size - readTotal, MAX_MEDIA_BYTES - streamCursor);
                    if (totalLength >= 0L) allowed = (int) Math.min(allowed, totalLength - streamCursor);
                    if (allowed <= 0) return -1;
                    int n = activeStream.read(buffer, offset + readTotal, allowed);
                    if (n <= 0) {
                        if (readTotal == 0) {
                            if (totalLength < 0L) totalLength = streamCursor;
                            closeStreamLocked();
                            return -1;
                        }
                        break;
                    }
                    cacheTailLocked(streamCursor, buffer, offset + readTotal, n);
                    streamCursor += n;
                    readTotal += n;
                    // MediaExtractor expects prompt partial reads rather than blocking to fill huge buffers.
                    break;
                }
                return readTotal;
            }
        }

        @Override
        public long getSize() {
            return totalLength;
        }

        @Override
        public void close() {
            cancelNow();
            synchronized (ioLock) {
                closeStreamLocked();
            }
        }
    }

    private static final Map<String, Pattern> SOAP_TAGS;
    static {
        Map<String, Pattern> tags = new HashMap<>();
        for (String tag : new String[]{"CurrentURI", "CurrentURIMetaData", "Target",
                "DesiredVolume", "dc:title", "upnp:artist", "dc:creator", "upnp:album"}) {
            tags.put(tag, xmlPattern(tag));
        }
        SOAP_TAGS = tags;
    }

    private static Pattern xmlPattern(String tag) {
        String prefix = tag.contains(":") ? "" : "(?:[A-Za-z0-9_-]+:)?";
        String quoted = Pattern.quote(tag);
        return Pattern.compile("<" + prefix + quoted + "(?:\\s[^>]*)?>([\\s\\S]*?)</" + prefix + quoted + ">", Pattern.CASE_INSENSITIVE);
    }

    private static String xmlTag(String xml, String tag) {
        Pattern p = SOAP_TAGS.get(tag);
        Matcher m = p.matcher(xml);
        return m.find() ? unescapeXml(m.group(1).trim()) : "";
    }

    private static String unescapeXml(String s) {
        return s.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&quot;", "\"").replace("&apos;", "'");
    }

    private static String escapeXml(String s) {
        if (s == null) return "";
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    }

    private static long parseDlnaTimeMs(String s) {
        String[] p = s.trim().split(":");
        if (p.length != 3) return 0;
        try {
            long h = Long.parseLong(p[0]), m = Long.parseLong(p[1]);
            double sec = Double.parseDouble(p[2]);
            return Math.round(((h * 3600) + (m * 60) + sec) * 1000.0);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String formatDlnaTime(long ms) {
        long sec = Math.max(0, ms / 1000);
        return String.format(Locale.US, "%02d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
    }
}
