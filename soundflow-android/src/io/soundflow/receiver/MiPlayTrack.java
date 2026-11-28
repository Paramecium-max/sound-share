package io.soundflow.receiver;

import org.json.JSONObject;

final class MiPlayTrack {
    final String title;
    final String artist;
    final String album;
    final String lyrics;
    final String line;
    final String artwork;
    final long durationMs;
    private final String id;

    MiPlayTrack(JSONObject event, MiPlayTrack previous) {
        String trackId = text(event, "track_id");
        id = trackId.startsWith("metadata:") ? "" : trackId;
        String incomingTitle = text(event, "title");
        String incomingArtist = text(event, "artist");
        String incomingAlbum = text(event, "album");
        boolean same = previous != null && sameIdentity(id, incomingTitle, incomingArtist, incomingAlbum, previous);
        title = keepWhenMissing(incomingTitle, same ? previous.title : "");
        artist = keepWhenMissing(incomingArtist, same ? previous.artist : "");
        album = keepWhenMissing(incomingAlbum, same ? previous.album : "");
        lyrics = keepWhenMissing(text(event, "lyrics"), same ? previous.lyrics : "");
        line = keepWhenMissing(text(event, "lyric_line"), same ? previous.line : "");
        String image = text(event, "artwork").replaceAll("\\s+", "");
        artwork = image.length() > 196608 ? ""
                : keepWhenMissing(image, same ? previous.artwork : "");
        long duration = PlaybackPosition.parseMiPlayMillis(event.opt("duration_ms"));
        durationMs = duration > 0 ? duration : same ? previous.durationMs : 0;
    }

    boolean hasIdentity() {
        return !title.isEmpty() || !album.isEmpty() || !artist.isEmpty() || !id.isEmpty();
    }

    boolean sameSong(MiPlayTrack previous) {
        return sameIdentity(id, title, artist, album, previous);
    }

    private static boolean sameIdentity(String id, String title, String artist, String album, MiPlayTrack previous) {
        if (!id.isEmpty() && !previous.id.isEmpty()) return id.equals(previous.id);
        if (!artist.equals(previous.artist) || !album.equals(previous.album)) return false;
        return title.equals(previous.title) || (!album.isEmpty() && artist.startsWith(album + "-"));
    }

    boolean matchesDlna(String dlnaTitle, String dlnaArtist) {
        return !dlnaTitle.isEmpty() && (dlnaTitle.equals(album)
                || (dlnaTitle.equals(title) && (dlnaArtist.isEmpty() || dlnaArtist.equals(artist)))
                || (artist.startsWith(dlnaTitle + "-")
                && (dlnaArtist.isEmpty() || artist.endsWith("-" + dlnaArtist))));
    }

    private static String text(JSONObject event, String key) {
        return event.isNull(key) ? "" : event.optString(key, "");
    }

    private static String keepWhenMissing(String value, String fallback) {
        return value.isEmpty() ? fallback : value;
    }
}
