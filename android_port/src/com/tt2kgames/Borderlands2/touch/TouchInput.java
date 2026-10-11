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
    private boolean invertY, autoSprint, aimToggle, aimLatched, sprintLatched, sprintHeld, stopBlockedUntilNeutral;
    private float lastStickX, lastStickY;
    private long held;
    public TouchInput(Sink sink) { this.sink = sink; }
    public void autoSprint(boolean enabled) {
        if (autoSprint && !enabled) stopAutoRun();
        autoSprint = enabled;
    }
    public void aimToggle(boolean enabled) {
        if (aimToggle == enabled) return;
        aimToggle = enabled; aimLatched = false; recomputeHeld(); emit(0, 0);
    }

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
                if (!next.allows(AIM)) aimLatched = false;
                if (!next.allows(SPRINT)) stopAutoRun();
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
        if (role == BUTTON && (action < 0 || action >= ACTION_COUNT || !context.allows(action))) role = IGNORE;
        if (role == BUTTON && action == AIM && aimLatches()) {
            boolean alreadyOwned = false;
            for (Pointer p : pointers.values()) if (p.role == BUTTON && p.action == AIM) alreadyOwned = true;
            if (alreadyOwned) role = IGNORE;
            else aimLatched = !aimLatched;
        }
        Pointer p = new Pointer(role, action, x, y);
        pointers.put(id, p);
        if (role == MOVEMENT) {
            // A fresh stick touch stops the latch without releasing another finger's fire/aim.
            stopAutoRun(); moveX = moveY = 0; sprintHeld = false;
            moveId = id; lastStickX = x; lastStickY = y;
        }
        if ((role == LOOK || (role == BUTTON && (action == FIRE || action == AIM))) && lookId < 0
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
            if (ny >= -.45f) { sprintLatched = false; stopBlockedUntilNeutral = false; }
            sprintHeld = !stopBlockedUntilNeutral && context.mode == GAMEPLAY && context.allows(SPRINT)
                    && ny <= -1.6f && Math.abs(nx) <= .45f;
            if (!stopBlockedUntilNeutral && autoSprint && context.mode == GAMEPLAY && context.allows(SPRINT)
                    && ny <= -1.6f && Math.abs(nx) <= .45f
                    && (held & (bit(AIM) | bit(FIRE))) == 0) sprintLatched = true;
            if (sprintLatched) { moveX = 0; moveY = 1; }
            else if (stopBlockedUntilNeutral) moveX = moveY = 0;
        }
        float gain = degreesPerDp / density * ((held & bit(AIM)) != 0 ? adsMultiplier : 1);
        emit(id == lookId ? dx * gain : 0,
                id == lookId ? -dy * gain * (invertY ? -1 : 1) : 0);
    }
    public void up(int id, float x, float y) {
        move(id, x, y);
        pointers.remove(id);
        if (id == moveId) {
            moveId = -1; moveX = 0; moveY = sprintLatched ? 1 : 0; sprintHeld = false;
        }
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
    /** One native action edge pair; existing held owners and look remain intact. */
    public void pulse(int action) {
        if (!context.usable() || action < 0 || action >= ACTION_COUNT || !context.allows(action)
                || (held() & bit(action)) != 0) return;
        sink.submit(context.epoch, moveX, moveY, 0, 0, held() | bit(action));
        emit(0, 0);
    }
    /** Sensor degrees share the native relative-look seam and keep touch ownership. */
    public void relativeLook(float yawDegrees, float pitchDegrees) {
        if (!context.usable() || !context.gameplay() || !context.has(RELATIVE_LOOK)
                || !Float.isFinite(yawDegrees) || !Float.isFinite(pitchDegrees)) return;
        float gain = (held & bit(AIM)) != 0 ? adsMultiplier : 1;
        emit(yawDegrees*gain, pitchDegrees*gain);
    }
    private void clearLocal() {
        pointers.clear(); moveId = lookId = -1; moveX = moveY = 0; held = 0;
        aimLatched = sprintLatched = sprintHeld = stopBlockedUntilNeutral = false; lastStickX = lastStickY = 0;
    }
    private void stopAutoRun() {
        sprintLatched = sprintHeld = stopBlockedUntilNeutral = false;
        if (moveId < 0) moveX = moveY = 0;
    }
    private void recomputeHeld() {
        held = aimLatched ? bit(AIM) : 0;
        // OR of all owners: releasing one of two fingers cannot release the other.
        for (Pointer p : pointers.values()) if (p.role == BUTTON && !(p.action == AIM && aimLatches())) held |= bit(p.action);
    }
    // On foot Aim may toggle; a vehicle seat's Aim is the turret's held
    // alternate fire (ZoomWeapon -> StartAltFire/StopAltFire), never a latch.
    private boolean aimLatches() { return aimToggle && context.mode == GAMEPLAY && context.gameplay(); }
    private void emit(float yaw, float pitch) {
        if (context.epoch > 0) sink.submit(context.epoch, moveX, moveY, yaw, pitch, held());
    }
    public long held() {
        return held | (((sprintLatched && autoSprint) || sprintHeld) && context.mode == GAMEPLAY && context.allows(SPRINT)
                && (held & (bit(AIM) | bit(FIRE))) == 0 ? bit(SPRINT) : 0);
    }
    public boolean sprintLatched() { return sprintLatched; }
    public void stopRunning() {
        if (sprintLatched || sprintHeld) {
            sprintLatched = false; stopBlockedUntilNeutral = moveId >= 0;
            sprintHeld = false;
            moveX = moveY = 0; emit(0,0);
        }
    }
    public boolean moving() { return moveId >= 0 || sprintLatched; }
    public float stickX() { Pointer p = pointers.get(moveId); return p == null ? lastStickX : p.startX; }
    public float stickY() { Pointer p = pointers.get(moveId); return p == null ? lastStickY : p.startY; }
    public float moveX() { return moveX; }
    public float moveY() { return moveY; }
    /** Native HUD hit targets own their gesture; they never become a camera drag. */
    public static final class HudGesture {
        public static final int MAP_ZONE = 1, AMMO_ZONE = 2, NONE = -1;
        private final Map<Integer, Pointer> contacts = new HashMap<>();
        private int primary = -1;
        private boolean cancelled, secondTap, reloadRequested;
        private long epoch, started, reloadAfter, reloadEpoch;
        private float distance, tapX, tapY;
        public boolean owns(int id) { return contacts.containsKey(id); }
        public int zone(int id) { Pointer p=contacts.get(id); return p == null ? 0 : p.role; }
        public boolean captured() { return !contacts.isEmpty(); }
        public void down(int id, int zone, float x, float y, long now, long epoch, float slop) {
            if (id < 0 || !Float.isFinite(x) || !Float.isFinite(y) || contacts.containsKey(id)
                    || contacts.size() >= 10) { cancelled = true; return; }
            boolean first = contacts.isEmpty();
            contacts.put(id, new Pointer(zone, 0, x, y));
            if (!first) { cancelled = true; reloadAfter = 0; return; }
            primary = id; this.epoch = epoch; started = now; cancelled = false; distance = 0;
            secondTap = zone == AMMO_ZONE && reloadAfter > now && reloadEpoch == epoch
                    && Math.hypot(x-tapX, y-tapY) <= slop*2;
            reloadAfter = 0; reloadRequested = false;
        }
        public void move(int id, float x, float y) {
            Pointer p = contacts.get(id);
            if (p == null) return;
            if (!Float.isFinite(x) || !Float.isFinite(y)) { cancelled = true; return; }
            p.x = x; p.y = y;
            if (id == primary) distance = Math.max(distance, (float)Math.hypot(x-p.startX, y-p.startY));
        }
        public int up(int id, float x, float y, long now, Context current, boolean inside, float slop) {
            move(id, x, y);
            Pointer p = contacts.remove(id);
            int action = NONE;
            if (p != null && id == primary && !cancelled && inside && current.usable()
                    && current.gameplay() && epoch == current.epoch && now >= started) {
                if (p.role == MAP_ZONE) {
                    if (current.hudMap() && distance <= slop && now-started < 500 && current.allows(MAP)) action = MAP;
                } else if (p.role == AMMO_ZONE && current.mode == GAMEPLAY && current.hudAmmo()) {
                    float dx = x-p.startX, dy = y-p.startY;
                    if (distance > slop) {
                        if (current.weaponCount >= 2 && Math.abs(dx) >= slop*2 && Math.abs(dx) > Math.abs(dy)*1.35f) {
                            int swipe = dx > 0 ? NEXT_WEAPON : PREV_WEAPON;
                            if (current.allows(swipe)) action = swipe;
                        }
                    } else if (now-started < 500) {
                        if (secondTap) { if (canLast(current)) action = LAST_WEAPON; }
                        else if (current.weaponCount >= 2) {
                            reloadAfter = now+280; reloadEpoch = epoch; tapX = x; tapY = y;
                            reloadRequested = canReload(current);
                        } else if (canReload(current)) action = RELOAD;
                    }
                }
            }
            if (contacts.isEmpty()) { primary = -1; cancelled = secondTap = false; }
            return action;
        }
        private static boolean canReload(Context context) { return context.known(RELOAD) && context.allows(RELOAD); }
        private static boolean canLast(Context context) { return context.weaponCount >= 2 && context.known(LAST_WEAPON) && context.allows(LAST_WEAPON); }
        public int poll(long now, Context current) {
            if (reloadAfter == 0) return NONE;
            if (!current.usable() || current.mode != GAMEPLAY || current.epoch != reloadEpoch || !current.hudAmmo()) {
                reloadAfter = 0; reloadRequested = false; return NONE;
            }
            if (now < reloadAfter) return NONE;
            reloadAfter = 0; boolean requested=reloadRequested;reloadRequested=false;
            return requested && canReload(current) ? RELOAD : NONE;
        }
        public void cancel() { contacts.clear(); primary = -1; cancelled = secondTap = reloadRequested = false; reloadAfter = 0; }
    }
    public static float finiteClamp(float n, float lo, float hi) {
        return Float.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : lo;
    }
}
