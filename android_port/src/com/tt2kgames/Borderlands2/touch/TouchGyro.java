package com.tt2kgames.Borderlands2.touch;

/** Device rotation becomes screen-relative degrees, independent of frame rate. */
public final class TouchGyro {
    private static final float DEGREES = (float)(180 / Math.PI);
    private final float[] previous = new float[4];
    private long timestamp;
    private int rotation = -1;
    public final float[] delta = new float[2];

    public void reset() { timestamp = 0; rotation = -1; delta[0] = delta[1] = 0; }

    private float seconds(long now, int screenRotation) {
        float dt = timestamp > 0 ? (now - timestamp) * 1.0e-9f : 0;
        boolean continuous = rotation == screenRotation && dt > 0 && dt <= .1f;
        timestamp = now; rotation = screenRotation; delta[0] = delta[1] = 0;
        return continuous ? dt : 0;
    }
    /** Quaternion is world-from-device, in Android's w,x,y,z order. */
    public void orientation(float w, float x, float y, float z, long now, int screenRotation,
            float sensitivity, float deadzone, boolean invertY) {
        float norm = (float)Math.sqrt(w*w + x*x + y*y + z*z);
        if (!Float.isFinite(norm) || norm < .5f || norm > 2 || now <= 0) { reset(); return; }
        w /= norm; x /= norm; y /= norm; z /= norm;
        float dt = seconds(now, screenRotation);
        if (dt > 0) {
            // q(previous)^-1 * q(current): local rotation has no Euler singularity.
            float rw = previous[0]*w + previous[1]*x + previous[2]*y + previous[3]*z;
            float rx = previous[0]*x - previous[1]*w - previous[2]*z + previous[3]*y;
            float ry = previous[0]*y + previous[1]*z - previous[2]*w - previous[3]*x;
            float rz = previous[0]*z - previous[1]*y + previous[2]*x - previous[3]*w;
            if (rw < 0) { rw = -rw; rx = -rx; ry = -ry; rz = -rz; }
            float length = (float)Math.sqrt(rx*rx + ry*ry + rz*rz);
            float gain = length > 1e-6f ? 2*(float)Math.atan2(length, rw)/length : 2;
            screenDelta(rx*gain, ry*gain, dt, screenRotation, sensitivity, deadzone, invertY);
        }
        previous[0] = w; previous[1] = x; previous[2] = y; previous[3] = z;
    }
    /** Angular velocity fallback for devices without a fused rotation vector. */
    public void velocity(float x, float y, long now, int screenRotation,
            float sensitivity, float deadzone, boolean invertY) {
        if (!Float.isFinite(x) || !Float.isFinite(y) || now <= 0) { reset(); return; }
        float dt = seconds(now, screenRotation);
        if (dt > 0) screenDelta(x*dt, y*dt, dt, screenRotation, sensitivity, deadzone, invertY);
    }
    private void screenDelta(float x, float y, float dt, int screenRotation,
            float sensitivity, float deadzone, boolean invertY) {
        float screenX = x, screenY = y;
        if (screenRotation == 1) { screenX = y; screenY = -x; }
        else if (screenRotation == 2) { screenX = -x; screenY = -y; }
        else if (screenRotation == 3) { screenX = -y; screenY = x; }
        float gain = TouchInput.finiteClamp(sensitivity, .25f, 3);
        float threshold = TouchInput.finiteClamp(deadzone, 0, 3)*dt;
        delta[0] = filter(screenY*DEGREES, threshold)*gain;
        delta[1] = filter(-screenX*DEGREES, threshold)*gain*(invertY ? -1 : 1);
    }
    private static float filter(float degrees, float threshold) {
        if (!Float.isFinite(degrees)) return 0;
        return Math.copySign(Math.min(25, Math.max(0, Math.abs(degrees)-threshold)), degrees);
    }
}
