package io.soundflow.receiver;

final class PlaybackPosition {
    private static final long MAX_MIPLAY_POSITION_MS = 24L * 60 * 60 * 1000;

    private PlaybackPosition() {}

    static boolean isValidMiPlay(long positionMs, long durationMs) {
        return positionMs >= 0 && positionMs <= MAX_MIPLAY_POSITION_MS
                && (durationMs <= 0 || positionMs <= durationMs);
    }

    static long parseMiPlayMillis(Object value) {
        long millis;
        if (value instanceof Integer || value instanceof Long) {
            millis = ((Number) value).longValue();
        } else if (value instanceof String) {
            try {
                millis = Long.parseLong(((String) value).trim());
            } catch (NumberFormatException ignored) {
                return -1L;
            }
        } else {
            return -1L;
        }
        return isValidMiPlay(millis, 0) ? millis : -1L;
    }
}
