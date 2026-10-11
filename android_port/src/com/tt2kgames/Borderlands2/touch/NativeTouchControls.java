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
    private static final int[] RADIAL = {NEXT_WEAPON, SKILL, CROUCH, MELEE, INVENTORY, MAP, PREV_WEAPON, RELOAD, CHAT};
    private static final String[] RADIAL_EN = {"Weapon", "Skill", "Crouch", "Melee", "Inventory", "Map", "Previous", "Reload", "Chat"};
    private static final String[] RADIAL_RU = {"Оружие", "Навык", "Присесть", "Удар", "Рюкзак", "Карта", "Предыдущее", "Зарядить", "Чат"};
    // A vehicle seat's radial (review 4.4): Horn and Switch seat. Look back and
    // Turret are holds: a second finger on the open radial keeps them pressed.
    private static final int[] RADIAL_VEHICLE = {VEHICLE_HORN, SWITCH_SEATS, LOOK_BACK, DRIVER_FIRE_TURRET,
            INVENTORY, MAP, NEXT_WEAPON, RELOAD, CHAT};
    private static final String[] RADIAL_VEHICLE_EN = {"Horn", "Switch seat", "Look back", "Turret", "Inventory", "Map", "Weapon", "Reload", "Chat"};
    private static final String[] RADIAL_VEHICLE_RU = {"Сигнал", "Пересесть", "Оглянуться", "Турель", "Рюкзак", "Карта", "Оружие", "Зарядить", "Чат"};
    private static final long STALE_MS = 1000;
    private static final int HINT_MAP = TouchPreferences.HINT_MAP, HINT_WEAPON = TouchPreferences.HINT_WEAPON,
            HINT_GAME_MORE = TouchPreferences.HINT_GAME_MORE, HINT_MENU_MORE = TouchPreferences.HINT_MENU_MORE,
            HINT_DEBUG_MORE = TouchPreferences.HINT_DEBUG_MORE, HINT_AUTORUN = TouchPreferences.HINT_AUTORUN,
            HINT_MAP_NAVIGATION = TouchPreferences.HINT_MAP_NAVIGATION, HINT_LAYOUT = TouchPreferences.HINT_LAYOUT;
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
    private final SparseArray<RectF> hudPointers = new SparseArray<>();
    private final RectF mapTarget = new RectF(), ammoTarget = new RectF(), useTarget = new RectF();
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
    private final int[] moveVisualBefore = new int[8], moveVisualAfter = new int[8];
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
    private long legacyHeld;
    private float fallbackYaw, fallbackPitch, radialX,radialY,radialRadius,radialStartX,radialStartY;
    private int radialPointer=-1, radialSelection=-1, radialCount=8;
    private int[] radial=RADIAL;
    private long radialStarted;
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
        if (!value) { view.cancelInput(); view.unschedule(); }
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
                hudGesture.cancel(); hudPointers.clear(); hintUntil = 0;
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
                    || !Arrays.equals(context.useRect,next.useRect)) layoutDirty = true;
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
                    || (hintKind == HINT_MAP_NAVIGATION && !context.has(MAP_GESTURES))) {
                hintKind = 0; hintUntil = 0; invalidate();
            }
            if (eligible && !editing) {
                int hudAction = hudGesture.poll(now,context);
                if (hudAction >= 0) activate(nextLegacyId++,hudAction,0,0);
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
        if (!eligible) return;
        if (context.gameplay()) {
            // HUD follows the full Surface; insets and left-hand mode affect
            // controls only. Never rewrite a player's deliberately saved layout.
            TouchLayout.reserveGameplayHud(viewport.left,viewport.top,viewport.right,viewport.bottom,hudReserved,
                    !ammoTarget.isEmpty() || confirmedUnarmed(context),context.hudObjectivesKnown() && context.hudMap());
            if (context.hudObjectives()) TouchLayout.reserveSurfaceRect(context.objectiveRect,
                    viewport.left,viewport.top,viewport.right,viewport.bottom,hudReserved);
            // The Action caption comes and goes with the aimed object. It stays
            // tappable but never displaces controls: hovering only adds Action.
            for (RectF rect : new RectF[]{mapTarget,ammoTarget}) if (!rect.isEmpty())
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
                add(UI_UP, label("Up", "Вверх"), 176, 96, 48); add(UI_DOWN, label("Down", "Вниз"), 176, 36, 48);
                add(UI_LEFT, label("Left", "Влево"), 236, 36, 48); add(UI_RIGHT, label("Right", "Вправо"), 116, 36, 48);
                if (context.has(DEBUG_CANVAS)) {
                    add(RELOAD,label("Action X", "Действие X"),300,36,48); add(NEXT_WEAPON,label("Action Y", "Действие Y"),300,96,48);
                    add(SKILL,label("Action R", "Действие R"),360,36,48); add(SPRINT,label("Action L", "Действие L"),360,96,48);
                    add(PREV_WEAPON,label("Tab −", "Вкладка −"),236,156,48); add(INVENTORY,label("Tab +", "Вкладка +"),176,156,48);
                } else {
                    add(LEGACY+99,label("Action", "Действие"),300,36,48); add(LEGACY+100,label("Other", "Другое"),300,96,48);
                    add(LEGACY+102,label("Tab −", "Вкладка −"),360,36,48); add(LEGACY+103,label("Tab +", "Вкладка +"),360,96,48);
                }
            }
        }
        if(radialOpen) buildRadial();
    }
    private void buildRadial() {
        radialButtons.clear();
        String[] labels=radial==RADIAL_VEHICLE ? (russian?RADIAL_VEHICLE_RU:RADIAL_VEHICLE_EN) : (russian?RADIAL_RU:RADIAL_EN);
        for(int i=0;i<radialCount;i++) {
            int a=radial[i];
            if (!radialAvailable(a)) continue;
            double angle=i*Math.PI*2/radialCount-Math.PI/2;
            float x=radialX+(float)Math.cos(angle)*radialRadius,y=radialY+(float)Math.sin(angle)*radialRadius,s=dp(25);
            radialButtons.add(new Button(a,labels[i],new RectF(x-s,y-s,x+s,y+s),dp(2)));
        }
    }
    private boolean radialAvailable(int action) {
        return context.allows(action) && !(fallback && action == PREV_WEAPON)
                && !(action == CHAT && (fallback || !context.gameplay()))
                && !(context.weaponCount >= 0 && context.weaponCount < 2 && (action == NEXT_WEAPON || action == PREV_WEAPON));
    }
    static int[] radialActions(Context context) { return context.mode == VEHICLE ? RADIAL_VEHICLE : RADIAL; }
    /** Radial items a second finger holds like a button (Turret fire, Look back). */
    static boolean radialHold(int action) { return action == LOOK_BACK || action == DRIVER_FIRE_TURRET; }
    static int radialActionCount(Context context,boolean fallback) {
        return !fallback && context.usable() && context.gameplay() && context.allows(CHAT) ? 9 : 8;
    }
    private void openRadial(int id,float x,float y) {
        radialOpen=true; radialPointer=id; radialSelection=-1; radialDragged=false;
        radialStartX=x; radialStartY=y; radialStarted=SystemClock.uptimeMillis();
        // Capture the count once: a live availability change must not rotate
        // the other sectors underneath an already held finger.
        radialCount=radialActionCount(context,fallback);
        radial=radialActions(context);
        radialRadius=Math.min(dp(94),Math.min(safe.width(),safe.height())/2-dp(30));
        float margin=radialRadius+dp(27);
        radialX=Math.max(safe.left+margin,Math.min(safe.right-margin,x));
        radialY=Math.max(safe.top+margin,Math.min(safe.bottom-margin,y));
        buildRadial(); haptic(HapticFeedbackConstants.VIRTUAL_KEY);
    }
    private void closeRadial() { radialOpen=false;radialPointer=-1;radialSelection=-1;radialButtons.clear(); }
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
        else showHint("Tap to select, tap again to change. Swipe lists; hold More for actions", "Тап — выбор, повторный — изменение. Свайп — список; удерживайте «Ещё» для действий",HINT_MENU_MORE);
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
    private void moreHint() { showHint("Crouch, melee, reload, inventory: hold More and drag", "Присесть, удар, перезарядка, рюкзак: удерживайте «Ещё» и сдвиньте палец",HINT_GAME_MORE); }
    private void offerHudHint(long now) {
        if (!context.gameplay() || fallback || radialOpen || now < hintUntil) return;
        if (!preferences.mapTapHintSeen && !mapTarget.isEmpty() && context.allows(MAP))
            showHint("Tap the minimap to open the map", "Тап по мини-карте открывает карту",HINT_MAP);
        else if (!preferences.weaponGestureHintSeen && context.mode == GAMEPLAY && context.weaponCount >= 2 && !ammoTarget.isEmpty()
                && context.allows(NEXT_WEAPON) && context.allows(PREV_WEAPON))
            showHint("Ammo: swipe to switch; double-tap returns after a switch", "Патроны: свайп — смена; двойной тап — возврат после смены",HINT_WEAPON);
    }
    private void drawHint(Canvas canvas) {
        if (hintText == null || SystemClock.uptimeMillis() >= hintUntil) return;
        float width=safe.width()-dp(32); if (width <= dp(24)) return;
        paint.setTextSize(dp(14)); paint.setTextAlign(Paint.Align.CENTER); paint.setStyle(Paint.Style.FILL);
        float measured=paint.measureText(hintText);
        if (measured > width-dp(24)) paint.setTextSize(dp(14)*(width-dp(24))/measured);
        float half=Math.min(width/2,(paint.measureText(hintText)+dp(24))/2);
        float cy=safe.top+dp(74),cx=safe.centerX();
        paint.setColor(Color.BLACK); paint.setAlpha(210);
        canvas.drawRoundRect(cx-half,cy-dp(18),cx+half,cy+dp(18),dp(10),dp(10),paint);
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
            canvas.drawCircle(radialX,radialY,radialRadius+dp(29),paint);
            for(Button b:radialButtons) {
                boolean selected=radialSelection>=0 && radial[radialSelection]==b.action;
                paint.setColor(selected?0xFFFFD569:0xFF454F5D);canvas.drawCircle(b.rect.centerX(),b.rect.centerY(),b.rect.width()/2,paint);
                paint.setColor(selected?Color.BLACK:Color.WHITE);paint.setTextSize(dp(10));paint.setTextAlign(Paint.Align.CENTER);
                float measured=paint.measureText(b.label),available=b.rect.width()-dp(8);
                if (measured > available) paint.setTextSize(dp(10)*available/measured);
                canvas.drawText(b.label,b.rect.centerX(),b.rect.centerY()+dp(4),paint);
            }
            paint.setColor(Color.WHITE);paint.setTextSize(dp(10));
            canvas.drawText(label("Choose", "Выберите"),radialX,radialY-dp(3),paint);
            canvas.drawText(label("action", "действие"),radialX,radialY+dp(12),paint);
        }
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
                closeRadial();
                if (selected >= 0 && radialAvailable(radial[selected])) activate(id,radial[selected],x,y);
                else if (tap) moreHint();
            } else if (hudGesture.owns(id)) {
                RectF rect=hudPointers.get(id); hudPointers.remove(id);
                boolean inside=rect != null && (hudGesture.zone(id) == TouchInput.HudGesture.AMMO_ZONE ? safe.contains(x,y) : rect.contains(x,y));
                int command=hudGesture.up(id,x,y,SystemClock.uptimeMillis(),context,inside,dp(14));
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
                utilityPointers.put(id,b);
                return;
            } else if(b!=null && b.action==MORE) {closeRadial();return;}
            else if(radialPointer<0) closeRadial();
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
            input.down(id, movement ? TouchInput.MOVEMENT : TouchInput.LOOK, -1, x, y);
        }
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
    }
    private void move(int id, float x, float y) {
        if (hudGesture.owns(id)) { hudGesture.move(id,x,y); return; }
        if (id==menuMorePointer) {
            if (menuMoreButton==null || !menuMoreButton.contains(x,y)) {utilityPointers.remove(id);clearMenuMoreHold();}
            return;
        }
        if(id==radialPointer) {
            if(Math.hypot(x-radialStartX,y-radialStartY)>dp(12)) radialDragged=true;
            int selected=radialDragged?TouchLayout.radialSector(x,y,radialX,radialY,radialRadius,radialCount):-1;
            if(selected>=0 && !radialAvailable(radial[selected])) selected=-1;
            if(selected!=radialSelection && selected>=0) haptic(HapticFeedbackConstants.CLOCK_TICK);
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
                AccessibilityManager manager=(AccessibilityManager)activity.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
                if (manager != null && manager.isTouchExplorationEnabled()) openRadial(-1,x,y); else moreHint();
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
                .setPositiveButton(label("Restart","Перезапустить"),(dialog,which)->GameRestart.restart(activity))
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
        hudGesture.cancel(); hudPointers.clear(); hintUntil = 0;
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
        publishRoute(false);
        if (surface instanceof SurfaceView) ((SurfaceView)surface).getHolder().removeCallback(this);
        if (getParent() instanceof ViewGroup) ((ViewGroup)getParent()).removeView(this);
        if (active.get()==this) active=new WeakReference<>(null);
    }
}
