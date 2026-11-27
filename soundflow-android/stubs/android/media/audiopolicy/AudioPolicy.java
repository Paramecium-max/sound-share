package android.media.audiopolicy;

import android.media.AudioFocusInfo;

public class AudioPolicy {
    public abstract static class AudioPolicyFocusListener {
        public AudioPolicyFocusListener() {}
        public void onAudioFocusGrant(AudioFocusInfo afi, int requestResult) {}
        public void onAudioFocusLoss(AudioFocusInfo afi, boolean wasNotified) {}
        public void onAudioFocusRequest(AudioFocusInfo afi, int requestResult) {}
        public void onAudioFocusAbandon(AudioFocusInfo afi) {}
    }
}
