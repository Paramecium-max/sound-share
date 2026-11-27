package io.soundflow.receiver;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Base64;
import android.webkit.WebView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Offline-first lyric orchestrator:
 * 1. Explicit embedded LRC pushed over LAN (0 internet)
 * 2. Local persistent offline .lrc store (user-imported / previously cached / LAN-recorded)
 * 3. Xiaomi real-time LAN line stream (0 internet, also auto-recorded locally for future offline plays)
 * 4. Optional online LRC assist (only when user enables autoOnlineLookup, saved offline forever)
 */
public final class LyricOrchestrator {
    public static final class LrcLine {
        public final long startTimeMs;
        public final String text;

        public LrcLine(long startTimeMs, String text) {
            this.startTimeMs = startTimeMs;
            this.text = text;
        }
    }

    private static final Pattern LRC_TAG = Pattern.compile("\\[(\\d{1,3}):(\\d{2})(?:[.:](\\d{1,3}))?\\]");

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService netPool = Executors.newSingleThreadExecutor();
    private final ExecutorService lyricFiles = Executors.newSingleThreadExecutor();
    private final Map<String, List<LrcLine>> lrcCache = new ConcurrentHashMap<>();
    private final List<LrcLine> lanRecordedLines = new ArrayList<>();

    private File cacheDir;
    private WebView webView;
    private boolean pageReady = false;
    private boolean playing = false;
    private boolean uiVisible = true;
    private long playbackPositionMs;
    private long positionAnchorElapsedMs;
    private long lastSenderPositionMs = -1;
    private boolean useSenderClock;
    private String currentSongKey = "";
    private String currentSongName = "";
    private String currentArtistName = "";
    private long currentDurationMs = 0;
    private String lastRealtimeLine = "";
    private String lastLrcPayload = "";
    private String lastArtworkSignature = "";
    private String rememberedTrackTitle = "";
    private String rememberedTrackAlbum = "";
    private long rememberedTrackDurSec = -1;
    private long songStartElapsedMs = 0;
    private boolean hasFullLrcForCurrentSong = false;
    private volatile boolean autoOnlineEnabled = false;
    private float lastTickPeak = -1f;
    private int lastTickSpectrum = -1;
    private long lastTickPosMs = -1L;
    private long lastMediaSeq = -1;
    private volatile long lookupTicket = 0;
    private final AtomicReference<HttpURLConnection> activeLookup = new AtomicReference<>();
    private double webClockBias = Double.NaN;
    private double webClockRtt = Double.POSITIVE_INFINITY;
    private int webClockEpoch;
    private boolean destroyed;
    private long offlineTicket;
    private String queuedJs = null;

    public void attachContext(Context ctx) {
        if (ctx != null) {
            cacheDir = new File(ctx.getApplicationContext().getFilesDir(), "lyrics_cache");
            if (!cacheDir.exists()) cacheDir.mkdirs();
            // v1.0 auto-recorded fragments were incorrectly treated as full LRCs.
            // Preserve those records, but remove them from the timed-lyric namespace.
            File[] legacy = cacheDir.listFiles((dir, name) -> name.endsWith("_lanrec.lrc"));
            if (legacy != null) for (File f : legacy) {
                f.renameTo(new File(cacheDir, f.getName().replace("_lanrec.lrc", "_lanrec.fragment")));
            }
        }
    }

    public void attachWebView(WebView view) {
        this.webView = view;
    }

    public int getOfflineCacheCount() {
        if (cacheDir == null || !cacheDir.exists()) return 0;
        File[] files = cacheDir.listFiles((dir, name) -> name.endsWith(".lrc") && !name.endsWith("_lanrec.lrc"));
        return files == null ? 0 : files.length;
    }

    public int clearOfflineCache() {
        lrcCache.clear();
        int removed = 0;
        if (cacheDir != null && cacheDir.exists()) {
            File[] files = cacheDir.listFiles((dir, name) -> name.endsWith(".lrc"));
            if (files != null) {
                for (File f : files) {
                    if (f.delete()) removed++;
                }
            }
        }
        if (!autoOnlineEnabled) {
            hasFullLrcForCurrentSong = false;
            eval("window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine("
                    + JSONObject.quote(lastRealtimeLine) + ",true);");
        }
        return removed;
    }

    private void invalidateOnlineLookup() {
        lookupTicket++;
        HttpURLConnection pending = activeLookup.getAndSet(null);
        if (pending != null) lyricFiles.execute(pending::disconnect);
    }

    private boolean lookupIsCurrent(long ticket) {
        return autoOnlineEnabled && ticket == lookupTicket;
    }

