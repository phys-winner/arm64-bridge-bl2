package com.tt2kgames.Borderlands2.touch;

/** Linked into the already loaded arm64 bridge, never loads the ARM32 guest. */
public final class TouchBridge implements TouchInput.Sink {
    private static native long[] nativeSnapshot();
    private static native void nativeSubmit(long epoch, float moveX, float moveY,
            float yawDegrees, float pitchDegrees, long heldActions);
    private static native void nativeReset();
    private static native void nativeUiPointer(long epoch, int pointerId, int phase,
            float surfaceNormalizedX, float surfaceNormalizedY);
    private static native int[] nativeSettingsExchange(int[] hostValues);
    private static native int nativeMovieState();
    private static native boolean nativeSkipMovie();

    public TouchContract.Context snapshot() {
        return TouchContract.Context.decode(nativeSnapshot());
    }
    @Override public void submit(long epoch, float x, float y, float yaw, float pitch, long held) {
        nativeSubmit(epoch, x, y, yaw, pitch, held);
    }
    @Override public void reset() { nativeReset(); }
    public void uiPointer(long epoch, int id, int phase, float x, float y) {
        nativeUiPointer(epoch, id, phase, x, y);
    }
    public int[] settingsExchange(int[] hostValues) { return nativeSettingsExchange(hostValues); }
    /** TouchContract.MOVIE_* of the full-screen movie, read from the player itself:
     *  the startup sequence blocks the game thread that feeds the mailbox. */
    public int movieState() { return nativeMovieState(); }
    /** Asks the player to stop the movie at its next frame; false if nothing to skip. */
    public boolean skipMovie() { return nativeSkipMovie(); }
}
