package io.soundflow.receiver;

import android.content.Context;
import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.audiofx.Visualizer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.HttpURLConnection;
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
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

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
    // Visualizer exposes mixer data without a presentation timestamp.
    private static final long VISUALIZER_OUTPUT_ESTIMATE_NANOS = 60_000_000L;
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
    private volatile long sampledAt;
    // Both listeners block for the life of the renderer. Keep their threads
    // separate so the bounded HTTP executor can always service its queue.
    private final ExecutorService listeners = Executors.newFixedThreadPool(2);
    private final ExecutorService httpWorkers = new ThreadPoolExecutor(
            2, 4, 30L, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(32),
            new ThreadPoolExecutor.AbortPolicy()
    );

    private volatile boolean running = false;
    private boolean closed;
    private final Set<Socket> openClients = ConcurrentHashMap.newKeySet();
    private volatile boolean playing = false;
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

    public void stopPlayback() {
        playerHandler.post(this::releasePlayer);
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
        playerHandler.post(() -> {
            if (player == null || !mediaPrepared) return;
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
            // stop() cannot race an as-yet-unpublished SSDP socket into a leak.
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

    private void handleHttp(Socket client) {
        try (Socket sock = client) {
            sock.setSoTimeout(5000);
            InputStream in = new BufferedInputStream(sock.getInputStream(), 8192);
            ByteArrayOutputStream hdrBuf = new ByteArrayOutputStream();
            int b, prev = 0, step = 0;
            while ((b = in.read()) != -1) {
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
            int contentLen = 0;
            try {
                if (headers.containsKey("content-length")) {
                    contentLen = Math.max(0, Math.min(262144, Integer.parseInt(headers.get("content-length"))));
                }
            } catch (NumberFormatException ignored) {}
            byte[] bodyBytes = new byte[contentLen];
            int read = 0;
            while (read < contentLen) {
                int n = in.read(bodyBytes, read, contentLen - read);
                if (n <= 0) break;
                read += n;
            }
            String body = new String(bodyBytes, 0, read, StandardCharsets.UTF_8);

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

    private String handleAvTransport(String action, String body) {
        if (action.contains("SetAVTransportURI")) {
            String uri = xmlTag(body, "CurrentURI");
            if (!isAllowedMediaUri(uri, localIp)) {
                listener.onDlnaLog("DLNA 拒绝非 HTTP(S) 或受限目标 URI");
                return soapResp("AVTransport", "SetAVTransportURI", "");
            }
            String meta = unescapeXml(xmlTag(body, "CurrentURIMetaData"));
            playerHandler.post(() -> {
                if (!running) return;
                if (!uri.equals(currentUri)) { title = ""; artist = ""; album = ""; }
                currentUri = uri;
                parseDidlMetadata(meta);
                session = NEXT_SESSION.incrementAndGet();
                listener.onDlnaPreparing(session, title, artist, album);
                listener.onDlnaLog("DLNA SetURI: " + title);
                prepareMedia(uri);
            });
            return soapResp("AVTransport", "SetAVTransportURI", "");
        }
        if (action.contains("Play")) {
            playerHandler.post(() -> {
                if (player != null && mediaPrepared) {
                    try {
                        player.start();
                        playing = true;
                        emitState("PLAYING");
                    } catch (IllegalStateException ignored) {}
                } else if (player == null && !currentUri.isEmpty()) {
                    session = NEXT_SESSION.incrementAndGet();
                    listener.onDlnaPreparing(session, title, artist, album);
                    prepareMedia(currentUri);
                }
            });
            return soapResp("AVTransport", "Play", "");
        }
        if (action.contains("Pause")) {
            togglePauseResume(true);
            return soapResp("AVTransport", "Pause", "");
        }
        if (action.contains("Stop")) {
            playerHandler.post(() -> {
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
            String st = playing ? "PLAYING" : (mediaPrepared ? "PAUSED_PLAYBACK" : "STOPPED");
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

    private void prepareMedia(String uri) {
        if (uri == null || uri.isEmpty()) return;
        if (!running) return;
        releasePlayer();
        String validatedUri = resolveAndValidateMediaUri(uri, localIp);
        if (validatedUri == null) {
            listener.onDlnaError("DLNA 媒体地址未通过局域网安全校验（已拦截受限目标或非法重定向）");
            emitState("STOPPED");
            return;
        }
        try {
            player = new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            player.setDataSource(validatedUri);
            player.setOnPreparedListener(mp -> {
                if (!running || player != mp) { mp.release(); return; }
                audioSessionId = mp.getAudioSessionId();
                mediaPrepared = true;
                mp.start();
                playing = true;
                emitState("PLAYING");
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
                if (player != mp) return true;
                playing = false;
                mediaPrepared = false;
                audioSessionId = 0;
                listener.onDlnaError("DLNA 音频流播放失败 (" + what + "/" + extra + ")");
                emitState("STOPPED");
                return true;
            });
            player.prepareAsync();
        } catch (Throwable e) {
            releasePlayer();
            listener.onDlnaError("DLNA 打开媒体流失败: " + e.getMessage());
            emitState("STOPPED");
        }
    }

    private void releasePlayer() {
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

    private static boolean isAllowedInetAddress(InetAddress addr, String selfIp) {
        if (addr == null) return false;
        if (addr.isLoopbackAddress() || addr.isAnyLocalAddress()
                || addr.isLinkLocalAddress() || addr.isMulticastAddress()) {
            return false;
        }
        String hostAddr = addr.getHostAddress();
        if (hostAddr != null && selfIp != null && !selfIp.isEmpty() && hostAddr.equals(selfIp)) {
            return false;
        }
        if (addr instanceof Inet4Address) {
            byte[] b = addr.getAddress();
            if (b.length == 4 && (b[0] & 0xff) == 255 && (b[1] & 0xff) == 255
                    && (b[2] & 0xff) == 255 && (b[3] & 0xff) == 255) {
                return false;
            }
        } else if (addr instanceof Inet6Address) {
            byte[] b = addr.getAddress();
            // Reject IPv4-mapped loopback/link-local/unspecified ::ffff:a.b.c.d
            if (b.length == 16) {
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
            }
        }
        return true;
    }

    static boolean isAllowedMediaUri(String raw, String selfIp) {
        if (raw == null || raw.isEmpty() || raw.length() > 4096) return false;
        String trimmed = raw.trim();
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (c <= 0x20 || c == 0x7f) return false;
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
                    || "::1".equals(lower) || "0:0:0:0:0:0:0:1".equals(lower)
                    || "::".equals(lower) || "0.0.0.0".equals(lower)
                    || "255.255.255.255".equals(lower)
                    || lower.startsWith("127.") || lower.startsWith("169.254.")
                    || lower.startsWith("fe80:") || lower.startsWith("::ffff:127.")
                    || lower.startsWith("::ffff:169.254.") || lower.startsWith("::ffff:0.")
                    || (selfIp != null && !selfIp.isEmpty() && selfIp.equals(lower))) {
                return false;
            }
            if (u.getPort() == 0) return false;
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    private static String resolveAndValidateMediaUri(String initialUri, String selfIp) {
        String current = initialUri;
        for (int hop = 0; hop <= 3; hop++) {
            if (!isAllowedMediaUri(current, selfIp)) return null;
            HttpURLConnection conn = null;
            try {
                URI parsed = URI.create(current.trim());
                String host = parsed.getHost();
                if (host == null) return null;
                String lookupHost = host.startsWith("[") && host.endsWith("]")
                        ? host.substring(1, host.length() - 1) : host;
                InetAddress[] resolved = InetAddress.getAllByName(lookupHost);
                if (resolved == null || resolved.length == 0) return null;
                for (InetAddress addr : resolved) {
                    if (!isAllowedInetAddress(addr, selfIp)) return null;
                }
                conn = (HttpURLConnection) new URL(current.trim()).openConnection();
                conn.setInstanceFollowRedirects(false);
                conn.setRequestMethod("GET");
                conn.setRequestProperty("Range", "bytes=0-0");
                conn.setConnectTimeout(4000);
                conn.setReadTimeout(4000);
                int code = conn.getResponseCode();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    if (hop == 3) return null;
                    String loc = conn.getHeaderField("Location");
                    if (loc == null || loc.trim().isEmpty()) return null;
                    current = parsed.resolve(loc.trim()).toString();
                    continue;
                }
                return current.trim();
            } catch (Exception e) {
                return null;
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
        return null;
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