    public void onOnlineLookupSettingChanged(boolean enabled) {
        this.autoOnlineEnabled = enabled;
        invalidateOnlineLookup();
        if (enabled && playing && !hasFullLrcForCurrentSong && !currentSongName.isEmpty()) {
            fetchOnlineLrcAsync(lookupTicket, currentSongKey, currentSongName, currentArtistName, currentDurationMs);
        }
    }

    public void markPageReady() {
        this.pageReady = true;
        startWebClockCalibration();
        eval("window.SoundFlowStage && window.SoundFlowStage.setPlaying(" + (playing && uiVisible) + ")");
        if (queuedJs != null) {
            eval(queuedJs);
            queuedJs = null;
        }
    }

    public void setPlayingState(boolean isPlaying) {
        if (playing != isPlaying) {
            playbackPositionMs = currentPlaybackPosition();
            positionAnchorElapsedMs = SystemClock.elapsedRealtime();
        }
        this.playing = isPlaying;
        eval("window.SoundFlowStage && window.SoundFlowStage.setPlaying(" + (isPlaying && uiVisible) + ")");
    }

    public void setUiVisible(boolean visible) {
        uiVisible = visible;
        if (visible) startWebClockCalibration();
        else webClockEpoch++;
        eval("window.SoundFlowStage && window.SoundFlowStage.setPlaying(" + (playing && visible) + ")");
    }

    private long currentPlaybackPosition() {
        long delta = playing && positionAnchorElapsedMs > 0
                ? Math.max(0, SystemClock.elapsedRealtime() - positionAnchorElapsedMs) : 0;
        long position = playbackPositionMs + delta;
        return currentDurationMs > 0 ? Math.min(currentDurationMs, position) : position;
    }

    private void startWebClockCalibration() {
        if (!uiVisible || !pageReady || webView == null) return;
        webClockRtt = Double.POSITIVE_INFINITY;
        calibrateWebClock(++webClockEpoch, 3);
    }

    private void calibrateWebClock(int epoch, int attempts) {
        if (attempts == 0 || epoch != webClockEpoch || webView == null) return;
        double start = System.nanoTime() / 1e6;
        webView.evaluateJavascript("performance.now()", value -> {
            if (epoch != webClockEpoch) return;
            double end = System.nanoTime() / 1e6;
            try {
                double sample = Double.parseDouble(value);
                if (Double.isFinite(sample) && end - start < webClockRtt) {
                    webClockBias = sample - (start + end) * 0.5;
                    webClockRtt = end - start;
                }
            } catch (NumberFormatException | NullPointerException ignored) {}
            calibrateWebClock(epoch, attempts - 1);
        });
    }

    public void updateAudioSample(String events, int spectrum, boolean hasMeter, long positionMs, long generation) {
        if (!playing || !uiVisible || !pageReady || webView == null) return;
        String meter = hasMeter ? audioMeterArguments(((spectrum >>> 24) & 0xff) / 255f, positionMs, spectrum) : null;
        if (events == null && meter == null) return;
        String bias = Double.isFinite(webClockBias) && webClockRtt <= 16 ? Double.toString(webClockBias) : "null";
        webView.evaluateJavascript("window.SoundFlowStage && window.SoundFlowStage.onAudioSample("
                + generation + "," + (events == null ? "null" : events) + "," + bias + ","
                + System.nanoTime() / 1e6 + "," + (meter == null ? "null" : "[" + meter + "]") + ")", null);
    }

    public void updateAudioEnergy(float peak, long positionMs, int spectrumPacked) {
        if (!playing || !uiVisible || !pageReady || webView == null) return;
        String meter = audioMeterArguments(peak, positionMs, spectrumPacked);
        if (meter != null) eval("window.SoundFlowStage && window.SoundFlowStage.onTick(" + meter + ")");
    }

    private String audioMeterArguments(float peak, long positionMs, int spectrumPacked) {
        long posArg = -1L;
        if (hasFullLrcForCurrentSong && positionMs > 0
                && Math.abs(positionMs - currentPlaybackPosition()) >= 320L) {
            playbackPositionMs = positionMs;
            positionAnchorElapsedMs = SystemClock.elapsedRealtime();
            posArg = positionMs;
        }
        if (Math.abs(peak - lastTickPeak) < 0.025f
                && spectrumPacked == lastTickSpectrum
                && posArg < 0) {
            return null;
        }
        lastTickPeak = peak;
        lastTickSpectrum = spectrumPacked;
        if (posArg >= 0) lastTickPosMs = posArg;
        float bass = ((spectrumPacked >>> 16) & 0xff) / 255f;
        float mid = ((spectrumPacked >>> 8) & 0xff) / 255f;
        float high = (spectrumPacked & 0xff) / 255f;
        return peak + "," + posArg + "," + bass + "," + mid + "," + high;
    }

