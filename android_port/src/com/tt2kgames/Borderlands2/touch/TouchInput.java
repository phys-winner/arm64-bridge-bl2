package com.tt2kgames.Borderlands2.touch;

import java.util.HashMap;
import java.util.Map;
import java.util.Iterator;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

/** Android-independent pointer ownership and touch-to-semantic input transform. */
public final class TouchInput {
    public interface Sink {
        void submit(long epoch, float moveX, float moveY, float yaw, float pitch, long held);
        void reset();
    }
    public static final int IGNORE = 0, MOVEMENT = 1, LOOK = 2, BUTTON = 3;
    private static final class Pointer {
        int role, action;
        float startX, startY, x, y;
        Pointer(int role, int action, float x, float y) {
            this.role = role; this.action = action;
            startX = this.x = x; startY = this.y = y;
        }
    }
    private final Sink sink;
    private final Map<Integer, Pointer> pointers = new HashMap<>();
    private Context context = Context.unknown();
    private int moveId = -1, lookId = -1;
    private float moveX, moveY;
    private float density = 1, radiusDp = 60, degreesPerDp = .22f, adsMultiplier = .55f;
    private boolean invertY;
    private long held;
    public TouchInput(Sink sink) { this.sink = sink; }

    public void configure(float density, float radiusDp, float degreesPerDp,
            float adsMultiplier, boolean invertY) {
        this.density = finiteClamp(density, .5f, 8);
        this.radiusDp = finiteClamp(radiusDp, 32, 120);
        this.degreesPerDp = finiteClamp(degreesPerDp, .04f, 1);
        this.adsMultiplier = finiteClamp(adsMultiplier, .1f, 1);
        this.invertY = invertY;
    }
    public void setContext(Context next) {
        if (next.epoch != context.epoch || next.mode != context.mode
                || next.capabilities != context.capabilities) {
            clearLocal();
            context = next;
            if (next.epoch > 0) emit(0, 0); // Rearm mailbox without inventing a DOWN.
        } else {
            if (next.actions != context.actions) {
                Iterator<Map.Entry<Integer,Pointer>> it = pointers.entrySet().iterator();
                while (it.hasNext()) {
                    Map.Entry<Integer,Pointer> entry = it.next();
                    Pointer p = entry.getValue();
                    if (p.role == BUTTON && !next.allows(p.action)) {
                        if (entry.getKey() == lookId) lookId = -1;
                        it.remove();
                    }
                }
                recomputeHeld(); context = next; emit(0,0);
            } else context = next;
        }
    }
    public void down(int id, int role, int action, float x, float y) {
        if (!context.usable() || pointers.containsKey(id) || pointers.size() >= 10
                || !Float.isFinite(x) || !Float.isFinite(y)) return;
        if (role == MOVEMENT && (!context.gameplay() || moveId >= 0)) role = IGNORE;
        if (role == LOOK && (!context.gameplay() || !context.has(RELATIVE_LOOK))) role = IGNORE;
        if (role == BUTTON && (action < 0 || action > UI_RIGHT || !context.allows(action))) role = IGNORE;
        Pointer p = new Pointer(role, action, x, y);
        pointers.put(id, p);
        if (role == MOVEMENT) moveId = id;
        if ((role == LOOK || (role == BUTTON && action == FIRE)) && lookId < 0
                && context.gameplay() && context.has(RELATIVE_LOOK)) lookId = id;
        recomputeHeld();
        emit(0, 0);
    }
    /** Call for every historical sample, then the current MotionEvent sample. */
    public void move(int id, float x, float y) {
        Pointer p = pointers.get(id);
        if (p == null || !Float.isFinite(x) || !Float.isFinite(y)) return;
        float dx = x - p.x, dy = y - p.y;
        p.x = x; p.y = y;
        if (id == moveId) {
            float nx = (x - p.startX) / (density * radiusDp);
            float ny = (y - p.startY) / (density * radiusDp);
            float length = (float) Math.sqrt(nx * nx + ny * ny);
            float magnitude = Math.min(1, Math.max(0, (length - .12f) / .88f));
            moveX = length > 0 ? nx / length * magnitude : 0;
            moveY = length > 0 ? -ny / length * magnitude : 0;
        }
        float gain = degreesPerDp / density * ((held & bit(AIM)) != 0 ? adsMultiplier : 1);
        emit(id == lookId ? dx * gain : 0,
                id == lookId ? -dy * gain * (invertY ? -1 : 1) : 0);
    }
    public void up(int id, float x, float y) {
        move(id, x, y);
        pointers.remove(id);
        if (id == moveId) { moveId = -1; moveX = moveY = 0; }
        if (id == lookId) lookId = -1; // No mid-gesture ownership transfer.
        recomputeHeld();
        emit(0, 0);
    }
    /** Cancels all touch sources. Caller must poll a fresh epoch before accepting DOWN. */
    public void cancel() {
        clearLocal(); context = Context.unknown(); sink.reset();
    }
    /** Refresh stationary holds without repeating camera displacement. */
    public void heartbeat() { emit(0, 0); }
    private void clearLocal() {
        pointers.clear(); moveId = lookId = -1; moveX = moveY = 0; held = 0;
    }
    private void recomputeHeld() {
        held = 0;
        // OR of all owners: releasing one of two fingers cannot release the other.
        for (Pointer p : pointers.values()) if (p.role == BUTTON) held |= bit(p.action);
    }
    private void emit(float yaw, float pitch) {
        if (context.epoch > 0) sink.submit(context.epoch, moveX, moveY, yaw, pitch, held);
    }
    public long held() { return held; }
    public boolean moving() { return moveId >= 0; }
    public float stickX() { Pointer p = pointers.get(moveId); return p == null ? 0 : p.startX; }
    public float stickY() { Pointer p = pointers.get(moveId); return p == null ? 0 : p.startY; }
    public float moveX() { return moveX; }
    public float moveY() { return moveY; }
    public static float finiteClamp(float n, float lo, float hi) {
        return Float.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : lo;
    }
}
