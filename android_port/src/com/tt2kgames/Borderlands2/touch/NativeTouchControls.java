package com.tt2kgames.Borderlands2.touch;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.util.Log;
import android.util.SparseArray;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

/**
 * Integration candidate, not installed by this kit. All methods run on the UI thread.
 * Engine context/semantic sinks must be wired before this replaces LaunchControls.
 */
public final class NativeTouchControls extends View implements Choreographer.FrameCallback,
        SurfaceHolder.Callback {
    public interface RouteListener { void nativeRouteChanged(boolean ownsInput); }
    private static final String TAG = "BL2NativeTouch";
    private static final int SETTINGS = -1, MORE = -2, DONE = -3;
    private static final long STALE_MS = 1000;
    private static WeakReference<NativeTouchControls> active = new WeakReference<>(null);
    private static final class Button {
        final int action;
        final String label;
        final RectF rect;
        Button(int action, String label, RectF rect) {
            this.action = action; this.label = label; this.rect = rect;
        }
    }
    private final Activity activity;
    private final View surface;
    private final TouchBridge bridge = new TouchBridge();
    private final TouchInput input = new TouchInput(bridge);
    private final TouchPreferences preferences;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF safe = new RectF(), viewport = new RectF();
    private final ArrayList<Button> buttons = new ArrayList<>();
    private final ArrayList<float[]> occupied = new ArrayList<>();
    private final SparseArray<float[]> uiPointers = new SparseArray<>();
    private final SparseArray<RectF> menuButtons = new SparseArray<>();
    private int menuPointer = -1;
    private final int[] screenLocation = new int[2], surfaceLocation = new int[2];
    private Context context = Context.unknown();
    private boolean focused, resumed = true, surfaceAlive, disposed, scheduled;
    private boolean eligible, streamOwned, settingsOpen, editing, expanded, linkageFailed;
    private boolean layoutDirty = true;
    private float layoutScale = 1;
    private int insetL, insetT, insetR, insetB, editPointer = -1;
    private Button editButton;
    private long lastFrame = -1, lastFrameTime;
    private AlertDialog settingsDialog;
    private RouteListener routeListener;
    private boolean nativeRoute, imeVisible;

    private NativeTouchControls(Activity owner, View renderSurface) {
        super(owner);
        activity = owner; surface = renderSurface;
        preferences = new TouchPreferences(owner);
        focused = owner.hasWindowFocus();
        setFocusable(false);
        setClickable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
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
            if (next.frame != lastFrame) { lastFrame = next.frame; lastFrameTime = now; }
            boolean nextEligible = focused && surfaceAlive && !settingsOpen && !imeVisible && next.usable()
                    && now - lastFrameTime < STALE_MS && !viewport.isEmpty() && !safe.isEmpty();
            if (eligible && !nextEligible) {
                cancelInput();
                // reset advanced the epoch; never re-use the snapshot preceding it.
                next = bridge.snapshot();
            }
            if (next.epoch != context.epoch) {
                uiPointers.clear(); menuButtons.clear(); menuPointer = -1; editPointer = -1; editButton = null;
                expanded = false;
            }
            if (eligible != nextEligible || context.epoch != next.epoch
                    || context.actions != next.actions) layoutDirty = true;
            context = next; eligible = nextEligible;
            // Focus/IME/settings temporarily suppress gestures while retaining the
            // native route, so an old overlay cannot appear over a modal keyboard.
            publishRoute(next.has(GAMEPLAY_INPUT | DIRECT_UI | UI_NAVIGATION | UI_TAP_TEST | PHYSICAL_OWNS_INPUT));
            if (eligible) { input.setContext(next); input.heartbeat(); }
            if (layoutDirty) buildButtons();
            invalidate();
        } catch (UnsatisfiedLinkError error) {
            linkageFailed = true; eligible = false; buttons.clear();
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
        if (!viewport.equals(next)) { cancelInput(); viewport.set(next); layoutDirty=true; }
        next.set(viewport);
        if (!next.intersect(insetL, insetT, getWidth() - insetR, getHeight() - insetB)) next.setEmpty();
        if (!safe.equals(next)) { safe.set(next); layoutDirty=true; }
        layoutScale=Math.min(preferences.scale,Math.max(.75f,(safe.height()/dp(1)-64)/260));
        input.configure(getResources().getDisplayMetrics().density, 60 * preferences.scale,
                preferences.look, preferences.ads, preferences.invertY);
    }
    private void add(int action, String label, float fromRightDp, float fromBottomDp, float sizeDp) {
        if (action >= 0 && !context.allows(action)) return;
        float size = dp(Math.max(48, sizeDp * layoutScale));
        if (safe.width() < size || safe.height() < size) return;
        float x = safe.right - dp(fromRightDp * layoutScale);
        float y = safe.bottom - dp(fromBottomDp * layoutScale);
        float nx = (x - safe.left) / safe.width(), ny = (y - safe.top) / safe.height();
        // Utility commands have fixed positions and do not share gameplay saved positions.
        if (action >= 0 && action != PAUSE && context.gameplay()) {
            nx = preferences.x(action, nx); ny = preferences.y(action, ny);
        }
        if (preferences.leftHanded && context.gameplay()) nx = 1 - nx;
        x = Math.max(safe.left + size / 2, Math.min(safe.right - size / 2, safe.left + nx * safe.width()));
        y = Math.max(safe.top + size / 2, Math.min(safe.bottom - size / 2, safe.top + ny * safe.height()));
        boolean utility=action<0 || action==PAUSE;
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
        float[] placed=TouchLayout.place(x,y,size,left,top,right,bottom,dp(4),dp(4),occupied);
        if(placed!=null) {
            occupied.add(placed);
            buttons.add(new Button(action,label,new RectF(placed[0],placed[1],placed[2],placed[3])));
        }
    }
    private void buildButtons() {
        layoutDirty=false; buttons.clear(); occupied.clear();
        if (!eligible) return;
        if (context.gameplay()) {
            add(FIRE, "Огонь", 52, 154, 64); add(AIM, "Прицел", 122, 218, 48);
            add(RELOAD, "Зарядить", 52, 228, 48); add(USE, "Действие", 122, 148, 48);
            if (context.mode == GAMEPLAY) {
                add(JUMP, "Прыжок", 52, 76, 48); add(CROUCH, "Присесть", 122, 52, 48);
                add(MELEE, "Удар", 192, 76, 48); add(GRENADE, "Граната", 192, 148, 48);
                add(SKILL, "Навык", 192, 218, 48);
            } else {
                add(VEHICLE_BRAKE, "Тормоз", 52, 76, 48);
                add(VEHICLE_BOOST, "Ускорить", 192, 76, 48);
                add(VEHICLE_HORN, "Сигнал", 192, 148, 48);
            }
            add(NEXT_WEAPON, "Оружие", 264, 76, 48);
            add(SPRINT, "Бег", 264, 148, 48);
            float topAnchor = safe.height()/dp(1)/layoutScale-28;
            add(PAUSE, "Пауза", 28, topAnchor, 48);
            add(editing ? DONE : MORE, editing ? "Готово" : "Ещё", 88, topAnchor, 48);
            if (expanded || editing) {
                add(INVENTORY, "Рюкзак", 264, 218, 48);
                add(MAP, "Карта", 336, 218, 48);
                add(PREV_WEAPON, "Назад", 336, 148, 48);
                add(SETTINGS, "Опции", 148, topAnchor, 48);
            }
        } else if (context.mode == CINEMATIC && context.has(CAN_SKIP)) {
            add(SKIP, "Пропуск", 48, 40, 64);
        } else if (context.menu()) {
            add(BACK, "Назад", 36, 36, 56);
            // Diagnostic ui-test navigation. Remove this panel when the real
            // widget/drag adapter passes its hardware gate; no Xbox emulation.
            if (context.has(UI_NAVIGATION)) {
                add(UI_UP, "Вверх", 176, 96, 48); add(UI_DOWN, "Вниз", 176, 36, 48);
                add(UI_LEFT, "Влево", 236, 36, 48); add(UI_RIGHT, "Вправо", 116, 36, 48);
                add(CONFIRM, "Выбрать", 116, 96, 48);
            }
        }
    }
    private Button hit(float x, float y) {
        for (int i = buttons.size()-1; i >= 0; --i) if (buttons.get(i).rect.contains(x, y)) return buttons.get(i);
        return null;
    }
    @Override protected void onDraw(Canvas canvas) {
        if (!eligible) return;
        canvas.save(); canvas.clipRect(safe);
        if (input.moving()) {
            paint.setColor(Color.WHITE); paint.setAlpha(50); paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            canvas.drawCircle(input.stickX(), input.stickY(), dp(60*preferences.scale), paint);
            paint.setStyle(Paint.Style.FILL); paint.setAlpha(110);
            canvas.drawCircle(input.stickX()+input.moveX()*dp(48*preferences.scale),
                    input.stickY()-input.moveY()*dp(48*preferences.scale), dp(18), paint);
        }
        for (Button b : buttons) {
            boolean held = b.action >= 0 && (input.held() & bit(b.action)) != 0;
            paint.setStyle(Paint.Style.FILL); paint.setColor(Color.BLACK);
            paint.setAlpha((int)(255*(held || editing ? .72f : preferences.opacity)));
            canvas.drawRoundRect(b.rect, dp(12), dp(12), paint);
            paint.setColor(held ? 0xFFFFD569 : Color.WHITE); paint.setAlpha(held || editing ? 255 : 180);
            paint.setTextSize(dp(10)); paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(b.label, b.rect.centerX(), b.rect.centerY()+dp(4), paint);
        }
        canvas.restore();
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            streamOwned = event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)
                    && eligible && (editing || context.gameplay()
                    || hit(event.getX(), event.getY()) != null
                    || (context.menu() && context.has(DIRECT_UI | UI_TAP_TEST) && viewport.contains(event.getX(), event.getY())));
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
            for (int h = 0; h < event.getHistorySize(); h++) for (int i = 0; i < event.getPointerCount(); i++)
                move(event.getPointerId(i), event.getHistoricalX(i,h), event.getHistoricalY(i,h));
            for (int i = 0; i < event.getPointerCount(); i++) move(event.getPointerId(i), event.getX(i), event.getY(i));
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int id = event.getPointerId(index);
            if (uiPointers.get(id) != null) {
                sendUi(id, viewport.contains(event.getX(index),event.getY(index)) ? UP : CANCEL,
                        event.getX(index), event.getY(index)); uiPointers.remove(id);
            } else {
                RectF rect = menuButtons.get(id);
                if (rect != null && !rect.contains(event.getX(index), event.getY(index))) cancelInput();
                else input.up(id, event.getX(index), event.getY(index));
            }
            menuButtons.remove(id);
            if (id == menuPointer) menuPointer = -1;
        }
        if (action == MotionEvent.ACTION_UP) streamOwned = false;
        invalidate(); return true;
    }
    private void down(int id, float x, float y) {
        if (context.menu()) {
            if (menuPointer >= 0) { cancelInput(); return; }
            menuPointer = id;
        }
        Button b = hit(x, y);
        if (b != null) {
            if (b.action == MORE) { expanded = !expanded; buildButtons(); return; }
            if (b.action == SETTINGS) { showSettings(); return; }
            if (context.menu()) menuButtons.put(id, new RectF(b.rect));
            input.down(id, TouchInput.BUTTON, b.action, x, y); return;
        }
        if (context.menu() && context.has(DIRECT_UI | UI_TAP_TEST) && viewport.contains(x, y)) {
            uiPointers.put(id, new float[]{x, y}); sendUi(id, DOWN, x, y); return;
        }
        if (context.gameplay() && safe.contains(x, y)) {
            float nx = (x-safe.left)/safe.width();
            if (preferences.leftHanded) nx = 1-nx;
            boolean movement = nx < .40f && y > safe.top + safe.height()*.35f;
            input.down(id, movement ? TouchInput.MOVEMENT : TouchInput.LOOK, -1, x, y);
        }
    }
    private void move(int id, float x, float y) {
        RectF rect = menuButtons.get(id);
        if (rect != null && !rect.contains(x,y)) { cancelInput(); return; }
        float[] p = uiPointers.get(id);
        if (p != null) {
            p[0]=x; p[1]=y;
            if(!viewport.contains(x,y)) { sendUi(id,CANCEL,x,y); uiPointers.remove(id); }
            else sendUi(id,MOVE,x,y);
        }
        else input.move(id,x,y);
    }
    private void sendUi(int id, int phase, float x, float y) {
        // Coordinates refer to the complete render Surface, not the safe layout rectangle.
        bridge.uiPointer(context.epoch, id, phase,
                Math.max(0,Math.min(1,(x-viewport.left)/viewport.width())),
                Math.max(0,Math.min(1,(y-viewport.top)/viewport.height())));
    }
    private void editEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            Button b = hit(event.getX(),event.getY());
            if (b != null && b.action == DONE) {
                editing=false; expanded=false; cancelInput(); return;
            }
            if (b != null && b.action >= 0) { editButton=b; editPointer=event.getPointerId(0); }
        } else if (action == MotionEvent.ACTION_MOVE && editButton != null) {
            int i=event.findPointerIndex(editPointer);
            if (i<0) return;
            float nx=Math.max(0,Math.min(1,(event.getX(i)-safe.left)/safe.width()));
            float ny=Math.max(0,Math.min(1,(event.getY(i)-safe.top)/safe.height()));
            if (preferences.leftHanded) nx=1-nx;
            preferences.position(editButton.action,nx,ny);
            buildButtons();
        } else if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                && event.getPointerId(index)==editPointer) { editButton=null; editPointer=-1; }
    }
    private SeekBar slider(LinearLayout box, String label, float value, float min, float max) {
        TextView text=new TextView(activity); text.setText(label); box.addView(text);
        SeekBar bar=new SeekBar(activity); bar.setMax(100); bar.setProgress(Math.round((value-min)/(max-min)*100));
        box.addView(bar); return bar;
    }
    private CheckBox checkbox(LinearLayout box, String label, boolean checked) {
        CheckBox check=new CheckBox(activity); check.setText(label); check.setChecked(checked); box.addView(check); return check;
    }
    private void showSettings() {
        cancelInput(); settingsOpen=true;
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding((int)dp(24),(int)dp(12),(int)dp(24),(int)dp(12));
        SeekBar size=slider(box,"Размер кнопок",preferences.scale,.85f,1.4f);
        SeekBar opacity=slider(box,"Непрозрачность",preferences.opacity,.15f,.9f);
        SeekBar look=slider(box,"Чувствительность обзора",preferences.look,.04f,1);
        SeekBar ads=slider(box,"Чувствительность прицела",preferences.ads,.1f,1);
        CheckBox left=checkbox(box,"Управление для левой руки",preferences.leftHanded);
        CheckBox invert=checkbox(box,"Инверсия вертикального обзора",preferences.invertY);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        settingsDialog=new AlertDialog.Builder(activity).setTitle("Сенсорное управление").setView(scroll)
                .setPositiveButton("Сохранить",(dialog,which)->{
                    preferences.scale=.85f+size.getProgress()/100f*.55f;
                    preferences.opacity=.15f+opacity.getProgress()/100f*.75f;
                    preferences.look=.04f+look.getProgress()/100f*.96f;
                    preferences.ads=.1f+ads.getProgress()/100f*.9f;
                    preferences.leftHanded=left.isChecked(); preferences.invertY=invert.isChecked(); preferences.save();
                }).setNegativeButton("Сбросить",(dialog,which)->preferences.reset())
                .setNeutralButton("Раскладка",(dialog,which)->{ editing=true; expanded=true; }).create();
        settingsDialog.setOnDismissListener(dialog->{ settingsOpen=false; settingsDialog=null; layoutDirty=true; schedule(); });
        settingsDialog.show();
    }
    private void cancelInput() {
        uiPointers.clear(); menuButtons.clear(); menuPointer = -1; editPointer=-1; editButton=null; eligible=false; layoutDirty=true;
        if (!linkageFailed) {
            try { input.cancel(); } catch (UnsatisfiedLinkError error) {
                linkageFailed=true; Log.e(TAG,"Touch JNI is not linked",error);
            }
        }
        invalidate();
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { surfaceAlive=true; schedule(); }
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) { cancelInput(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { surfaceAlive=false; cancelInput(); }
    @Override protected void onDetachedFromWindow() { dispose(); super.onDetachedFromWindow(); }
    private void dispose() {
        if (disposed) return;
        disposed=true; cancelInput(); unschedule();
        publishRoute(false);
        if (settingsDialog != null) settingsDialog.dismiss();
        if (surface instanceof SurfaceView) ((SurfaceView)surface).getHolder().removeCallback(this);
        if (getParent() instanceof ViewGroup) ((ViewGroup)getParent()).removeView(this);
        if (active.get()==this) active=new WeakReference<>(null);
    }
}