    public void onMediaUpdate(
            long mediaSeq,
            String rawTitle,
            String rawArtist,
            String rawAlbum,
            String explicitLrc,
            String explicitLyricLine,
            String artworkBase64,
            String format,
            String protocol,
            long durationMs,
            long positionMs,
            boolean autoOnlineLookup
    ) {
        if (this.autoOnlineEnabled != autoOnlineLookup) {
            this.autoOnlineEnabled = autoOnlineLookup;
            invalidateOnlineLookup();
        }
        useSenderClock = "dlna".equals(protocol) || positionMs > 0;
        if (positionMs >= 0 && positionMs != lastSenderPositionMs) {
            playbackPositionMs = positionMs;
            lastSenderPositionMs = positionMs;
            positionAnchorElapsedMs = SystemClock.elapsedRealtime();
        }
        if (hasFullLrcForCurrentSong && positionMs > 0) {
            evalOrQueue("window.SoundFlowStage && window.SoundFlowStage.syncPosition(" + positionMs + ",false);");
        }
        if (mediaSeq == lastMediaSeq && mediaSeq != 0) return;
        lastMediaSeq = mediaSeq;

        String cleanTitle = safeTrim(rawTitle);
        String cleanArtist = safeTrim(rawArtist);
        String cleanAlbum = safeTrim(rawAlbum);
        long durSec = durationMs / 1000L;
        if (cleanTitle.isEmpty() && cleanArtist.isEmpty() && cleanAlbum.isEmpty()) return;

        String songName;
        String artistName;
        String activeLine = !isEmpty(explicitLyricLine) ? explicitLyricLine.trim() : "";

        // Xiaomi players commonly push the active lyric sentence in `title` while
        // encoding `<SongTitle>-<ArtistName>` in `artist` and/or keeping song name in `album`.
        int dashSplit = "xiaomi".equals(protocol) ? findSongArtistDash(cleanArtist) : -1;
        if (!rememberedTrackTitle.isEmpty() && cleanArtist.startsWith(rememberedTrackTitle + "-")) {
            songName = rememberedTrackTitle;
            artistName = cleanArtist.substring(rememberedTrackTitle.length() + 1).trim();
            if (activeLine.isEmpty() && !cleanTitle.equalsIgnoreCase(songName)) activeLine = cleanTitle;
        } else if (!cleanAlbum.isEmpty() && cleanArtist.startsWith(cleanAlbum + "-")) {
            songName = cleanAlbum;
            artistName = cleanArtist.substring(cleanAlbum.length() + 1).trim();
            if (activeLine.isEmpty() && !cleanTitle.equalsIgnoreCase(songName)) activeLine = cleanTitle;
        } else if (dashSplit > 0 && !cleanTitle.equalsIgnoreCase(cleanArtist.substring(0, dashSplit).trim())) {
            songName = cleanArtist.substring(0, dashSplit).trim();
            artistName = cleanArtist.substring(dashSplit + 1).trim();
            rememberedTrackTitle = songName;
            rememberedTrackAlbum = cleanAlbum;
            rememberedTrackDurSec = durSec;
            if (activeLine.isEmpty()) activeLine = cleanTitle;
        } else if ("xiaomi".equals(protocol)
                && !rememberedTrackTitle.isEmpty()
                && durSec == rememberedTrackDurSec
                && cleanAlbum.equals(rememberedTrackAlbum)
                && !cleanTitle.isEmpty()
                && !cleanTitle.equalsIgnoreCase(rememberedTrackTitle)) {
            songName = rememberedTrackTitle;
            artistName = cleanArtist;
            if (activeLine.isEmpty()) activeLine = cleanTitle;
        } else {
            rememberedTrackTitle = !cleanTitle.isEmpty() ? cleanTitle : cleanAlbum;
            rememberedTrackAlbum = cleanAlbum;
            rememberedTrackDurSec = durSec;
            songName = rememberedTrackTitle;
            artistName = cleanArtist;
        }

        // Duration commonly changes from unknown/estimated to exact in DLNA.
        // That is a progress update, not a new song or a reason to rebuild lyrics.
        String newSongKey = songName + "||" + artistName;
        boolean songChanged = !newSongKey.equals(currentSongKey);

        String codecLabel = format == null || format.isEmpty() ? ("dlna".equals(protocol) ? "DLNA Audio" : "AAC 48kHz") : format;
        String defaultModeLabel = "dlna".equals(protocol) ? "纯离线 · DLNA 音频" : "纯离线 · 局域网实时";

        if (songChanged) {
            flushLanRecordedToDisk();
            invalidateOnlineLookup();
            offlineTicket++;
            currentSongKey = newSongKey;
            currentSongName = songName;
            currentArtistName = artistName;
            currentDurationMs = durationMs;
            playbackPositionMs = Math.max(0, positionMs);
            lastSenderPositionMs = positionMs;
            positionAnchorElapsedMs = SystemClock.elapsedRealtime();
            songStartElapsedMs = SystemClock.elapsedRealtime();
            lanRecordedLines.clear();
            lastRealtimeLine = "";
            lastLrcPayload = "";
            lastArtworkSignature = "";
            hasFullLrcForCurrentSong = false;

            String hdrCall = "window.SoundFlowStage && window.SoundFlowStage.setTrackHeader("
                    + JSONObject.quote(songName) + ","
                    + JSONObject.quote(artistName.isEmpty() ? "局域网音频投送" : artistName) + ","
                    + JSONObject.quote(codecLabel) + ","
                    + JSONObject.quote(defaultModeLabel) + ");"
                    + "window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine('',true);";
            evalOrQueue(hdrCall);
        } else {
            if (durationMs > 0) currentDurationMs = durationMs;
            evalOrQueue("window.SoundFlowStage && window.SoundFlowStage.setTrackHeader("
                    + JSONObject.quote(songName) + ","
                    + JSONObject.quote(artistName.isEmpty() ? "局域网音频投送" : artistName) + ","
                    + JSONObject.quote(codecLabel) + ",null);");
        }

        if (artworkBase64 != null && artworkBase64.length() > 64) {
            String sig = artworkBase64.length() + ":" + artworkBase64.substring(0, 32);
            if (!sig.equals(lastArtworkSignature)) {
                lastArtworkSignature = sig;
                evalOrQueue("window.SoundFlowStage && window.SoundFlowStage.setArtworkBase64("
                        + JSONObject.quote(artworkBase64) + ");");
            }
        }

        boolean newEmbedded = !isEmpty(explicitLrc) && !explicitLrc.equals(lastLrcPayload);
        if (newEmbedded) lastLrcPayload = explicitLrc;
        if (newEmbedded || songChanged) {
            loadTrackLyrics(songName, artistName, newEmbedded ? explicitLrc : "", newSongKey);
        }

        // Active live lyric sentence (Xiaomi LAN realtime push — works instantaneously offline)
        if (!activeLine.isEmpty() && !activeLine.equals(lastRealtimeLine)) {
            lastRealtimeLine = activeLine;
            recordLanLine(activeLine);
            if (!hasFullLrcForCurrentSong || !useSenderClock) {
                evalOrQueue("window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine("
                        + JSONObject.quote(activeLine) + ",false);");
            }
        } else if (songChanged && activeLine.isEmpty() && !hasFullLrcForCurrentSong) {
            evalOrQueue("window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine("
                    + JSONObject.quote("dlna".equals(protocol) ? "此投送未附带歌词" : "等待发送端同步歌词") + ",false);");
        }

    }

