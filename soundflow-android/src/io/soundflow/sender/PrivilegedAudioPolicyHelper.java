package io.soundflow.sender;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/**
 * Reflective @SystemApi AudioPolicy loopback helper shared by:
 * 1) Direct Magisk priv-app mode (CAPTURE_AUDIO_OUTPUT + MODIFY_AUDIO_ROUTING)
 * 2) Shizuku UID 2000 UserService mode (com.android.shell AttributionSource)
 *
 * Uses AudioPolicy.createAudioRecordSink(AudioMix) only; never opens any microphone input.
 */
public final class PrivilegedAudioPolicyHelper {
    private static final int RULE_MATCH_ATTRIBUTE_USAGE = 0x1;
    private static final int RULE_MATCH_UID = 0x4;
    private static final int RULE_EXCLUDE_UID = 0x8000 | RULE_MATCH_UID; // 0x8004

    private static final int ROUTE_FLAG_RENDER = 0x1;
    private static final int ROUTE_FLAG_LOOP_BACK = 0x2;

    private PrivilegedAudioPolicyHelper() {}

    public static final class ActivePolicySession {
        public final AudioManager audioManager;
        public final Object audioPolicy;
        public final AudioRecord audioRecord;
        private volatile boolean closed = false;

        ActivePolicySession(AudioManager am, Object policy, AudioRecord record) {
            this.audioManager = am;
            this.audioPolicy = policy;
            this.audioRecord = record;
        }

        public void close() {
            if (closed) return;
            closed = true;
            try { audioRecord.stop(); } catch (Throwable ignored) {}
            try { audioRecord.release(); } catch (Throwable ignored) {}
            try {
                Method unreg = AudioManager.class.getMethod("unregisterAudioPolicyAsync", audioPolicy.getClass());
                unreg.invoke(audioManager, audioPolicy);
            } catch (Throwable fallback) {
                try {
                    Method unregSync = AudioManager.class.getMethod("unregisterAudioPolicy", audioPolicy.getClass());
                    unregSync.invoke(audioManager, audioPolicy);
                } catch (Throwable ignored) {}
            }
        }
    }

    public static boolean hasDirectPrivilegedRouting(Context ctx) {
        if (ctx == null) return false;
        return ctx.checkSelfPermission("android.permission.MODIFY_AUDIO_ROUTING") == PackageManager.PERMISSION_GRANTED
                && ctx.checkSelfPermission("android.permission.CAPTURE_AUDIO_OUTPUT") == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Opens a privileged AudioPolicy loopback sink without MediaProjection.
     *
     * @param attributionContext Context carrying privileged UID (priv-app or com.android.shell under UID 2000)
     * @param sampleRate         48000 or 44100
     * @param selfAppUid         SoundFlow's own UID (excluded in global mode to prevent feedback loops)
     * @param targetAppUid       > 0 for single-app capture; <= 0 for global media capture
     * @param muteLocalSpeaker   true -> ROUTE_FLAG_LOOP_BACK (target app silent locally, other apps unaffected);
     *                           false -> ROUTE_FLAG_LOOP_BACK | ROUTE_FLAG_RENDER (plays locally & remotely)
     */
    public static ActivePolicySession openLoopbackSession(
            Context attributionContext,
            int sampleRate,
            int selfAppUid,
            int targetAppUid,
            boolean muteLocalSpeaker) throws Exception {

        if (attributionContext == null) {
            throw new IllegalArgumentException("attributionContext is null");
        }
        AudioManager am = (AudioManager) attributionContext.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) throw new IllegalStateException("AudioManager unavailable");

        Class<?> ruleBuilderCls = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object ruleBuilder = ruleBuilderCls.getConstructor().newInstance();

        // Allow capturing even apps with allowAudioPlaybackCapture="false" when privileged.
        try {
            Method allowPriv = ruleBuilderCls.getMethod("allowPrivilegedPlaybackCapture", boolean.class);
            allowPriv.invoke(ruleBuilder, true);
        } catch (NoSuchMethodException ignored) {}

        Method addRuleAttr = ruleBuilderCls.getMethod("addRule", AudioAttributes.class, int.class);
        for (int usage : new int[]{
                AudioAttributes.USAGE_MEDIA,
                AudioAttributes.USAGE_GAME,
                AudioAttributes.USAGE_UNKNOWN
        }) {
            AudioAttributes attr = new AudioAttributes.Builder().setUsage(usage).build();
            addRuleAttr.invoke(ruleBuilder, attr, RULE_MATCH_ATTRIBUTE_USAGE);
        }

        Method addMixRule = ruleBuilderCls.getMethod("addMixRule", int.class, Object.class);
        if (targetAppUid > 0 && targetAppUid != selfAppUid) {
            addMixRule.invoke(ruleBuilder, RULE_MATCH_UID, Integer.valueOf(targetAppUid));
        } else if (selfAppUid > 0) {
            addMixRule.invoke(ruleBuilder, RULE_EXCLUDE_UID, Integer.valueOf(selfAppUid));
        }

        Object mixingRule = ruleBuilderCls.getMethod("build").invoke(ruleBuilder);

        Class<?> mixBuilderCls = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Constructor<?> mixCtor = mixBuilderCls.getConstructor(mixingRule.getClass());
        Object mixBuilder = mixCtor.newInstance(mixingRule);

        AudioFormat fmt = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build();
        mixBuilderCls.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, fmt);

        int routeFlags = muteLocalSpeaker
                ? ROUTE_FLAG_LOOP_BACK
                : (ROUTE_FLAG_LOOP_BACK | ROUTE_FLAG_RENDER);
        mixBuilderCls.getMethod("setRouteFlags", int.class).invoke(mixBuilder, routeFlags);
        Object audioMix = mixBuilderCls.getMethod("build").invoke(mixBuilder);

        Class<?> policyBuilderCls = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object policyBuilder = policyBuilderCls.getConstructor(Context.class).newInstance(attributionContext);
        policyBuilderCls.getMethod("addMix", audioMix.getClass()).invoke(policyBuilder, audioMix);
        Object audioPolicy = policyBuilderCls.getMethod("build").invoke(policyBuilder);

        Method regMethod = AudioManager.class.getMethod("registerAudioPolicy", audioPolicy.getClass());
        int res = (Integer) regMethod.invoke(am, audioPolicy);
        if (res != 0) { // AudioManager.SUCCESS == 0 (@SystemApi)
            throw new SecurityException("registerAudioPolicy failed with code=" + res);
        }

        try {
            Method createSink = audioPolicy.getClass().getMethod("createAudioRecordSink", audioMix.getClass());
            AudioRecord record = (AudioRecord) createSink.invoke(audioPolicy, audioMix);
            if (record == null || record.getState() != AudioRecord.STATE_INITIALIZED) {
                throw new IllegalStateException("createAudioRecordSink returned uninitialized AudioRecord");
            }
            return new ActivePolicySession(am, audioPolicy, record);
        } catch (Throwable t) {
            try {
                Method unreg = AudioManager.class.getMethod("unregisterAudioPolicy", audioPolicy.getClass());
                unreg.invoke(am, audioPolicy);
            } catch (Throwable ignored) {}
            if (t instanceof Exception) throw (Exception) t;
            throw new RuntimeException(t);
        }
    }
}
