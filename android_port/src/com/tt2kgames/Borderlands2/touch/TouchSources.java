package com.tt2kgames.Borderlands2.touch;

/** Source tag for the temporary legacy overlay. This never invokes UE3. */
public final class TouchSources {
    private TouchSources() { }
    public static native void nativeBeginLegacy();
    public static native void nativeEndLegacy();
}