    private void loadTrackLyrics(String song, String artist, String embedded, String songKey) {
        final long revision = ++offlineTicket;
        lyricFiles.execute(() -> {
            List<LrcLine> lines = isEmpty(embedded) ? Collections.emptyList() : parseLrc(embedded);
            OfflineHit hit;
            if (!lines.isEmpty()) {
                saveDiskLrc(song, artist, "embedded", formatLrc(lines));
                hit = new OfflineHit(lines, "纯离线 · 投送内嵌 LRC");
            } else {
                hit = loadOfflineDiskLrc(song, artist);
            }
            main.post(() -> {
                if (destroyed || revision != offlineTicket || !songKey.equals(currentSongKey)) return;
                if (hit != null) {
                    pushFullLyricsToStage(hit.lines, currentPlaybackPosition(), hit.badgeLabel);
                } else if (autoOnlineEnabled && !hasFullLrcForCurrentSong && !song.isEmpty()) {
                    fetchOnlineLrcAsync(lookupTicket, songKey, song, artist, currentDurationMs);
                }
            });
        });
    }

    public boolean importLocalLrcForCurrentTrack(String rawContent) {
        if (rawContent == null || rawContent.trim().isEmpty()) return false;
        List<LrcLine> parsed = parseLrc(rawContent);
        if (parsed.isEmpty()) {
            // Fallback for plain text lyric files (auto-timed every 3.5s)
            parsed = parsePlainTextAsLrc(rawContent);
        }
        if (parsed.isEmpty()) return false;
        String song = !currentSongName.isEmpty() ? currentSongName : "离线导入歌词";
        String artist = currentArtistName;
        String serialized = formatLrc(parsed);
        offlineTicket++;
        invalidateOnlineLookup();
        lyricFiles.execute(() -> saveDiskLrc(song, artist, "imported", serialized));
        lrcCache.put(currentSongKey, parsed);
        pushFullLyricsToStage(parsed, currentPlaybackPosition(), "纯离线 · 本地导入 LRC");
        return true;
    }

