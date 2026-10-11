package com.tt2kgames.Borderlands2.touch;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.util.SparseArray;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.Arrays;
import com.tt2kgames.Borderlands2.LaunchControls;
import com.tt2kgames.Borderlands2.GameRestart;
import com.tt2kgames.Borderlands2.GameStorage;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

/**
 * Compact, context-driven controls. All methods run on the UI thread.
 */
public final class NativeTouchControls extends View implements Choreographer.FrameCallback,
        SurfaceHolder.Callback, SensorEventListener {
    public interface RouteListener { void nativeRouteChanged(boolean ownsInput); }
    private static final String TAG = "BL2NativeTouch";
    private static final int MORE = -2, DONE = -3;
    private static final int LEGACY = 1000;
    // On foot; Give up exists only where InputSet_Injured offers Suicide (FFYL).
    private static final int[] RADIAL = {NEXT_WEAPON, SKILL, CROUCH, MELEE, INVENTORY, MAP, PREV_WEAPON, RELOAD, SUICIDE, CHAT};
    // A vehicle seat's radial (review 4.4): Horn and Switch seat. Look back and
    // Turret are holds: a second finger on the open radial keeps them pressed.
    private static final int[] RADIAL_VEHICLE = {VEHICLE_HORN, SWITCH_SEATS, LOOK_BACK, DRIVER_FIRE_TURRET,
            INVENTORY, MAP, NEXT_WEAPON, RELOAD, CHAT};
    /** Give up acts only after its sector is held this long. */
    private static final long CONFIRM_HOLD_MS = 1000;
    private static final long STALE_MS = 1000;
    private static final int HINT_MAP = TouchPreferences.HINT_MAP, HINT_WEAPON = TouchPreferences.HINT_WEAPON,
            HINT_GAME_MORE = TouchPreferences.HINT_GAME_MORE, HINT_MENU_MORE = TouchPreferences.HINT_MENU_MORE,
            HINT_DEBUG_MORE = TouchPreferences.HINT_DEBUG_MORE, HINT_AUTORUN = TouchPreferences.HINT_AUTORUN,
            HINT_MAP_NAVIGATION = TouchPreferences.HINT_MAP_NAVIGATION, HINT_LAYOUT = TouchPreferences.HINT_LAYOUT,
            HINT_SLOTS = TouchPreferences.HINT_SLOTS, HINT_SKILL_TAP = TouchPreferences.HINT_SKILL_TAP,
            HINT_STATUS_TAPS = TouchPreferences.HINT_STATUS_TAPS;
    private static volatile WeakReference<NativeTouchControls> active = new WeakReference<>(null);
    private static final class Button {
        final int action;
        final String label;
        final RectF rect;
        final float hitSlop;
        Button(int action, String label, RectF rect,float hitSlop) {
            this.action = action; this.label = label; this.rect = rect; this.hitSlop=hitSlop;
        }
        boolean contains(float x,float y) { return TouchLayout.circleContains(x,y,rect.centerX(),rect.centerY(),rect.width()/2,hitSlop); }
    }
    private final Activity activity;
    private final boolean russian;
    private final View surface;
    private final TouchBridge bridge = new TouchBridge();
    private final TouchInput input = new TouchInput(new TouchInput.Sink() {
        public void submit(long epoch,float x,float y,float yaw,float pitch,long held) {
            if(!fallback) { bridge.submit(epoch,x,y,yaw,pitch,held); publishLegacyKeys(0);return; }
            legacyHeld=held;
            publishLegacyKeys(held);
            fallbackYaw+=yaw; fallbackPitch+=pitch;
            LaunchControls.axes(activity,x,y,Math.max(-1,Math.min(1,fallbackYaw*.12f)),Math.max(-1,Math.min(1,fallbackPitch*.12f)));
        }
        public void reset() { bridge.reset(); LaunchControls.cancel(activity);legacyKeys.clear(); legacyHeld=0; fallbackYaw=fallbackPitch=0; }
    });
    private final TouchPreferences preferences;
    private final TouchInput.HudGesture hudGesture = new TouchInput.HudGesture();
    private final TouchInput.HudTaps hudTaps = new TouchInput.HudTaps();
    private final SparseArray<RectF> hudPointers = new SparseArray<>();
    private final RectF mapTarget = new RectF(), ammoTarget = new RectF(), useTarget = new RectF();
    private final RectF skillTarget = new RectF(), experienceTarget = new RectF(),
            challengeTarget = new RectF(), objectiveTarget = new RectF();
    /** Weapon-slot cells while a hold on the ammo strip keeps them open. */
    private float[][] slotCells;
    private int slotSelection = -1;
    private final SensorManager sensorManager;
    private final Sensor aimSensor;
    private final TouchGyro gyro = new TouchGyro();
    private final float[] quaternion = new float[4];
    private boolean sensorRegistered, loggedSensor;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF safe = new RectF(), viewport = new RectF();
    private final ArrayList<Button> buttons = new ArrayList<>();
    private final ArrayList<Button> radialButtons = new ArrayList<>();
    private final SparseArray<Button> utilityPointers = new SparseArray<>();
    private final int[] moveVisualBefore = new int[9], moveVisualAfter = new int[9];
    private final SparseArray<Integer> legacyPointers = new SparseArray<>();
    private final SparseArray<Boolean> legacyKeys = new SparseArray<>();
    private final ArrayList<float[]> occupied = new ArrayList<>();
    private final ArrayList<float[]> hudReserved = new ArrayList<>();
    private final SparseArray<float[]> uiPointers = new SparseArray<>();
    private int menuPointer = -1;
    private int menuMorePointer = -1;
    private Button menuMoreButton;
    private long menuMoreStarted, menuMoreEpoch, menuExpandedUntil;
    private final int[] screenLocation = new int[2], surfaceLocation = new int[2];
    private Context context = Context.unknown();
    private boolean focused, resumed = true, surfaceAlive, disposed, scheduled;
    private boolean eligible, streamOwned, editing, expanded, linkageFailed;
    private boolean layoutDirty = true;
    private float layoutScale = 1;
    private int insetL, insetT, insetR, insetB, editPointer = -1;
    private Button editButton;
    private float editOffsetX, editOffsetY;
    private long lastFrame = -1, lastFrameTime;
    private int settingsRevision, renderRestartOffered, videoOptionsAway;
    private long lastSettingsPoll;
    private boolean layoutEditPending, layoutChanged, inWorld;
    private android.app.AlertDialog restartDialog;
    /** Render resolution this process started with: the engine sizes its targets once. */
    private static int sessionRenderPercent;
    private RouteListener routeListener;
    private boolean nativeRoute, imeVisible;
    private boolean fallback, radialOpen, radialDragged;
    /** RADIAL BY TAP: open after a short tap, waiting for a second tap. */
    private boolean radialTapMode;
    private long legacyHeld;
    private float fallbackYaw, fallbackPitch, radialX,radialY,radialRadius,radialItemRadius,radialStartX,radialStartY;
    private int radialPointer=-1, radialSelection=-1, radialCount=8;
    private int[] radial=RADIAL;
    private long radialStarted, radialSelectedAt;
    private String hintText;
    private long hintUntil;
    private int hintKind;
    private int nextLegacyId=100000, accessibilityFocus=View.NO_ID, hoverTarget=View.NO_ID;

    private NativeTouchControls(Activity owner, View renderSurface) {
        super(owner);
        activity = owner; surface = renderSurface;
        russian = "ru".equals(GameStorage.appLanguage(owner));
        preferences = new TouchPreferences(owner);
        sensorManager = (SensorManager)owner.getSystemService(android.content.Context.SENSOR_SERVICE);
        Sensor vector = sensorManager == null ? null : sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
        aimSensor = vector != null ? vector : sensorManager == null ? null : sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        focused = owner.hasWindowFocus();
        setFocusable(false);
        setClickable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setOnApplyWindowInsetsListener((view, insets) -> {
            int l = insets.getSystemWindowInsetLeft(), t = insets.getSystemWindowInsetTop();
            int r = insets.getSystemWindowInsetRight(), b = insets.getSystemWindowInsetBottom();
            boolean ime = false;
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets reserved = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.mandatorySystemGestures());
                l = reserved.left; t = reserved.top; r = reserved.right; b = reserved.bottom;
                ime = insets.isVisible(WindowInsets.Type.ime());
            }
            if (android.os.Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                l = Math.max(l, insets.getDisplayCutout().getSafeInsetLeft());
                t = Math.max(t, insets.getDisplayCutout().getSafeInsetTop());
                r = Math.max(r, insets.getDisplayCutout().getSafeInsetRight());
                b = Math.max(b, insets.getDisplayCutout().getSafeInsetBottom());
            }
            if (l != insetL || t != insetT || r != insetR || b != insetB || ime != imeVisible) cancelInput();
            insetL = l; insetT = t; insetR = r; insetB = b;
            imeVisible = ime;
            return insets;
        });
        if (surface instanceof SurfaceView) {
            SurfaceHolder holder = ((SurfaceView) surface).getHolder();
            surfaceAlive = holder.getSurface().isValid();
            holder.addCallback(this);
        }
    }
    public static void attach(Activity owner, View renderSurface) {
        if (renderSurface == null) throw new IllegalArgumentException("renderSurface is required");
        NativeTouchControls old = active.get();
        if (old != null && old.activity == owner && old.surface == renderSurface && !old.disposed) return;
        if (old != null) old.dispose();
        NativeTouchControls view = new NativeTouchControls(owner, renderSurface);
        View decor = owner.getWindow().getDecorView();
        if (!(decor instanceof ViewGroup)) throw new IllegalStateException("Decor must be ViewGroup");
        ((ViewGroup) decor).addView(view, new ViewGroup.LayoutParams(-1, -1));
        active = new WeakReference<>(view);
        view.requestApplyInsets();
        view.schedule();
    }
    public static void focus(Activity owner, boolean value) {
        NativeTouchControls view = active.get();
        if (view == null || view.activity != owner) return;
        view.focused = value;
        if (!value) view.cancelInput();
        else view.schedule();
    }
    /** Temporary rollout seam: hide/release the old overlay while this route owns input. */
    public static void setRouteListener(Activity owner, RouteListener listener) {
        NativeTouchControls view = active.get();
        if (view == null || view.activity != owner) return;
        view.routeListener = listener;
        if (listener != null) listener.nativeRouteChanged(view.nativeRoute);
    }
    private void publishRoute(boolean owns) {
        if (owns == nativeRoute) return;
        nativeRoute = owns;
        if (routeListener != null) routeListener.nativeRouteChanged(owns);
    }
    public static void resumed(Activity owner, boolean value) {
        NativeTouchControls view = active.get();
        if (view == null || view.activity != owner) return;
        view.resumed = value;
        // A backgrounded process may be killed without another callback.
        if (!value) { view.cancelInput(); view.unschedule(); view.preferences.flush(); }
        else view.schedule();
    }
    public static void detach(Activity owner) {
        NativeTouchControls view = active.get();
        if (view != null && view.activity == owner) view.dispose();
    }
    /**
     * Android Back behaves like the on-screen Back (menus), Pause (play) or Skip
     * (cinematic) button after closing this overlay's own transient UI. False
     * leaves Back to the caller: no linked route, or a controller owns input.
     */
    public static boolean systemBack(Activity owner) {
        NativeTouchControls view = active.get();
        if (view == null || view.activity != owner || view.disposed || view.linkageFailed) return false;
        return view.back();
    }
    private boolean back() {
        if (editing) { editing=false; expanded=false; cancelInput(); return true; }
        if (radialOpen) { closeRadial(); invalidate(); return true; }
        if (expanded && context.menu()) { clearMenuMore(); buildButtons(); invalidate(); return true; }
        if (context.has(PHYSICAL_OWNS_INPUT)) return false;
        if (context.mode == CINEMATIC) {
            int action = cinematicBackAction(context,eligible);
            if (action < 0) return false;
            activate(nextLegacyId++, action, 0, 0);
            return true;
        }
        // Loading, text entry and stale frames swallow Back instead of closing the game.
        if (!eligible) return true;
        int action = context.gameplay() ? PAUSE : context.menu() ? BACK : -1;
        if (action >= 0 && context.allows(action)) activate(nextLegacyId++, action, 0, 0);
        return true;
    }
    /**
     * Back in a cinematic skips it, else opens the pause menu. Without a routed
     * cinematic action -1 keeps the game's own Android Back instead of eating it.
     */
    static int cinematicBackAction(Context c,boolean eligible) {
        if (c.mode != CINEMATIC || !eligible || !c.usable()) return -1;
        return c.allows(SKIP) ? SKIP : c.allows(PAUSE) ? PAUSE : -1;
    }
    /** Only the legacy fallback still needs virtual key X; native Use is a held action bit. */
    static boolean legacyUse(Context c,boolean fallback,int action) {
        return fallback && action == USE && c.gameplay();
    }
    private void schedule() {
        if (!scheduled && !disposed && resumed && !linkageFailed) {
            scheduled = true; Choreographer.getInstance().postFrameCallback(this);
        }
    }
    private void unschedule() {
        Choreographer.getInstance().removeFrameCallback(this); scheduled = false;
    }
    @Override public void doFrame(long frameTimeNanos) {
        scheduled = false;
        if (disposed || !resumed || linkageFailed) return;
        try {
            updateGeometry();
            Context next = bridge.snapshot();
            long now = SystemClock.uptimeMillis();
            if (syncSettings(next, now)) next = bridge.snapshot();
            if (next.frame != lastFrame) { lastFrame = next.frame; lastFrameTime = now; }
            boolean nextFallback=next.legacyFallback();
            boolean nextEligible = focused && surfaceAlive && !imeVisible
                    && (next.usable() || (nextFallback && LaunchControls.available(activity)))
                    && (nextFallback || now - lastFrameTime < STALE_MS) && !viewport.isEmpty() && !safe.isEmpty();
            if(fallback!=nextFallback) { cancelInput(); next=bridge.snapshot(); }
            if (eligible && !nextEligible) {
                cancelInput();
                // reset advanced the epoch; never re-use the snapshot preceding it.
                next = bridge.snapshot();
            }
            if (next.epoch != context.epoch) {
                saveLayout();
                clearMenuMore();
                uiPointers.clear(); menuPointer = -1; editPointer = -1; editButton = null;
                if(!next.gameplay()) editing=false;
                closeRadial(); utilityPointers.clear(); legacyPointers.clear();
                hudGesture.cancel(); hudTaps.cancel(); closeSlots(); hudPointers.clear(); hintUntil = 0;
                LaunchControls.cancel(activity);
            }
            fallback=nextFallback;
            if(fallback) {
                boolean play=next.mode==GAMEPLAY || next.mode==VEHICLE;
                next=new Context(Math.max(1,next.epoch),play?next.mode:MENU,
                        (play?GAMEPLAY_INPUT|RELATIVE_LOOK:UI_NAVIGATION)|(next.capabilities&PAUSE_BACKDROP),(1L<<25)-1,next.frame);
            }
            if (eligible != nextEligible || context.epoch != next.epoch || context.mode != next.mode
                    || context.capabilities != next.capabilities || context.actions != next.actions
                    || context.knownActions != next.knownActions || context.weaponCount != next.weaponCount
                    || context.grenadeCount != next.grenadeCount || context.hudValidMask != next.hudValidMask
                    || !Arrays.equals(context.mapRect,next.mapRect) || !Arrays.equals(context.ammoRect,next.ammoRect)
                    || !Arrays.equals(context.objectiveRect,next.objectiveRect)
                    || !Arrays.equals(context.useRect,next.useRect) || !Arrays.equals(context.skillRect,next.skillRect)
                    || !Arrays.equals(context.experienceRect,next.experienceRect)
                    || !Arrays.equals(context.challengeRect,next.challengeRect)
                    || context.weaponSlots != next.weaponSlots) layoutDirty = true;
            context = next; eligible = nextEligible;
            // Main menu versus a loaded world, for the restart warning.
            if (context.mode == FRONTEND) inWorld = false;
            else if (context.mode == GAMEPLAY || context.mode == VEHICLE || context.mode == CINEMATIC
                    || context.mode == DEAD) inWorld = true;
            if (!context.menu()) clearMenuMore();
            if (eligible) updateMenuMore(now);
            if (layoutEditPending && eligible && context.gameplay()) {
                layoutEditPending = false; editing = true; layoutDirty = true;
                showHint("Drag the buttons; tap Done to save", "Перетащите кнопки; нажмите «Готово», чтобы сохранить",HINT_LAYOUT);
            }
            // Focus/IME/settings temporarily suppress gestures while retaining the
            // native route, so an old overlay cannot appear over a modal keyboard.
            publishRoute(true);
            if (eligible) { input.setContext(next); input.heartbeat(); }
            updateSensor();
            fallbackYaw=fallbackPitch=0;
            if (layoutDirty) { buildButtons(); invalidate(); }
            if ((hintKind == HINT_MAP && (!context.gameplay() || mapTarget.isEmpty() || !context.allows(MAP)))
                    || (hintKind == HINT_WEAPON && (context.mode != GAMEPLAY || ammoTarget.isEmpty() || context.weaponCount < 2
                    || !context.allows(NEXT_WEAPON) || !context.allows(PREV_WEAPON)))
                    || (hintKind == HINT_MAP_NAVIGATION && !context.has(MAP_GESTURES))
                    || ((hintKind == HINT_SLOTS || hintKind == HINT_SKILL_TAP || hintKind == HINT_STATUS_TAPS)
                    && context.mode != GAMEPLAY)) {
                hintKind = 0; hintUntil = 0; invalidate();
            }
            if (eligible && !editing) {
                int hudAction = hudGesture.poll(now,context);
                if (hudAction >= 0) activate(nextLegacyId++,hudAction,0,0);
                if (hudGesture.slotHoldDue(now,context,dp(14))) openSlots();
                if (!hudGesture.slotsOpen() && slotCells != null) { closeSlots(); invalidate(); }
                confirmRadialHold(now);
                offerHudHint(now);
            }
        } catch (UnsatisfiedLinkError error) {
            linkageFailed = true; eligible = false; buttons.clear();
            invalidate();
            publishRoute(false);
            Log.e(TAG, "Touch JNI is not linked; overlay disabled", error);
        }
        schedule();
    }
    private float dp(float value) { return value * getResources().getDisplayMetrics().density; }
    private void updateGeometry() {
        getLocationOnScreen(screenLocation); surface.getLocationOnScreen(surfaceLocation);
        RectF next = new RectF(surfaceLocation[0] - screenLocation[0],
                surfaceLocation[1] - screenLocation[1],
                surfaceLocation[0] - screenLocation[0] + surface.getWidth(),
                surfaceLocation[1] - screenLocation[1] + surface.getHeight());
        if (!viewport.equals(next)) { cancelInput(); viewport.set(next); layoutDirty=true; applyRenderScale(); }
        next.set(viewport);
        if (!next.intersect(insetL, insetT, getWidth() - insetR, getHeight() - insetB)) next.setEmpty();
        if (!safe.equals(next)) { safe.set(next); layoutDirty=true; }
        layoutScale=Math.min(preferences.scale,Math.max(.75f,(safe.height()/dp(1)-64)/260));
        input.configure(getResources().getDisplayMetrics().density, 60 * preferences.scale,
                preferences.look, preferences.ads, preferences.invertY);
        input.autoSprint(preferences.autoSprint);
        input.aimToggle(preferences.aimToggle);
    }
    private void add(int action, String label, float fromRightDp, float fromBottomDp, float sizeDp) {
        if (action >= 0 && action < LEGACY && !context.allows(action)) return;
        float size = dp(Math.max(48, sizeDp * layoutScale));
        if (safe.width() < size || safe.height() < size) return;
        float x = safe.right - dp(fromRightDp * layoutScale);
        float y = safe.bottom - dp(fromBottomDp * layoutScale);
        float nx = (x - safe.left) / safe.width(), ny = (y - safe.top) / safe.height();
        boolean custom=false;
        // Utility commands have fixed positions and do not share gameplay saved positions.
        if (action >= 0 && action < LEGACY && action != PAUSE && context.gameplay()) {
            custom=customPosition(preferences,action);
            nx = preferences.x(action, nx); ny = preferences.y(action, ny);
        }
        if (preferences.leftHanded && context.gameplay()) nx = 1 - nx;
        x = Math.max(safe.left + size / 2, Math.min(safe.right - size / 2, safe.left + nx * safe.width()));
        y = Math.max(safe.top + size / 2, Math.min(safe.bottom - size / 2, safe.top + ny * safe.height()));
        boolean utility=action<0 || action==PAUSE || action>=LEGACY;
        float left=safe.left, right=safe.right, top=safe.top, bottom=safe.bottom;
        if(context.gameplay()) {
            if(utility) bottom=top+dp(56);
            else {
                top+=dp(64);
                if(preferences.leftHanded) right=safe.left+safe.width()*.65f;
                else left=safe.right-safe.width()*.65f;
            }
        }
        if(utility) size=dp(48);
        float[] placed=TouchLayout.place(x,y,size,left,top,right,bottom,dp(4),dp(4),occupied,
                context.gameplay() && !custom ? hudReserved : null);
        if(placed!=null) {
            occupied.add(placed);
            buttons.add(new Button(action,label,new RectF(placed[0],placed[1],placed[2],placed[3]),context.gameplay()?dp(2):0));
        }
    }
    static boolean customPosition(TouchPreferences preferences,int action) {
        // Distinct fallbacks distinguish absence from an explicit position,
        // including a saved position identical to the original default.
        return preferences.x(action,0)==preferences.x(action,1)
                || preferences.y(action,0)==preferences.y(action,1);
    }
    /**
     * Without a native ammo bar or reload observation the button is the only
     * route. Otherwise the native route already withholds Reload for a full or
     * unreloadable magazine and while reloading, so the preference decides.
     * The caller still requires the action to be allowed outside the editor.
     */
    static boolean reloadButtonShown(Context c,boolean ammoBar,boolean editing,boolean preference) {
        return editing || c.mode != GAMEPLAY || !ammoBar || !c.known(RELOAD) || preference;
    }
    static boolean confirmedUnarmed(Context c) {
        // A special/transient readied-count zero with an equipped weapon must
        // keep its fallback. Native known Fire=false also proves Pawn.Weapon=null.
        return c.mode==GAMEPLAY && c.weaponCount==0 && c.known(FIRE) && !c.allows(FIRE);
    }
    private void addGameplay(int action,String text) {
        if (action >= 0 && !editing && !context.allows(action)) return;
        float[] anchor = TouchLayout.gameplayAnchor(action);
        boolean custom = action >= 0 && customPosition(preferences,action);
        float nx = custom ? preferences.x(action,anchor[0]) : anchor[0];
        float ny = custom ? preferences.y(action,anchor[1]) : anchor[1];
        float size = TouchLayout.gameplaySize(action,safe.height(),dp(1),layoutScale);
        float[] placed = TouchLayout.placeGameplay(action,nx,ny,size,safe.left,safe.top,safe.right,safe.bottom,
                dp(1),preferences.leftHanded,custom,occupied,hudReserved);
        if (placed != null) {
            occupied.add(placed);
            buttons.add(new Button(action,text,new RectF(placed[0],placed[1],placed[2],placed[3]),
                    TouchLayout.gameplayHitSlop(action,dp(1))));
        }
    }
    private static float[] hudBox(RectF target) {
        return target.isEmpty() ? null : new float[]{target.left,target.top,target.right,target.bottom};
    }
    private void hudTarget(RectF target,float[] rect,boolean valid) {
        target.setEmpty();
        if (!valid) return;
        float cx=viewport.left+(rect[0]+rect[2])*viewport.width()/2;
        float cy=viewport.top+(rect[1]+rect[3])*viewport.height()/2;
        float hw=Math.max(dp(22),(rect[2]-rect[0])*viewport.width()/2);
        float hh=Math.max(dp(22),(rect[3]-rect[1])*viewport.height()/2);
        target.set(cx-hw,cy-hh,cx+hw,cy+hh);
        if (!target.intersect(safe)) target.setEmpty();
    }
    private void addGameplayCluster() {
        addCluster(new int[]{FIRE,AIM,GRENADE,JUMP},new String[]{label("Fire", "Огонь"),label("Aim", "Прицел"),
                label("Grenade", "Граната"),label("Jump", "Прыжок")},editing ? (1L<<26)-1 : context.actions);
    }
    /**
     * A vehicle seat replaces the on-foot cluster (review 4.4): the driver keeps
     * Fire and gets Boost and Brake in the Grenade and Jump places; a turret seat
     * shows its Aim (held alternate fire), an attached rider its Grenade. Exit
     * moves with the group in the Action place, away from Fire, Boost and Brake.
     */
    private void addSeatCluster() {
        boolean rider=!editing && !context.allows(VEHICLE_BOOST) && context.allows(GRENADE);
        addCluster(new int[]{FIRE,AIM,rider ? GRENADE : VEHICLE_BOOST,VEHICLE_BRAKE,EXIT_VEHICLE},
                new String[]{label("Fire", "Огонь"),label("Aim", "Прицел"),
                        rider ? label("Grenade", "Граната") : label("Boost", "Ускорить"),label("Brake", "Тормоз"),
                        label("Exit", "Выйти")},
                editing ? bit(FIRE)|bit(VEHICLE_BOOST)|bit(VEHICLE_BRAKE)|bit(EXIT_VEHICLE) : context.actions);
    }
    private void addCluster(int[] actions,String[] labels,long mask) {
        boolean custom=false;for(int action:actions) custom |= customPosition(preferences,action);
        float[][] group=custom ? null : TouchLayout.gameplayCluster(safe.left,safe.top,safe.right,safe.bottom,
                dp(1),layoutScale,preferences.leftHanded,actions,mask,occupied,hudReserved);
        if (group == null) {
            for(int i=0;i<actions.length;++i) if ((mask & bit(actions[i])) != 0) addGameplay(actions[i],labels[i]);
            return;
        }
        for(int i=0;i<actions.length;++i) if(group[i]!=null) {
            occupied.add(group[i]);
            buttons.add(new Button(actions[i],labels[i],new RectF(group[i][0],group[i][1],group[i][2],group[i][3]),dp(2)));
        }
    }
    private void buildButtons() {
        layoutDirty=false; buttons.clear(); occupied.clear(); hudReserved.clear();
        hudTarget(mapTarget,context.mapRect,context.hudMap());
        hudTarget(ammoTarget,context.ammoRect,context.hudAmmo());
        hudTarget(useTarget,context.useRect,context.hudUse() && context.known(USE) && context.allows(USE));
        hudTarget(skillTarget,context.skillRect,context.hudSkill());
        hudTarget(experienceTarget,context.experienceRect,context.hudExperience());
        hudTarget(challengeTarget,context.challengeRect,context.hudChallenge());
        hudTarget(objectiveTarget,context.objectiveRect,context.hudObjectives());
        if (!eligible) return;
        if (context.gameplay()) {
            // HUD follows the full Surface; insets and left-hand mode affect
            // controls only. Never rewrite a player's deliberately saved layout.
            TouchLayout.reserveGameplayHud(viewport.left,viewport.top,viewport.right,viewport.bottom,hudReserved,
                    !ammoTarget.isEmpty() || confirmedUnarmed(context),context.hudObjectivesKnown() && context.hudMap());
            if (context.hudObjectives()) TouchLayout.reserveSurfaceRect(context.objectiveRect,
                    viewport.left,viewport.top,viewport.right,viewport.bottom,hudReserved);
            // The Action caption comes and goes with the aimed object, as does a
            // challenge notice. They stay tappable but never displace controls.
            for (RectF rect : new RectF[]{mapTarget,ammoTarget,skillTarget,experienceTarget}) if (!rect.isEmpty())
                hudReserved.add(new float[]{rect.left,rect.top,rect.right,rect.bottom});
            addGameplay(PAUSE,label("Pause", "Пауза"));
            addGameplay(editing ? DONE : MORE,editing ? label("Done", "Готово") : label("More", "Ещё"));
            boolean seat=context.mode == VEHICLE;
            if (seat) addSeatCluster();
            else addGameplayCluster();
            // A seat shows Reload only where the engine offers it (an attached
            // rider's own weapon); its layout editor never adds on-foot buttons.
            if ((!seat || !editing) && reloadButtonShown(context,!ammoTarget.isEmpty(),editing,preferences.reloadButton))
                addGameplay(RELOAD,label("Reload", "Зарядить"));
            // A seat's Exit is in its cluster; Horn and Switch seat are in the radial.
            if (!seat) addGameplay(USE,label("Use", "Действие"));
        } else if (context.mode == CINEMATIC && context.has(CAN_SKIP)) {
            // The cinematic replaces the cluster: Pause where it always is,
            // Skip only while the original SkipCinematic is available.
            addGameplay(PAUSE,label("Pause", "Пауза"));
            add(SKIP, label("Skip", "Пропуск"), 48, 40, 64);
        } else if (context.menu()) {
            add(BACK, label("Back", "Назад"), 36, 36, 56);
            float size=dp(44), x=context.has(DEBUG_CANVAS) ? safe.left+safe.width()*.28f : safe.left+dp(26);
            // Keep the Debug footer caption below the full button plus a 4dp gap.
            float y=context.has(DEBUG_CANVAS) ? safe.bottom-dp(36+44+4) : safe.top+size/2;
            float[] placed=TouchLayout.place(x,y,size,safe.left,safe.top,safe.right,safe.bottom,dp(4),dp(4),occupied);
            if (placed!=null) {
                occupied.add(placed);
                buttons.add(new Button(MORE,label("More", "Ещё"),new RectF(placed[0],placed[1],placed[2],placed[3]),0));
            }
            add(CONFIRM,label("Select", "Выбрать"),110,36,56);
            if (expanded && context.has(UI_NAVIGATION)) {
                int[] commands=menuCommands(context,fallback,touchExploration());
                for (int i=0;i<commands.length;i++) {
                    float[] at=menuCommandPlace(commands,i);
                    add(commands[i],menuCommandLabel(commands[i]),at[0],at[1],48);
                }
            }
        }
        if(radialOpen) buildRadial();
    }
    /**
     * More-panel keys in a menu: only what no gesture expresses. A drag scrolls
     * lists and a sideways swipe changes tabs (native; the fallback swipes the
     * D-pad), so X/Y stay (Debug: X/Y/R/L) plus the fallback's shoulder keys.
     * TalkBack consumes swipes, so with touch exploration every key stays.
     */
    static int[] menuCommands(Context c,boolean fallback,boolean touchExploration) {
        if (c.has(DEBUG_CANVAS)) return touchExploration
                ? new int[]{UI_UP,UI_DOWN,UI_LEFT,UI_RIGHT,RELOAD,NEXT_WEAPON,SKILL,SPRINT,PREV_WEAPON,INVENTORY}
                : new int[]{RELOAD,NEXT_WEAPON,SKILL,SPRINT};
        if (fallback) return touchExploration
                ? new int[]{UI_UP,UI_DOWN,UI_LEFT,UI_RIGHT,LEGACY+99,LEGACY+100,LEGACY+102,LEGACY+103}
                : new int[]{LEGACY+99,LEGACY+100,LEGACY+102,LEGACY+103};
        return touchExploration
                ? new int[]{UI_UP,UI_DOWN,UI_LEFT,UI_RIGHT,UI_ACTION,UI_OTHER,UI_PREVIOUS_TAB,UI_NEXT_TAB}
                : new int[]{UI_ACTION,UI_OTHER};
    }
    /** dp from the right and bottom edges: two columns beside Select, or the full TalkBack panel. */
    static float[] menuCommandPlace(int[] commands,int index) {
        if (commands.length == 0 || commands[0] != UI_UP) return new float[]{176+60*(index/2),36+60*(index%2)};
        switch (commands[index]) {
            case UI_UP: return new float[]{176,96};
            case UI_DOWN: return new float[]{176,36};
            case UI_LEFT: return new float[]{236,36};
            case UI_RIGHT: return new float[]{116,36};
            case PREV_WEAPON: return new float[]{236,156};
            case INVENTORY: return new float[]{176,156};
            default: return new float[]{300+60*((index-4)/2),36+60*((index-4)%2)};
        }
    }
    private String menuCommandLabel(int action) {
        switch (action) {
            case UI_UP: return label("Up", "Вверх");
            case UI_DOWN: return label("Down", "Вниз");
            case UI_LEFT: return label("Left", "Влево");
            case UI_RIGHT: return label("Right", "Вправо");
            case RELOAD: return label("Action X", "Действие X");
            case NEXT_WEAPON: return label("Action Y", "Действие Y");
            case SKILL: return label("Action R", "Действие R");
            case SPRINT: return label("Action L", "Действие L");
            case UI_ACTION: case LEGACY+99: return label("Action", "Действие");
            case UI_OTHER: case LEGACY+100: return label("Other", "Другое");
            case PREV_WEAPON: case UI_PREVIOUS_TAB: case LEGACY+102: return label("Tab −", "Вкладка −");
            default: return label("Tab +", "Вкладка +");
        }
    }
    private boolean touchExploration() {
        AccessibilityManager manager=(AccessibilityManager)activity.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
        return manager != null && manager.isTouchExplorationEnabled();
    }
    private void buildRadial() {
        radialButtons.clear();
        for(int i=0;i<radialCount;i++) {
            int a=radial[i];
            if (!radialAvailable(a)) continue;
            double angle=i*Math.PI*2/radialCount-Math.PI/2;
            float x=radialX+(float)Math.cos(angle)*radialRadius,y=radialY+(float)Math.sin(angle)*radialRadius,s=radialItemRadius;
            radialButtons.add(new Button(a,radialLabel(a,russian),new RectF(x-s,y-s,x+s,y+s),dp(2)));
        }
    }
    private boolean radialAvailable(int action) { return radialAvailable(context,fallback,action); }
    static boolean radialAvailable(Context context,boolean fallback,int action) {
        return context.allows(action) && !(fallback && action == PREV_WEAPON)
                && !(action == CHAT && (fallback || !context.gameplay()))
                && !(context.weaponCount >= 0 && context.weaponCount < 2 && (action == NEXT_WEAPON || action == PREV_WEAPON));
    }
    static int[] radialActions(Context context) { return context.mode == VEHICLE ? RADIAL_VEHICLE : RADIAL; }
    /**
     * The radial adapts to the ways that already work (review section 5): no
     * unavailable item, no Map while the minimap tap opens it, no Reload while
     * its button or the ammo strip reloads; seat items exist only in a seat and
     * Give up only where FFYL offers it. Fewer sectors make larger targets.
     */
    static int[] radialItems(Context context,boolean fallback,boolean mapTap,boolean reloadElsewhere) {
        int[] base=radialActions(context),items=new int[base.length];
        int count=0;
        if (!context.usable()) return new int[0];
        for (int action:base) {
            if (!radialAvailable(context,fallback,action) || (action == MAP && mapTap)
                    || (action == RELOAD && reloadElsewhere)) continue;
            items[count++]=action;
        }
        return Arrays.copyOf(items,count);
    }
    static String radialLabel(int action,boolean ru) {
        switch (action) {
            case NEXT_WEAPON: return ru ? "Оружие" : "Weapon";
            case SKILL: return ru ? "Навык" : "Skill";
            case CROUCH: return ru ? "Присесть" : "Crouch";
            case MELEE: return ru ? "Удар" : "Melee";
            case INVENTORY: return ru ? "Рюкзак" : "Inventory";
            case MAP: return ru ? "Карта" : "Map";
            case PREV_WEAPON: return ru ? "Предыдущее" : "Previous";
            case RELOAD: return ru ? "Зарядить" : "Reload";
            case SUICIDE: return ru ? "Сдаться" : "Give up";
            case CHAT: return ru ? "Чат" : "Chat";
            case VEHICLE_HORN: return ru ? "Сигнал" : "Horn";
            case SWITCH_SEATS: return ru ? "Пересесть" : "Switch seat";
            case LOOK_BACK: return ru ? "Оглянуться" : "Look back";
            case DRIVER_FIRE_TURRET: return ru ? "Турель" : "Turret";
            default: return "";
        }
    }
    /** Radial items a second finger holds like a button (Turret fire, Look back). */
    static boolean radialHold(int action) { return action == LOOK_BACK || action == DRIVER_FIRE_TURRET; }
    /** Radial items that act only after their sector is held (Give up). */
    static boolean radialConfirm(int action) { return action == SUICIDE; }
    /** Item radius: 25dp at nine sectors, growing as fewer share the ring, up to 32dp. */
    static float radialItemRadius(float ringRadius,int count,float density) {
        float spacing=(float)(Math.PI*ringRadius/Math.max(1,count))-4*density;
        return Math.max(25*density,Math.min(32*density,spacing));
    }
    private void openRadial(int id,float x,float y) {
        // Capture the items once: a live availability change must not rotate
        // the other sectors underneath an already held finger.
        boolean mapTap=!fallback && !mapTarget.isEmpty() && context.allows(MAP);
        boolean ammoBar=!ammoTarget.isEmpty();
        boolean reloadElsewhere=reloadButtonShown(context,ammoBar,false,preferences.reloadButton)
                || (!fallback && context.mode == GAMEPLAY && ammoBar && context.known(RELOAD));
        int[] items=radialItems(context,fallback,mapTap,reloadElsewhere);
        if (items.length == 0) return;
        radialOpen=true; radialPointer=id; radialSelection=-1; radialDragged=false; radialSelectedAt=0;
        radialStartX=x; radialStartY=y; radialStarted=SystemClock.uptimeMillis();
        radial=items; radialCount=items.length;
        radialRadius=Math.min(dp(94),Math.min(safe.width(),safe.height())/2-dp(30));
        radialItemRadius=radialItemRadius(radialRadius,radialCount,dp(1));
        float margin=radialRadius+radialItemRadius+dp(2);
        radialX=Math.max(safe.left+margin,Math.min(safe.right-margin,x));
        radialY=Math.max(safe.top+margin,Math.min(safe.bottom-margin,y));
        buildRadial(); haptic(HapticFeedbackConstants.VIRTUAL_KEY);
    }
    private void closeRadial() { radialOpen=false;radialTapMode=false;radialPointer=-1;radialSelection=-1;radialSelectedAt=0;radialButtons.clear(); }
    /** Give up acts once the finger has stayed on its sector for a second. */
    private void confirmRadialHold(long now) {
        if (!radialOpen || radialPointer<0 || radialSelection<0 || radialSelectedAt==0
                || !radialConfirm(radial[radialSelection])) return;
        if (now-radialSelectedAt < CONFIRM_HOLD_MS) { invalidate(); return; }
        int action=radial[radialSelection];
        closeRadial();
        if (radialAvailable(action)) activate(nextLegacyId++,action,0,0);
        haptic(HapticFeedbackConstants.LONG_PRESS);
        invalidate();
    }
    private void openSlots() {
        slotCells=TouchLayout.slotCells(new float[]{ammoTarget.left,ammoTarget.top,ammoTarget.right,ammoTarget.bottom},
                safe.left,safe.top,safe.right,safe.bottom,dp(1));
        slotSelection=-1;
        if (slotCells == null) { hudGesture.cancel(); return; }
        haptic(HapticFeedbackConstants.LONG_PRESS);
        invalidate();
    }
    private void closeSlots() { slotCells=null; slotSelection=-1; }
    static String slotTypeLabel(int type,boolean ru) {
        switch (type) {
            case 1: return ru ? "Пистолет" : "Pistol";
            case 2: return ru ? "Дробовик" : "Shotgun";
            case 3: return ru ? "ПП" : "SMG";
            case 4: return ru ? "Снайперская" : "Sniper";
            case 5: return ru ? "Штурмовая" : "Rifle";
            case 6: return ru ? "РПГ" : "Launcher";
            default: return "";
        }
    }
    private void clearMenuMoreHold() { menuMorePointer=-1;menuMoreButton=null;menuMoreStarted=menuMoreEpoch=0; }
    private void clearMenuMore() { clearMenuMoreHold();expanded=false;menuExpandedUntil=0; }
    private void openMenuCommands(long now) {
        expanded=true;menuExpandedUntil=now+6000;menuMoreStarted=0;
        buildButtons();invalidate();
    }
    private void updateMenuMore(long now) {
        // No delayed callback: the exact captured Button and epoch bind the hold
        // to its pointer sequence, even when Android later reuses that pointer ID.
        if (menuMorePointer>=0 && menuMoreStarted>0 && menuMoreEpoch==context.epoch
                && menuPointer==menuMorePointer && utilityPointers.get(menuMorePointer)==menuMoreButton
                && now-menuMoreStarted>=300) {
            openMenuCommands(now);haptic(HapticFeedbackConstants.LONG_PRESS);
        }
        if (expanded && now>=menuExpandedUntil && menuPointer<0) {
            expanded=false;menuExpandedUntil=0;layoutDirty=true;invalidate();
        }
    }
    private void menuMoreHint() {
        if (context.has(MAP_GESTURES)) showHint("Drag the map, pinch to zoom. Waypoint marks the centre; hold a place to mark it",
                "Двигайте карту, щипок — масштаб. «Маршрутная точка» — метка в центре, удержание — под пальцем",HINT_MAP_NAVIGATION);
        else if (context.has(DEBUG_CANVAS)) showHint("Tap rows; swipe lists and tabs. Hold More for actions", "Тапы — выбор; свайпы — списки и вкладки. Удерживайте «Ещё» для действий",HINT_DEBUG_MORE);
        else showHint("Tap to select, tap again to change. Drag lists, swipe sideways for tabs; hold More for actions",
                "Тап — выбор, повторный — изменение. Ведите по списку, свайп вбок — вкладки; удерживайте «Ещё» для действий",HINT_MENU_MORE);
    }
    private Button hit(float x, float y) {
        if(radialOpen) for(Button b:radialButtons) if(b.contains(x,y)) return b;
        // Visible neighbours win before the enlarged Action target's outer ring.
        for (int i = buttons.size()-1; i >= 0; --i) {
            Button b=buttons.get(i);
            if(TouchLayout.circleContains(x,y,b.rect.centerX(),b.rect.centerY(),b.rect.width()/2,
                    b.action==USE?dp(2):b.hitSlop)) return b;
        }
        if(safe.contains(x,y)) for(Button b:buttons) if(b.action==USE && b.contains(x,y)) return b;
        return null;
    }
    private void showHint(String english,String ru,int kind) {
        if (preferences.hintSeen(kind)) return;
        hintText=label(english,ru); hintKind=kind; hintUntil=SystemClock.uptimeMillis()+4500;
        invalidate(); postInvalidateDelayed(4500);
    }
    private void moreHint() { showHint("Crouch, melee, inventory: hold More and drag", "Присесть, удар, рюкзак: удерживайте «Ещё» и сдвиньте палец",HINT_GAME_MORE); }
    private void offerHudHint(long now) {
        if (!context.gameplay() || fallback || radialOpen || now < hintUntil) return;
        if (!preferences.mapTapHintSeen && !mapTarget.isEmpty() && context.allows(MAP))
            showHint("Tap the minimap to open the map", "Тап по мини-карте открывает карту",HINT_MAP);
        else if (!preferences.weaponGestureHintSeen && context.mode == GAMEPLAY && context.weaponCount >= 2 && !ammoTarget.isEmpty()
                && context.allows(NEXT_WEAPON) && context.allows(PREV_WEAPON))
            showHint("Ammo: swipe to switch; double-tap returns after a switch", "Патроны: свайп — смена; двойной тап — возврат после смены",HINT_WEAPON);
        else if (!preferences.weaponSlotsHintSeen && preferences.weaponGestureHintSeen && context.mode == GAMEPLAY
                && context.weaponCount >= 2 && !ammoTarget.isEmpty() && context.slotsKnown())
            showHint("Hold the ammo strip to pick a weapon slot", "Удерживайте полосу патронов, чтобы выбрать слот оружия",HINT_SLOTS);
        else if (!preferences.skillTapHintSeen && context.mode == GAMEPLAY && !skillTarget.isEmpty() && context.allows(SKILL))
            showHint("Tap the skill icon to use your action skill", "Тап по значку навыка применяет навык",HINT_SKILL_TAP);
        else if (!preferences.statusTapsHintSeen && context.mode == GAMEPLAY && !objectiveTarget.isEmpty()
                && !experienceTarget.isEmpty() && context.allows(LOGS) && context.allows(SKILLS))
            showHint("Tap the missions for the log, the XP bar for skills", "Тап по заданиям — журнал, по полосе опыта — навыки",HINT_STATUS_TAPS);
    }
    private void drawSlots(Canvas canvas) {
        if (slotCells == null || !hudGesture.slotsOpen()) return;
        paint.setTextAlign(Paint.Align.CENTER);
        for (int i=0;i<slotCells.length;++i) {
            float[] c=slotCells[i];
            boolean selectable=TouchInput.HudGesture.slotSelectable(context,i);
            boolean equipped=context.equippedSlot()==i+1, chosen=i==slotSelection && selectable;
            boolean empty=context.slotsKnown() && !context.slotOccupied(i);
            paint.setStyle(Paint.Style.FILL); paint.setColor(chosen ? 0xFFFFD569 : Color.BLACK);
            paint.setAlpha(chosen ? 255 : empty ? 120 : 210);
            canvas.drawRoundRect(c[0],c[1],c[2],c[3],dp(10),dp(10),paint);
            if (equipped) {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(2)); paint.setColor(0xFFFFD569); paint.setAlpha(255);
                canvas.drawRoundRect(c[0],c[1],c[2],c[3],dp(10),dp(10),paint);
                paint.setStyle(Paint.Style.FILL);
            }
            int ink=chosen ? Color.BLACK : Color.WHITE, alpha=selectable || equipped ? 255 : 110;
            paint.setColor(ink); paint.setAlpha(alpha); paint.setTextSize(dp(18));
            float cx=(c[0]+c[2])/2;
            canvas.drawText(Integer.toString(i+1),cx,c[1]+dp(24),paint);
            String type=empty ? "—" : slotTypeLabel(context.slotType(i),russian);
            if (!type.isEmpty()) {
                paint.setTextSize(dp(10));
                float measured=paint.measureText(type),available=c[2]-c[0]-dp(6);
                if (measured > available) paint.setTextSize(dp(10)*available/measured);
                canvas.drawText(type,cx,c[3]-dp(10),paint);
            }
        }
    }
    private void drawHint(Canvas canvas) {
        if (hintText == null || SystemClock.uptimeMillis() >= hintUntil) return;
        paint.setTextSize(dp(14)); paint.setTextAlign(Paint.Align.CENTER); paint.setStyle(Paint.Style.FILL);
        float cy=safe.top+dp(74);
        // The pill shifts or narrows to stay off the minimap, objectives and challenge notice.
        float[] span=TouchLayout.hintSpan(safe.centerX(),paint.measureText(hintText)+dp(24),safe.left+dp(16),safe.right-dp(16),
                cy-dp(18),cy+dp(18),dp(8),hudBox(mapTarget),hudBox(objectiveTarget),hudBox(challengeTarget));
        if (span == null || span[1]-span[0] <= dp(48)) return; // Unseen: offered again later.
        float measured=paint.measureText(hintText);
        if (measured > span[1]-span[0]-dp(24)) paint.setTextSize(dp(14)*(span[1]-span[0]-dp(24))/measured);
        float cx=(span[0]+span[1])/2;
        paint.setColor(Color.BLACK); paint.setAlpha(210);
        canvas.drawRoundRect(span[0],cy-dp(18),span[1],cy+dp(18),dp(10),dp(10),paint);
        paint.setColor(Color.WHITE); paint.setAlpha(255);
        canvas.drawText(hintText,cx,cy-(paint.ascent()+paint.descent())/2,paint);
        if (hintKind > 0 && !preferences.hintSeen(hintKind)) preferences.markHintSeen(hintKind);
    }
    @Override protected void onDraw(Canvas canvas) {
        if (!eligible) return;
        canvas.save(); canvas.clipRect(safe);
        if (input.moving()) {
            boolean running=(input.held()&bit(SPRINT))!=0;
            paint.setColor(running ? 0xFFFFD569 : Color.WHITE); paint.setAlpha(90); paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            canvas.drawCircle(input.stickX(), input.stickY(), dp(60*preferences.scale), paint);
            paint.setStyle(Paint.Style.FILL); paint.setAlpha(110);
            canvas.drawCircle(input.stickX()+input.moveX()*dp(48*preferences.scale),
                    input.stickY()-input.moveY()*dp(48*preferences.scale), dp(18), paint);
            if (running) {
                paint.setAlpha(255); paint.setTextSize(dp(12)); paint.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(label("RUN", "БЕГ"),input.stickX(),input.stickY()+dp(5),paint);
            }
        }
        for (Button b : buttons) {
            boolean held = b.action >= 0 && b.action<LEGACY && ((input.held() & bit(b.action)) != 0
                    || (b.action==USE && legacyKeys.get(99)!=null));
            paint.setStyle(Paint.Style.FILL); paint.setColor(Color.BLACK);
            paint.setAlpha((int)(255*(held || editing ? .72f : preferences.opacity)));
            canvas.drawCircle(b.rect.centerX(),b.rect.centerY(),b.rect.width()/2,paint);
            paint.setColor(held ? 0xFFFFD569 : Color.WHITE); paint.setAlpha(held || editing ? 255 : 180);
            paint.setTextSize(dp(10)); paint.setTextAlign(Paint.Align.CENTER);
            float measured=paint.measureText(b.label),available=b.rect.width()-dp(8);
            if (measured > available) paint.setTextSize(dp(10)*available/measured);
            canvas.drawText(b.label, b.rect.centerX(), b.rect.centerY()+dp(4), paint);
        }
        if(radialOpen) {
            paint.setStyle(Paint.Style.FILL);paint.setColor(0xE6222830);paint.setAlpha(235);
            canvas.drawCircle(radialX,radialY,radialRadius+radialItemRadius+dp(4),paint);
            for(Button b:radialButtons) {
                boolean selected=radialSelection>=0 && radial[radialSelection]==b.action;
                paint.setColor(selected?0xFFFFD569:0xFF454F5D);canvas.drawCircle(b.rect.centerX(),b.rect.centerY(),b.rect.width()/2,paint);
                if (selected && radialConfirm(b.action) && radialSelectedAt>0) {
                    // Give up fills a ring while it is held; lifting early cancels.
                    float progress=Math.min(1f,(SystemClock.uptimeMillis()-radialSelectedAt)/(float)CONFIRM_HOLD_MS);
                    paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(3));paint.setColor(0xFFE0483A);
                    RectF ring=new RectF(b.rect);ring.inset(-dp(3),-dp(3));
                    canvas.drawArc(ring,-90,360*progress,false,paint);
                    paint.setStyle(Paint.Style.FILL);
                }
                paint.setColor(selected?Color.BLACK:Color.WHITE);paint.setTextSize(dp(10));paint.setTextAlign(Paint.Align.CENTER);
                float measured=paint.measureText(b.label),available=b.rect.width()-dp(8);
                if (measured > available) paint.setTextSize(dp(10)*available/measured);
                canvas.drawText(b.label,b.rect.centerX(),b.rect.centerY()+dp(4),paint);
            }
            paint.setColor(Color.WHITE);paint.setTextSize(dp(10));
            canvas.drawText(label("Choose", "Выберите"),radialX,radialY-dp(3),paint);
            canvas.drawText(label("action", "действие"),radialX,radialY+dp(12),paint);
        }
        drawSlots(canvas);
        drawHint(canvas);
        canvas.restore();
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            streamOwned = event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)
                    && eligible && (editing || context.gameplay()
                    || hit(event.getX(), event.getY()) != null
                    || (context.menu() && (fallback || context.has(DIRECT_UI | UI_TAP_TEST | UI_WIDGETS)) && viewport.contains(event.getX(), event.getY())));
        }
        if (!streamOwned) return false;
        if (action == MotionEvent.ACTION_CANCEL) {
            cancelInput(); streamOwned = false; return true;
        }
        if (!eligible) {
            if (action == MotionEvent.ACTION_UP) streamOwned = false;
            return true; // Never hand half a captured gesture to the underlying game.
        }
        if (editing) {
            editEvent(event);
        } else if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            down(event.getPointerId(index), event.getX(index), event.getY(index));
        } else if (action == MotionEvent.ACTION_MOVE) {
            moveVisual(moveVisualBefore);
            for (int h = 0; h < event.getHistorySize(); h++) for (int i = 0; i < event.getPointerCount(); i++)
                move(event.getPointerId(i), event.getHistoricalX(i,h), event.getHistoricalY(i,h));
            for (int i = 0; i < event.getPointerCount(); i++) move(event.getPointerId(i), event.getX(i), event.getY(i));
            moveVisual(moveVisualAfter);
            // Look drags draw nothing here. Redrawing this full-screen layer per
            // touch sample costs GPU time beside the game, and the compositor then
            // waits for the game's GPU work before it takes the game's frame.
            if (Arrays.equals(moveVisualBefore, moveVisualAfter)) return true;
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int id = event.getPointerId(index);
            float x=event.getX(index),y=event.getY(index);
            if(id==radialPointer) {
                int selected=radialDragged?TouchLayout.radialSector(x,y,radialX,radialY,radialRadius,radialCount):-1;
                boolean tap=!radialDragged && SystemClock.uptimeMillis()-radialStarted<300;
                if (tap && preferences.radialTap && !fallback) {
                    // Stay open; the next tap picks an item, More or elsewhere closes.
                    radialPointer=-1; radialTapMode=true; radialSelection=-1;
                } else {
                    closeRadial();
                    // Give up acts only from confirmRadialHold; an early release explains why.
                    if (selected >= 0 && radialConfirm(radial[selected]))
                        showHint("Hold Give up for a second", "Удерживайте «Сдаться» секунду",0);
                    else if (selected >= 0 && radialAvailable(radial[selected])) activate(id,radial[selected],x,y);
                    else if (tap) moreHint();
                }
            } else if (hudGesture.owns(id)) {
                RectF rect=hudPointers.get(id); hudPointers.remove(id);
                boolean inside=rect != null && (hudGesture.zone(id) == TouchInput.HudGesture.AMMO_ZONE ? safe.contains(x,y) : rect.contains(x,y));
                int slot=hudGesture.slotsOpen() ? TouchLayout.slotAt(x,y,slotCells,dp(1)) : -1;
                int command=hudGesture.up(id,x,y,SystemClock.uptimeMillis(),context,inside,dp(14),slot);
                if (!hudGesture.slotsOpen()) closeSlots();
                if (command >= 0) activate(id,command,x,y);
            } else if (hudTaps.owns(id)) {
                int command=hudTaps.up(id,x,y,SystemClock.uptimeMillis(),context,dp(14));
                if (command >= 0) activate(id,command,x,y);
            } else if(utilityPointers.get(id)!=null) {
                Button b=utilityPointers.get(id);
                utilityPointers.remove(id);
                if (id==menuMorePointer) {
                    long now=SystemClock.uptimeMillis();
                    boolean held=menuMoreStarted==0 || now-menuMoreStarted>=300;
                    clearMenuMoreHold();
                    if (b.contains(x,y)) {
                        if (held) openMenuCommands(now);
                        else { clearMenuMore();buildButtons();menuMoreHint(); }
                    }
                } else if(b.contains(x,y)) activate(id,b.action,x,y);
            } else if(legacyPointers.get(id)!=null) {
                int key=legacyPointers.get(id);legacyPointers.remove(id);
                boolean owned=false;for(int i=0;i<legacyPointers.size();i++) if(legacyPointers.valueAt(i)==key) owned=true;
                if(!owned) publishLegacyKeys(fallback?legacyHeld:0);
            } else if (uiPointers.get(id) != null) {
                float[] start=uiPointers.get(id);
                if(fallback) {
                    float dx=x-start[0],dy=y-start[1];
                    if(Math.hypot(dx,dy)>dp(18)) activate(id,Math.abs(dx)>Math.abs(dy)?(dx>0?UI_RIGHT:UI_LEFT):(dy>0?UI_DOWN:UI_UP),x,y);
                } else
                sendUi(id, viewport.contains(event.getX(index),event.getY(index)) ? UP : CANCEL,
                        event.getX(index), event.getY(index));
                uiPointers.remove(id);
            } else {
                input.up(id, event.getX(index), event.getY(index));
            }
            if (id == menuPointer) menuPointer = uiPointers.size()>0 ? uiPointers.keyAt(0) : -1;
            if (expanded) menuExpandedUntil=SystemClock.uptimeMillis()+6000;
        }
        if (action == MotionEvent.ACTION_UP) streamOwned = false;
        invalidate(); return true;
    }
    private void down(int id, float x, float y) {
        Button b = hit(x, y);
        if (context.menu()) {
            if (!menuPointerAllowed(context,menuPointer,uiPointers.size(),b!=null)) { cancelInput(); return; }
            if (menuPointer < 0) menuPointer = id;
        }
        if(radialOpen && id!=radialPointer) {
            if(b!=null && radialButtons.contains(b)) {
                // A held item stays pressed under this finger past the radial,
                // through the same native held bit as a button; others act on release.
                if (!fallback && radialHold(b.action)) {
                    input.down(id, TouchInput.BUTTON, b.action, x, y);
                    haptic(HapticFeedbackConstants.VIRTUAL_KEY);return;
                }
                if (radialTapMode && radialPointer<0 && radialConfirm(b.action)) {
                    // Give up still needs its one-second hold: this finger now
                    // owns the radial with that sector selected.
                    radialPointer=id; radialDragged=true; radialSelectedAt=SystemClock.uptimeMillis();
                    for (int i=0;i<radialCount;i++) if (radial[i]==b.action) radialSelection=i;
                    haptic(HapticFeedbackConstants.CLOCK_TICK); return;
                }
                utilityPointers.put(id,b);
                return;
            } else if(b!=null && b.action==MORE) {closeRadial();return;}
            else if(radialPointer<0) {
                // A tap-mode radial is dismissed by the next touch, which does nothing else.
                boolean consume=radialTapMode;
                closeRadial();
                if (consume) return;
            }
        }
        if (b != null) {
            if (b.action == MORE) {
                if(context.gameplay()) openRadial(id,x,y);
                else {
                    utilityPointers.put(id,b);menuMorePointer=id;menuMoreButton=b;
                    menuMoreStarted=SystemClock.uptimeMillis();menuMoreEpoch=context.epoch;
                }
                return;
            }
            if (context.menu()) {
                utilityPointers.put(id,b);
                if (expanded) menuExpandedUntil=SystemClock.uptimeMillis()+6000;
                return;
            }
            if(b.action>=LEGACY) {legacyPointers.put(id,b.action-LEGACY);publishLegacyKeys(fallback?legacyHeld:0);return;}
            if(b.action<0 || b.action==PAUSE) {
                utilityPointers.put(id,b);return;
            }
            // The native held Use bit presses the game's own X button: tap picks
            // up, hold equips, with no virtual controller. Fallback keeps key X.
            if(legacyUse(context,fallback,b.action)) { legacyPointers.put(id,99);publishLegacyKeys(legacyHeld);return; }
            input.down(id, TouchInput.BUTTON, b.action, x, y);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY); return;
        }
        if (context.menu() && (fallback || context.has(DIRECT_UI | UI_TAP_TEST | UI_WIDGETS)) && viewport.contains(x, y)) {
            uiPointers.put(id, new float[]{x, y}); if(!fallback) sendUi(id, DOWN, x, y); return;
        }
        if (context.gameplay() && safe.contains(x, y)) {
            if (!fallback && context.known(USE) && context.allows(USE) && useTarget.contains(x,y)) {
                // The transparent native hint shares the Action button's real
                // press/hold/up path, including pickup and hold-to-equip.
                input.down(id, TouchInput.BUTTON, USE, x, y);
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);return;
            }
            if (!fallback && (mapTarget.contains(x,y) || (context.mode == GAMEPLAY && ammoTarget.contains(x,y)))) {
                long now=SystemClock.uptimeMillis();
                int pending=hudGesture.poll(now,context); if (pending >= 0) activate(nextLegacyId++,pending,0,0);
                boolean map=mapTarget.contains(x,y);
                hudPointers.put(id,new RectF(map ? mapTarget : ammoTarget));
                hudGesture.down(id,map ? TouchInput.HudGesture.MAP_ZONE : TouchInput.HudGesture.AMMO_ZONE,x,y,now,context.epoch,dp(14));
                return;
            }
            float nx = (x-safe.left)/safe.width();
            if (preferences.leftHanded) nx = 1-nx;
            boolean stopTarget = input.sprintLatched() && TouchLayout.circleContains(x,y,
                    input.stickX(),input.stickY(),dp(60*preferences.scale),dp(8));
            if (stopTarget) input.stopRunning();
            boolean movement = stopTarget || (nx < .40f && y > safe.top + safe.height()*.35f);
            int zone = stopTarget || fallback ? 0 : tapZone(x,y);
            if (zone > 0) {
                // Still tap: skill or a status tab. Dragged: the same stick or
                // camera this touch would have started, from where it went down.
                hudTaps.down(id,zone,movement ? TouchInput.MOVEMENT : TouchInput.LOOK,x,y,
                        SystemClock.uptimeMillis(),context.epoch);
                return;
            }
            input.down(id, movement ? TouchInput.MOVEMENT : TouchInput.LOOK, -1, x, y);
        }
    }
    /** Tap-only HUD target under a new touch, while its action is available. */
    private int tapZone(float x,float y) {
        if (context.mode != GAMEPLAY) return 0;
        if (challengeTarget.contains(x,y) && context.allows(CHALLENGES)) return TouchInput.HudTaps.CHALLENGE_ZONE;
        if (skillTarget.contains(x,y) && context.allows(SKILL)) return TouchInput.HudTaps.SKILL_ZONE;
        if (objectiveTarget.contains(x,y) && context.allows(LOGS)) return TouchInput.HudTaps.OBJECTIVES_ZONE;
        if (experienceTarget.contains(x,y) && context.allows(SKILLS)) return TouchInput.HudTaps.EXPERIENCE_ZONE;
        return 0;
    }
    // What onDraw shows that a touch move can change: held buttons, the radial
    // selection, utility holds, and the stick with its knob in whole pixels.
    private void moveVisual(int[] out) {
        long held = input.held();
        boolean moving = input.moving();
        float reach = dp(48 * preferences.scale);
        out[0] = (int) held; out[1] = (int) (held >>> 32);
        out[2] = radialOpen ? radialSelection : -2;
        out[3] = utilityPointers.size();
        out[4] = moving ? Math.round(input.stickX()) : Integer.MIN_VALUE;
        out[5] = moving ? Math.round(input.stickY()) : 0;
        out[6] = moving ? Math.round(input.moveX() * reach) : 0;
        out[7] = moving ? Math.round(input.moveY() * reach) : 0;
        out[8] = slotCells != null ? slotSelection : -2;
    }
    private void move(int id, float x, float y) {
        if (hudGesture.owns(id)) {
            hudGesture.move(id,x,y);
            if (hudGesture.slotsOpen() && slotCells != null) {
                int slot=TouchLayout.slotAt(x,y,slotCells,dp(1));
                if (!TouchInput.HudGesture.slotSelectable(context,slot)) slot=-1;
                if (slot != slotSelection && slot >= 0) haptic(HapticFeedbackConstants.CLOCK_TICK);
                slotSelection=slot;
            }
            return;
        }
        if (hudTaps.owns(id)) {
            float[] drag=hudTaps.move(id,x,y,dp(14));
            if (drag != null) { input.down(id,(int)drag[0],-1,drag[1],drag[2]); input.move(id,x,y); }
            return;
        }
        if (id==menuMorePointer) {
            if (menuMoreButton==null || !menuMoreButton.contains(x,y)) {utilityPointers.remove(id);clearMenuMoreHold();}
            return;
        }
        if(id==radialPointer) {
            if(Math.hypot(x-radialStartX,y-radialStartY)>dp(12)) radialDragged=true;
            int selected=radialDragged?TouchLayout.radialSector(x,y,radialX,radialY,radialRadius,radialCount):-1;
            if(selected>=0 && !radialAvailable(radial[selected])) selected=-1;
            if(selected!=radialSelection) {
                if (selected>=0) haptic(HapticFeedbackConstants.CLOCK_TICK);
                radialSelectedAt=selected>=0 && radialConfirm(radial[selected]) ? SystemClock.uptimeMillis() : 0;
            }
            radialSelection=selected;return;
        }
        if(utilityPointers.get(id)!=null || legacyPointers.get(id)!=null) return;
        float[] p = uiPointers.get(id);
        if (p != null) {
            if(fallback) return;
            p[0]=x; p[1]=y;
            if(!viewport.contains(x,y)) { sendUi(id,CANCEL,x,y); uiPointers.remove(id); }
            else sendUi(id,MOVE,x,y);
        }
        else {
            boolean running=input.sprintLatched(); input.move(id,x,y);
            if (!running && input.sprintLatched()) {
                haptic(HapticFeedbackConstants.LONG_PRESS);
                showHint("Auto-run: pull back or tap the stick to stop", "Автобег: верните стик или коснитесь его, чтобы остановиться",HINT_AUTORUN);
            }
        }
    }
    private void activate(int id,int action,float x,float y) {
        if(action==MORE) {
            if(context.gameplay()) {
                if (touchExploration()) openRadial(-1,x,y); else moreHint();
            } else {clearMenuMore();buildButtons();menuMoreHint();}
            return;
        }
        if(action>=LEGACY || legacyUse(context,fallback,action) || (fallback && action==MAP)) {
            final int key=action>=LEGACY?action-LEGACY:action==USE?99:4;
            final int owner=nextLegacyId++;
            legacyPointers.put(owner,key);publishLegacyKeys(fallback?legacyHeld:0);
            postDelayed(()->{if(legacyPointers.get(owner)!=null) {legacyPointers.remove(owner);publishLegacyKeys(fallback?legacyHeld:0);}},action==MAP?500:150);
            closeRadial();return;
        }
        if(action<0 || !context.allows(action)) return;
        if (action == PAUSE) input.stopRunning();
        closeRadial();
        if (action == AIM) { input.down(id,TouchInput.BUTTON,action,x,y);input.up(id,x,y); }
        else input.pulse(action);
        haptic(HapticFeedbackConstants.VIRTUAL_KEY);
    }
    private void publishLegacyKeys(long held) {
        SparseArray<Boolean> desired=new SparseArray<>();
        for(int a=0;a<=UI_RIGHT;a++) if((held&bit(a))!=0) desired.put(legacyKey(a),true);
        for(int i=0;i<legacyPointers.size();i++) desired.put(legacyPointers.valueAt(i),true);
        for(int i=0;i<legacyKeys.size();i++) if(desired.get(legacyKeys.keyAt(i))==null) LaunchControls.key(activity,legacyKeys.keyAt(i),false);
        for(int i=0;i<desired.size();i++) if(legacyKeys.get(desired.keyAt(i))==null) LaunchControls.key(activity,desired.keyAt(i),true);
        legacyKeys.clear();for(int i=0;i<desired.size();i++) legacyKeys.put(desired.keyAt(i),true);
    }
    private static int legacyKey(int action) {
        switch(action) {
            case FIRE:return -2;case AIM:return -1;case JUMP:case CONFIRM:return 96;
            case CROUCH:case BACK:return 97;case RELOAD:case USE:return 99;case MELEE:return 107;
            case GRENADE:case VEHICLE_BRAKE:return 103;case SKILL:case VEHICLE_BOOST:return 102;
            case SPRINT:case VEHICLE_HORN:return 106;case NEXT_WEAPON:case PREV_WEAPON:return 100;
            case INVENTORY:case MAP:return 4;case PAUSE:case SKIP:return 108;
            case UI_UP:return 19;case UI_DOWN:return 20;
            case UI_LEFT:return 21;case UI_RIGHT:return 22;default:return 97;
        }
    }
    private void sendUi(int id, int phase, float x, float y) {
        // Coordinates refer to the complete render Surface, not the safe layout rectangle.
        bridge.uiPointer(context.epoch, id, phase,
                Math.max(0,Math.min(1,(x-viewport.left)/viewport.width())),
                Math.max(0,Math.min(1,(y-viewport.top)/viewport.height())));
    }
    static boolean menuPointerAllowed(Context context,int activePointer,int uiCount,boolean overlayTarget) {
        if (activePointer < 0 && uiCount == 0) return true;
        // Only the authenticated original Map advertises two-finger gestures.
        // A button owner or a third contact still cancels the entire sequence.
        return context.has(MAP_GESTURES) && uiCount == 1 && !overlayTarget;
    }
    private void editEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            Button b = hit(event.getX(),event.getY());
            if (b != null && b.action == DONE) {
                editing=false; expanded=false; cancelInput(); return;
            }
            if (b != null && b.action >= 0) {
                editButton=b; editPointer=event.getPointerId(0);
                editOffsetX=b.rect.centerX()-event.getX(); editOffsetY=b.rect.centerY()-event.getY();
            }
        } else if (action == MotionEvent.ACTION_MOVE && editButton != null) {
            int i=event.findPointerIndex(editPointer);
            if (i<0) return;
            float cx=Math.max(safe.left+editButton.rect.width()/2,Math.min(safe.right-editButton.rect.width()/2,event.getX(i)+editOffsetX));
            float cy=Math.max(safe.top+editButton.rect.height()/2,Math.min(safe.bottom-editButton.rect.height()/2,event.getY(i)+editOffsetY));
            float nx=(cx-safe.left)/safe.width(), ny=(cy-safe.top)/safe.height();
            if (preferences.leftHanded && editButton.action != PAUSE) nx=1-nx;
            preferences.position(editButton.action,nx,ny);
            layoutChanged = true;
            buildButtons();
        } else if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                && event.getPointerId(index)==editPointer) { saveLayout(); editButton=null; editPointer=-1; }
    }
    private void saveLayout() {
        if (layoutChanged) { layoutChanged = false; preferences.save(); }
    }
    public static float renderScale(Activity owner) {
        return sessionRenderPercent(owner)/100f;
    }
    /** A changed resolution takes effect in the next process, after a restart. */
    private static synchronized int sessionRenderPercent(Activity owner) {
        if (sessionRenderPercent == 0) {
            NativeTouchControls controls=active.get();
            sessionRenderPercent=(controls!=null && controls.activity==owner ? controls.preferences : new TouchPreferences(owner)).renderPercent;
        }
        return sessionRenderPercent;
    }
    public static boolean microphoneEnabled(Activity owner) {
        NativeTouchControls controls=active.get();
        return (controls!=null && controls.activity==owner ? controls.preferences : new TouchPreferences(owner)).microphoneEnabled;
    }
    public static void microphonePermissionResult(Activity owner, int result) {
        if (result == android.content.pm.PackageManager.PERMISSION_GRANTED) return;
        NativeTouchControls controls = active.get();
        TouchPreferences prefs = controls != null && controls.activity == owner
                ? controls.preferences : new TouchPreferences(owner);
        prefs.microphoneEnabled = false;
        prefs.save();
        if (controls != null && controls.activity == owner) controls.applyMicrophone();
    }
    private void applyMicrophone() {
        try {
            if(preferences.microphoneEnabled) {
                activity.getClass().getMethod("JavaCallback_StartVoiceRecording").invoke(activity);
                if(activity.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)!=android.content.pm.PackageManager.PERMISSION_GRANTED)
                    activity.requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO},1);
            } else {
                activity.getClass().getMethod("JavaCallback_StopVoiceRecording").invoke(activity);
                activity.getClass().getMethod("releaseAudioInput").invoke(activity);
            }
        } catch(ReflectiveOperationException error) {
            Log.e(TAG,"Cannot apply microphone setting",error);
            android.widget.Toast.makeText(activity,label("Could not change microphone state", "Не удалось изменить состояние микрофона"),android.widget.Toast.LENGTH_LONG).show();
        }
    }
    private String label(String english, String ru) { return russian ? ru : english; }
    private void applyRenderScale() {
        // Same size as UE3JavaApp's launch callback, which truncates.
        float scale=renderScale(activity);
        if(surface instanceof SurfaceView && surface.getWidth()>0 && surface.getHeight()>0)
            ((SurfaceView)surface).getHolder().setFixedSize((int)(surface.getWidth()*scale),(int)(surface.getHeight()*scale));
    }
    private void offerRenderRestart() {
        renderRestartOffered=preferences.renderPercent;
        if (activity.isFinishing() || activity.isDestroyed() || (restartDialog!=null && restartDialog.isShowing())) return;
        String message=label("Render resolution "+renderRestartOffered+"% applies after the game restarts. Restart now?",
                "Разрешение рендера "+renderRestartOffered+"% применится после перезапуска игры. Перезапустить сейчас?");
        if (inWorld) message+=label("\n\nProgress since the last save will be lost.",
                "\n\nПрогресс после последнего сохранения будет потерян.");
        restartDialog=new android.app.AlertDialog.Builder(activity)
                .setMessage(message)
                .setPositiveButton(label("Restart","Перезапустить"),(dialog,which)->{ preferences.flush(); GameRestart.restart(activity); })
                .setNegativeButton(label("Later","Позже"),null)
                .show();
    }
    private void haptic(int feedback) {
        if (preferences.haptics) performHapticFeedback(feedback);
    }
    private boolean syncSettings(Context next, long now) {
        boolean finalPoll = context.menu() && !next.menu();
        if (settingsRevision != 0 && !next.menu() && !finalPoll) return false;
        if (!finalPoll && settingsRevision != 0 && now-lastSettingsPoll < 100) return false;
        lastSettingsPoll = now;
        int[] host = preferences.settingsWire(settingsRevision);
        int[] guest = bridge.settingsExchange(host);
        if (!TouchPreferences.validSettingsWire(guest)) return false;
        boolean update = settingsRevision == 0 || (guest[2]&(2|8))!=0;
        for (int i = 2; i < host.length; ++i) {
            int expected = i==18 && host[i]==-1 ? (preferences.debugMenuVisible?1:0) : host[i];
            if (expected != guest[i+1]) update = true;
        }
        boolean microphoneBefore = preferences.microphoneEnabled;
        if (update) preferences.applySettingsWire(guest);
        boolean changed = settingsRevision == 0 || (guest[2]&2)!=0;
        int[] applied = preferences.settingsWire(settingsRevision);
        for (int i = 2; i < host.length; ++i) if (host[i] != applied[i]) changed = true;
        boolean microphoneChanged = microphoneBefore != preferences.microphoneEnabled;
        settingsRevision = guest[1];
        if ((guest[2]&1) != 0) layoutEditPending = true;
        if ((guest[2]&4) != 0) gyro.reset();
        if (microphoneChanged) applyMicrophone();
        // The engine sizes its render targets at launch: a new resolution is
        // offered as a restart once the video options page has closed. Two
        // polls ride out the page's own list rebuilds.
        videoOptionsAway = (guest[2]&16) != 0 ? 0 : Math.min(videoOptionsAway+1, 2);
        if ((finalPoll || videoOptionsAway >= 2) && preferences.renderPercent != renderRestartOffered
                && preferences.renderPercent != sessionRenderPercent(activity)) offerRenderRestart();
        if (changed) {
            layoutDirty = true; gyro.reset();
            if (!next.menu()) { cancelInput(); return true; }
        }
        return false;
    }
    private void updateSensor() {
        boolean wanted = !disposed && resumed && focused && eligible && !editing
                && context.gameplay() && context.has(RELATIVE_LOOK) && preferences.gyroEnabled;
        if (wanted && !sensorRegistered && sensorManager != null && aimSensor != null) {
            sensorRegistered = sensorManager.registerListener(this, aimSensor, SensorManager.SENSOR_DELAY_GAME);
            if (sensorRegistered && !loggedSensor) {
                loggedSensor = true;
                Log.i(TAG, "Tilt sensor registered type="+aimSensor.getType()+" name="+aimSensor.getName()
                        +" mode="+context.mode+" epoch="+context.epoch+" caps="+context.capabilities);
            }
        } else if (!wanted) stopSensor();
    }
    private void stopSensor() {
        if (sensorRegistered && sensorManager != null) sensorManager.unregisterListener(this);
        sensorRegistered = false; gyro.reset();
    }
    @Override public void onSensorChanged(SensorEvent event) {
        if (!sensorRegistered || !eligible || !focused || !resumed || editing || !context.gameplay()
                || !preferences.gyroEnabled || (preferences.gyroAdsOnly && (input.held() & bit(AIM)) == 0)) {
            gyro.reset(); return;
        }
        android.view.Display display = getDisplay();
        int rotation = display == null ? Surface.ROTATION_0 : display.getRotation();
        if (event.sensor.getType() == Sensor.TYPE_GAME_ROTATION_VECTOR) {
            SensorManager.getQuaternionFromVector(quaternion, event.values);
            gyro.orientation(quaternion[0], quaternion[1], quaternion[2], quaternion[3], event.timestamp,
                    rotation, preferences.gyroSensitivity, preferences.gyroDeadzone, preferences.gyroInvertY);
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            gyro.velocity(event.values[0], event.values[1], event.timestamp, rotation,
                    preferences.gyroSensitivity, preferences.gyroDeadzone, preferences.gyroInvertY);
        }
        if (gyro.delta[0] != 0 || gyro.delta[1] != 0) input.relativeLook(gyro.delta[0], gyro.delta[1]);
    }
    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    private void cancelInput() {
        saveLayout();
        stopSensor();
        accessibilityFocus=hoverTarget=View.NO_ID;
        closeRadial();clearMenuMore();utilityPointers.clear();legacyPointers.clear();
        hudGesture.cancel(); hudTaps.cancel(); closeSlots(); hudPointers.clear(); hintUntil = 0;
        uiPointers.clear(); menuPointer = -1; editPointer=-1; editButton=null; eligible=false; layoutDirty=true;
        if (!linkageFailed) {
            try { input.cancel(); } catch (UnsatisfiedLinkError error) {
                linkageFailed=true; Log.e(TAG,"Touch JNI is not linked",error);
            }
        }
        invalidate();
    }
    private void accessibilityEvent(int id,int type) {
        AccessibilityEvent event=AccessibilityEvent.obtain(type);event.setSource(this,id);
        event.setPackageName(activity.getPackageName());event.setClassName("android.widget.Button");
        if(getParent()!=null) getParent().requestSendAccessibilityEvent(this,event);
    }
    @Override public boolean dispatchHoverEvent(MotionEvent event) {
        AccessibilityManager manager=(AccessibilityManager)activity.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
        if(!eligible || manager==null || !manager.isTouchExplorationEnabled()) return super.dispatchHoverEvent(event);
        Button b=event.getActionMasked()==MotionEvent.ACTION_HOVER_EXIT?null:hit(event.getX(),event.getY());
        int id=b==null?View.NO_ID:b.action+4;
        if(id!=hoverTarget) {
            if(hoverTarget!=View.NO_ID) accessibilityEvent(hoverTarget,AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
            hoverTarget=id;
            if(id!=View.NO_ID) accessibilityEvent(id,AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
        }
        return id!=View.NO_ID;
    }
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return new AccessibilityNodeProvider() {
            public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
                AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();
                if(id==View.NO_ID) {
                    onInitializeAccessibilityNodeInfo(node);
                    if(eligible) for(Button b:radialOpen?radialButtons:buttons) node.addChild(NativeTouchControls.this,b.action+4);
                    return node;
                }
                if(!eligible) return null;
                for(Button b:radialOpen?radialButtons:buttons) if(b.action+4==id) {
                    node.setSource(NativeTouchControls.this,id);node.setParent(NativeTouchControls.this);
                    node.setPackageName(activity.getPackageName());node.setClassName("android.widget.Button");
                    node.setText(b.label);node.setContentDescription(b.label);
                    node.setEnabled(true);node.setClickable(true);node.setFocusable(true);node.setVisibleToUser(true);
                    node.setAccessibilityFocused(accessibilityFocus==id);
                    Rect bounds=new Rect();b.rect.roundOut(bounds);node.setBoundsInParent(bounds);bounds.offset(screenLocation[0],screenLocation[1]);node.setBoundsInScreen(bounds);
                    node.addAction(AccessibilityNodeInfo.ACTION_CLICK);
                    if (b.action==MORE && context.menu()) {node.setLongClickable(true);node.addAction(AccessibilityNodeInfo.ACTION_LONG_CLICK);}
                    node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
                    return node;
                }
                return null;
            }
            public boolean performAction(int id,int action,Bundle args) {
                if(!eligible) return false;
                for(Button b:radialOpen?radialButtons:buttons) if(b.action+4==id) {
                    if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                        if(accessibilityFocus!=View.NO_ID) accessibilityEvent(accessibilityFocus,AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                        accessibilityFocus=id;accessibilityEvent(id,AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);return true;
                    }
                    if(action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS && accessibilityFocus==id) {
                        accessibilityFocus=View.NO_ID;accessibilityEvent(id,AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);return true;
                    }
                    if(action==AccessibilityNodeInfo.ACTION_CLICK) {activate(10000+id,b.action,b.rect.centerX(),b.rect.centerY());return true;}
                    if(action==AccessibilityNodeInfo.ACTION_LONG_CLICK && b.action==MORE && context.menu()) {openMenuCommands(SystemClock.uptimeMillis());return true;}
                }
                return false;
            }
            public AccessibilityNodeInfo findFocus(int focus) {
                return focus==AccessibilityNodeInfo.FOCUS_ACCESSIBILITY && accessibilityFocus!=View.NO_ID?createAccessibilityNodeInfo(accessibilityFocus):null;
            }
        };
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { surfaceAlive=true; schedule(); }
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) { cancelInput(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { surfaceAlive=false; cancelInput(); }
    @Override protected void onDetachedFromWindow() { dispose(); super.onDetachedFromWindow(); }
    private void dispose() {
        if (disposed) return;
        disposed=true; cancelInput(); unschedule();
        saveLayout(); preferences.flush();
        publishRoute(false);
        if (surface instanceof SurfaceView) ((SurfaceView)surface).getHolder().removeCallback(this);
        if (getParent() instanceof ViewGroup) ((ViewGroup)getParent()).removeView(this);
        if (active.get()==this) active=new WeakReference<>(null);
    }
}
