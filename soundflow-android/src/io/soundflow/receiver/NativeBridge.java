package io.soundflow.receiver;

/** JNI bridge to the self-contained Rust MiPlay/Lyra/Oboe native receiver core. */
public final class NativeBridge {
    static {
        System.loadLibrary("soundflow");
    }

    private NativeBridge() {}

    public static native String start(String configJson);

    public static native void stop();

    public static native String poll();

    public static native String action(String action, int value);

    public static native void setAudioSyncOffset(int millis);

    public static native float audioLevel();

    public static native int audioSpectrum();

    public static native String audioOnsets();

    public static native String spectrumOnsets(float[] bins, int sampleRate, long stampNanos);

    public static native void setAudioMeterEnabled(boolean enabled);

    public static native void setOnsetCapture(boolean enabled, long monotonicNanos);

    public static native void setOnsetSensitivity(int value);
}