    public void clearStage() {
        flushLanRecordedToDisk();
        invalidateOnlineLookup();
        offlineTicket++;
        currentSongKey = "";
        currentSongName = "";
        currentArtistName = "";
        currentDurationMs = 0;
        lastRealtimeLine = "";
        lastLrcPayload = "";
        lastArtworkSignature = "";
        rememberedTrackTitle = "";
        rememberedTrackAlbum = "";
        rememberedTrackDurSec = -1;
        hasFullLrcForCurrentSong = false;
        lanRecordedLines.clear();
        setPlayingState(false);
        eval("window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine('',true);");
    }

    private void recordLanLine(String text) {
        if (currentSongName.isEmpty() || text.isEmpty()) return;
        if (text.startsWith("词：") || text.startsWith("曲：") || text.startsWith("编曲：")) return;
        long relMs = currentPlaybackPosition();
        if (!lanRecordedLines.isEmpty() && text.equals(lanRecordedLines.get(lanRecordedLines.size() - 1).text)) return;
        lanRecordedLines.add(new LrcLine(relMs, text));
        if (lanRecordedLines.size() >= 6 && lanRecordedLines.size() % 4 == 0) {
            flushLanRecordedToDisk();
        }
    }

    private void flushLanRecordedToDisk() {
        if (cacheDir == null || currentSongName.isEmpty() || lanRecordedLines.size() < 6) return;
        final String song = currentSongName;
        final String artist = currentArtistName;
        final String body = formatLrc(new ArrayList<>(lanRecordedLines));
        if (netPool.isShutdown()) return;
        try {
            netPool.execute(() -> {
                // Never overwrite curated/imported/online cached LRCs with auto-recorded stream
                if (hasDiskVariant(song, artist, "imported")
                        || hasDiskVariant(song, artist, "embedded")
                        || hasDiskVariant(song, artist, "cached")) {
                    return;
                }
                saveDiskLrc(song, artist, "lanrec", body);
            });
        } catch (Throwable ignored) {}
    }

    private static final class OfflineHit {
        final List<LrcLine> lines;
        final String badgeLabel;

        OfflineHit(List<LrcLine> lines, String badgeLabel) {
            this.lines = lines;
            this.badgeLabel = badgeLabel;
        }
    }

    private OfflineHit loadOfflineDiskLrc(String song, String artist) {
        String[][] order = {
                {"imported", "纯离线 · 本地导入 LRC"},
                {"embedded", "纯离线 · 内嵌已缓存 LRC"},
                {"cached", "纯离线 · 已缓存完整 LRC"}
        };
        for (String[] pair : order) {
            String raw = readDiskVariant(song, artist, pair[0]);
            if (raw != null) {
                List<LrcLine> parsed = parseLrc(raw);
                if (parsed.size() >= 3) return new OfflineHit(parsed, pair[1]);
            }
        }
        return null;
    }

    private boolean hasDiskVariant(String song, String artist, String tag) {
        return readDiskVariant(song, artist, tag) != null;
    }

    private String readDiskVariant(String song, String artist, String tag) {
        if (cacheDir == null || !cacheDir.exists()) return null;
        File exact = new File(cacheDir, hashStem(song, artist) + "_" + tag + ".lrc");
        File songOnly = new File(cacheDir, hashStem(song, "") + "_" + tag + ".lrc");
        File chosen = exact.exists() ? exact : (songOnly.exists() ? songOnly : null);
        if (chosen == null) return null;
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(chosen), StandardCharsets.UTF_8))) {
            StringBuilder sb = new StringBuilder();
            String ln;
            while ((ln = br.readLine()) != null) sb.append(ln).append('\n');
            return sb.toString();
        } catch (Throwable ignored) {
            return null;
        }
    }

    private void saveDiskLrc(String song, String artist, String tag, String lrcBody) {
        if (cacheDir == null || song == null || song.isEmpty() || lrcBody == null || lrcBody.isEmpty()) return;
        if (!cacheDir.exists()) cacheDir.mkdirs();
        // A LAN line stream proves neither full-song coverage nor original timing.
        // It must never be loaded as a complete synchronized lyric file.
        File target = new File(cacheDir, hashStem(song, artist) + "_" + tag + ("lanrec".equals(tag) ? ".fragment" : ".lrc"));
        try (FileOutputStream fos = new FileOutputStream(target)) {
            fos.write(lrcBody.getBytes(StandardCharsets.UTF_8));
        } catch (Throwable ignored) {}
    }

    private static String hashStem(String song, String artist) {
        String normSong = (song == null ? "" : song).replaceAll("[\\s\\p{P}]", "").toLowerCase();
        String normArtist = (artist == null ? "" : artist).replaceAll("[\\s\\p{P}]", "").toLowerCase();
        String input = normSong + "|" + normArtist;
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] dig = md.digest(input.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(24);
            for (int i = 0; i < 12 && i < dig.length; i++) {
                hex.append(String.format("%02x", dig[i] & 0xff));
            }
            return hex.toString();
        } catch (Throwable ignored) {
            return Integer.toHexString(input.hashCode());
        }
    }

    private static String formatLrc(List<LrcLine> lines) {
        StringBuilder sb = new StringBuilder();
        for (LrcLine ln : lines) {
            long ms = Math.max(0, ln.startTimeMs);
            long min = ms / 60_000L;
            long sec = (ms % 60_000L) / 1000L;
            long cent = (ms % 1000L) / 10L;
            sb.append(String.format("[%02d:%02d.%02d]%s\n", min, sec, cent, ln.text));
        }
        return sb.toString();
    }

    private static List<LrcLine> parsePlainTextAsLrc(String raw) {
        List<LrcLine> out = new ArrayList<>();
        long t = 0;
        for (String line : raw.split("\\r?\\n")) {
            String trimmed = line.trim();
            if (!trimmed.isEmpty()) {
                out.add(new LrcLine(t, trimmed));
                t += 3500;
            }
        }
        return out;
    }

    private static int findSongArtistDash(String artist) {
        if (artist == null) return -1;
        int idx = artist.lastIndexOf('-');
        if (idx >= 2 && idx + 2 < artist.length()) {
            String left = artist.substring(0, idx).trim();
            String right = artist.substring(idx + 1).trim();
            if (left.length() >= 2 && right.length() >= 2) return idx;
        }
        return -1;
    }

    public void destroy() {
        invalidateOnlineLookup();
        lyricFiles.shutdown();
        destroyed = true;
        webClockEpoch++;
        flushLanRecordedToDisk();
        netPool.shutdownNow();
        main.removeCallbacksAndMessages(null);
        webView = null;
    }

    private void pushFullLyricsToStage(List<LrcLine> lines, long posMs, String modeLabel) {
        hasFullLrcForCurrentSong = true;
        JSONArray arr = new JSONArray();
        try {
            for (int i = 0; i < lines.size(); i++) {
                LrcLine cur = lines.get(i);
                long end = (i + 1 < lines.size()) ? Math.max(cur.startTimeMs + 250, lines.get(i + 1).startTimeMs) : cur.startTimeMs + 5000;
                JSONObject o = new JSONObject();
                o.put("startTime", cur.startTimeMs);
                o.put("endTime", end);
                o.put("text", cur.text);
                arr.put(o);
            }
        } catch (Throwable ignored) {}
        String js = "window.SoundFlowStage && window.SoundFlowStage.setFullLyrics("
                + JSONObject.quote(arr.toString()) + "," + posMs + "," + JSONObject.quote(modeLabel) + ");";
        if (!lastRealtimeLine.isEmpty() && !useSenderClock) {
            js += "window.SoundFlowStage && window.SoundFlowStage.pushRealtimeLine("
                    + JSONObject.quote(lastRealtimeLine) + ",false);";
        }
        evalOrQueue(js);
    }

    private void fetchOnlineLrcAsync(long expectedTicket, String expectedKey, String songName, String artistName, long durationMs) {
        List<LrcLine> cached = lrcCache.get(expectedKey);
        if (cached != null && !cached.isEmpty()) {
            main.post(() -> {
                if (lookupIsCurrent(expectedTicket) && expectedKey.equals(currentSongKey)) pushFullLyricsToStage(cached, currentPlaybackPosition(), "纯离线 · 已缓存完整 LRC");
            });
            return;
        }
        netPool.execute(() -> {
            try {
                if (!lookupIsCurrent(expectedTicket)) return;
                String cleanSong = songName.replaceAll("[（(][^）)]*[）)]", "").trim();
                if (cleanSong.isEmpty()) cleanSong = songName.trim();
                String cleanArtist = artistName.replaceAll("[&/、].*$", "").trim();

                List<LrcLine> parsed = queryLrcLibExact(expectedTicket, songName, cleanArtist, durationMs);
                if (parsed.size() < 3 && lookupIsCurrent(expectedTicket)) parsed = queryLrcLibSearch(expectedTicket, cleanSong, cleanArtist);
                if (parsed.size() < 3 && lookupIsCurrent(expectedTicket)) parsed = queryKugouLrc(expectedTicket, cleanSong, cleanArtist);
                if (parsed.size() < 3 && lookupIsCurrent(expectedTicket)) parsed = queryNeteaseLrc(expectedTicket, cleanSong, cleanArtist);
                if (parsed.size() < 3 || !lookupIsCurrent(expectedTicket)) return;

                final List<LrcLine> finalLines = parsed;
                saveDiskLrc(songName, artistName, "cached", formatLrc(finalLines));
                lrcCache.put(expectedKey, finalLines);
                main.post(() -> {
                    if (lookupIsCurrent(expectedTicket) && expectedKey.equals(currentSongKey) && playing) {
                        pushFullLyricsToStage(finalLines, currentPlaybackPosition(), "联网已缓存 LRC");
                    }
                });
            } catch (Throwable ignored) {}
        });
    }

    private String httpGetString(long ticket, String url, String referer) {
        if (!lookupIsCurrent(ticket)) return null;
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            activeLookup.set(conn);
            if (!lookupIsCurrent(ticket)) return null;
            conn.setConnectTimeout(4000);
            conn.setReadTimeout(4000);
            conn.setRequestProperty("User-Agent", "Mozilla/5.0 (SoundFlowReceiver/1.0)");
            if (referer != null) conn.setRequestProperty("Referer", referer);
            if (conn.getResponseCode() != 200) return null;
            StringBuilder sb = new StringBuilder();
            try (BufferedReader br = new BufferedReader(new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                String ln;
                while (lookupIsCurrent(ticket) && (ln = br.readLine()) != null) {
                    sb.append(ln).append('\n');
                }
            }
            return sb.toString();
        } catch (Throwable ignored) {
            return null;
        } finally {
            if (conn != null) {
                activeLookup.compareAndSet(conn, null);
                conn.disconnect();
            }
        }
    }

    private List<LrcLine> pickBestCandidate(List<String> rawCandidates) {
        String hint = lastRealtimeLine.replaceAll("[\\s\\p{P}]", "").toLowerCase();
        List<LrcLine> firstGood = Collections.emptyList();
        for (String raw : rawCandidates) {
            List<LrcLine> parsed = parseLrc(raw);
            if (parsed.size() < 3) continue;
            if (firstGood.isEmpty()) firstGood = parsed;
            if (hint.length() >= 2) {
                for (LrcLine ln : parsed) {
                    String norm = ln.text.replaceAll("[\\s\\p{P}]", "").toLowerCase();
                    if (!norm.isEmpty() && (norm.contains(hint) || hint.contains(norm))) {
                        return parsed;
                    }
                }
            } else {
                return parsed;
            }
        }
        return firstGood;
    }

    private List<LrcLine> queryLrcLibExact(long ticket, String song, String artist, long durationMs) throws Exception {
        StringBuilder q = new StringBuilder("https://lrclib.net/api/get?track_name=")
                .append(URLEncoder.encode(song, "UTF-8"));
        if (!artist.isEmpty()) q.append("&artist_name=").append(URLEncoder.encode(artist, "UTF-8"));
        if (durationMs > 10000) q.append("&duration=").append(durationMs / 1000L);
        String body = httpGetString(ticket, q.toString(), null);
        if (body == null) return Collections.emptyList();
        JSONObject json = new JSONObject(body);
        String synced = safeTrim(json.optString("syncedLyrics", ""));
        return synced.isEmpty() ? Collections.emptyList() : parseLrc(synced);
    }

    private List<LrcLine> queryLrcLibSearch(long ticket, String song, String artist) throws Exception {
        String kw = artist.isEmpty() ? song : (song + " " + artist);
        String body = httpGetString(ticket, "https://lrclib.net/api/search?q=" + URLEncoder.encode(kw, "UTF-8"), null);
        if (body == null) return Collections.emptyList();
        JSONArray arr = new JSONArray(body);
        List<String> cands = new ArrayList<>();
        for (int i = 0; i < Math.min(5, arr.length()); i++) {
            String s = safeTrim(arr.getJSONObject(i).optString("syncedLyrics", ""));
            if (!s.isEmpty()) cands.add(s);
        }
        return pickBestCandidate(cands);
    }

    private List<LrcLine> queryKugouLrc(long ticket, String song, String artist) throws Exception {
        String kw = artist.isEmpty() ? song : (song + " " + artist);
        String body = httpGetString(ticket, "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword="
                + URLEncoder.encode(kw, "UTF-8") + "&duration=&hash=", null);
        if (body == null) return Collections.emptyList();
        JSONArray arr = new JSONObject(body).optJSONArray("candidates");
        if (arr == null || arr.length() == 0) return Collections.emptyList();
        List<String> cands = new ArrayList<>();
        for (int i = 0; i < Math.min(3, arr.length()); i++) {
            JSONObject c = arr.getJSONObject(i);
            String id = c.optString("id", "");
            String key = c.optString("accesskey", "");
            if (id.isEmpty() || key.isEmpty()) continue;
            String dl = httpGetString(ticket, "https://lyrics.kugou.com/download?ver=1&client=pc&id="
                    + URLEncoder.encode(id, "UTF-8")
                    + "&accesskey=" + URLEncoder.encode(key, "UTF-8")
                    + "&fmt=lrc&charset=utf8", null);
            if (dl == null) continue;
            String content = new JSONObject(dl).optString("content", "");
            if (!content.isEmpty()) {
                cands.add(new String(Base64.decode(content, Base64.DEFAULT), StandardCharsets.UTF_8));
            }
        }
        return pickBestCandidate(cands);
    }

    private List<LrcLine> queryNeteaseLrc(long ticket, String song, String artist) throws Exception {
        String kw = artist.isEmpty() ? song : (song + " " + artist);
        String body = httpGetString(ticket, "https://music.163.com/api/search/get/web?s="
                + URLEncoder.encode(kw, "UTF-8") + "&type=1&limit=3", "https://music.163.com/");
        if (body == null) return Collections.emptyList();
        JSONObject res = new JSONObject(body).optJSONObject("result");
        JSONArray songs = res == null ? null : res.optJSONArray("songs");
        if (songs == null || songs.length() == 0) return Collections.emptyList();
        List<String> cands = new ArrayList<>();
        for (int i = 0; i < Math.min(3, songs.length()); i++) {
            long id = songs.getJSONObject(i).optLong("id", 0);
            if (id <= 0) continue;
            String lrcResp = httpGetString(ticket, "https://music.163.com/api/song/lyric?id=" + id + "&lv=1&tv=-1",
                    "https://music.163.com/");
            if (lrcResp == null) continue;
            JSONObject lrcObj = new JSONObject(lrcResp).optJSONObject("lrc");
            if (lrcObj != null) {
                String txt = safeTrim(lrcObj.optString("lyric", ""));
                if (!txt.isEmpty()) cands.add(txt);
            }
        }
        return pickBestCandidate(cands);
    }

    public static List<LrcLine> parseLrc(String raw) {
        List<LrcLine> out = new ArrayList<>();
        if (raw == null || raw.isEmpty()) return out;
        for (String line : raw.split("\\r?\\n")) {
            Matcher m = LRC_TAG.matcher(line);
            List<Long> stamps = new ArrayList<>();
            int lastEnd = 0;
            while (m.find()) {
                long min = Long.parseLong(m.group(1));
                long sec = Long.parseLong(m.group(2));
                String frac = m.group(3);
                long ms = 0;
                if (frac != null) {
                    ms = frac.length() == 3 ? Long.parseLong(frac)
                            : frac.length() == 2 ? Long.parseLong(frac) * 10L
                            : Long.parseLong(frac) * 100L;
                }
                stamps.add(min * 60_000L + sec * 1_000L + ms);
                lastEnd = m.end();
            }
            String content = line.substring(lastEnd).trim();
            if (!stamps.isEmpty() && !content.isEmpty()) {
                for (Long ts : stamps) {
                    out.add(new LrcLine(ts, content));
                }
            }
        }
        Collections.sort(out, Comparator.comparingLong(a -> a.startTimeMs));
        return out;
    }

    private static boolean isEmpty(String s) {
        if (s == null) return true;
        String t = s.trim();
        return t.isEmpty() || "null".equalsIgnoreCase(t);
    }

    private static String safeTrim(String s) {
        if (s == null) return "";
        String t = s.trim();
        return "null".equalsIgnoreCase(t) ? "" : t;
    }

    private void evalOrQueue(String js) {
        if (pageReady && webView != null) {
            eval(js);
        } else {
            if (queuedJs == null || queuedJs.isEmpty() || queuedJs.length() > 65536) {
                queuedJs = js;
            } else {
                queuedJs = queuedJs + ";" + js;
            }
        }
    }

    private void eval(String js) {
        if (webView == null) return;
        main.post(() -> {
            if (webView != null) {
                webView.evaluateJavascript(js, null);
            }
        });
    }
}
