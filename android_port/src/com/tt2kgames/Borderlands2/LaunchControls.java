package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import com.tt2kgames.Borderlands2.touch.NativeTouchControls;
import com.tt2kgames.Borderlands2.touch.TouchSources;

/** Provisional dual-stick/gamepad overlay; gameplay bindings still need validation. */
public final class LaunchControls extends FrameLayout
        implements InputManager.InputDeviceListener {
    private static final String TAG = "BL2LaunchControls";
    private static final int CONTROLLER = 1;
    private static final int GENERIC_GAMEPAD = 4;
    private static final long MIN_BUTTON_PRESS_MS = 150L;
    private static final long MIN_MENU_PRESS_MS = 600L;
    private static final long MIN_TRIGGER_PRESS_MS = 150L;
    private static final long STICK_REPEAT_MS = 50L;
    private static volatile WeakReference<LaunchControls> active = new WeakReference<>(null);
    private static final AtomicInteger loggedPresence = new AtomicInteger();
    private final Activity activity;
    private final Method keyEvent;
    private final Method motionEvent;
    private final Method deviceEvent;
    private final InputManager inputManager;
    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final ArrayList<KeyButton> buttons = new ArrayList<>();
    private final StickView stick;
    private final StickView cameraStick;
    private float leftTrigger;
    private float rightTrigger;
    private volatile boolean virtualConnected;
    private boolean nativeTouchOwns;
    private boolean focused;
    private boolean disposed;
    private int loggedKeys;
    private int loggedTouches;
    private int loggedMotions;
    private int lastMotionMask;
    private boolean loggedLayout;
    private int lastInsetLeft = -1;
    private int lastInsetTop = -1;
    private int lastInsetRight = -1;
    private int lastInsetBottom = -1;

    private LaunchControls(Activity owner) throws ReflectiveOperationException {
        super(owner);
        activity = owner;
        keyEvent = owner.getClass().getMethod("NativeCallback_KeyEvent",
                int.class, int.class, int.class, int.class, long.class);
        motionEvent = owner.getClass().getMethod("NativeCallback_MotionEvent",
                int.class, int.class, float.class, float.class, float.class, float.class,
                float.class, float.class, float.class, float.class, int.class, long.class);
        deviceEvent = owner.getClass().getMethod("NativeCallback_DeviceEvent",
                int.class, int.class, boolean.class);
        inputManager = (InputManager) owner.getSystemService(Context.INPUT_SERVICE);
        setFocusable(false);
        setClickable(false);
        setMotionEventSplittingEnabled(true);
        setClipChildren(false);
        setClipToPadding(false);

        FrameLayout dpad = new FrameLayout(owner);
        dpad.setMotionEventSplittingEnabled(true);
        addButton(dpad, "UP", KeyEvent.KEYCODE_DPAD_UP, 52, 0, 52);
        addButton(dpad, "LEFT", KeyEvent.KEYCODE_DPAD_LEFT, 0, 52, 52);
        addButton(dpad, "RIGHT", KeyEvent.KEYCODE_DPAD_RIGHT, 104, 52, 52);
        addButton(dpad, "DOWN", KeyEvent.KEYCODE_DPAD_DOWN, 52, 104, 52);
        LayoutParams left = new LayoutParams(dp(156), dp(156), Gravity.LEFT | Gravity.BOTTOM);
        left.setMargins(dp(12), dp(12), dp(12), dp(12));
        addView(dpad, left);

        stick = new StickView(owner, true);
        LayoutParams stickLayout = new LayoutParams(dp(156), dp(156),
                Gravity.LEFT | Gravity.BOTTOM);
        stickLayout.setMargins(dp(180), dp(12), dp(12), dp(12));
        addView(stick, stickLayout);

        cameraStick = new StickView(owner, false);
        LayoutParams cameraLayout = new LayoutParams(dp(156), dp(156),
                Gravity.RIGHT | Gravity.BOTTOM);
        cameraLayout.setMargins(dp(12), dp(12), dp(220), dp(12));
        addView(cameraStick, cameraLayout);

        FrameLayout actions = new FrameLayout(owner);
        actions.setMotionEventSplittingEnabled(true);
        addButton(actions, "BACK", KeyEvent.KEYCODE_BACK, 0, 0, 76);
        addButton(actions, "X", KeyEvent.KEYCODE_BUTTON_X, 80, 0, 56);
        addButton(actions, "Y", KeyEvent.KEYCODE_BUTTON_Y, 140, 0, 56);
        addButton(actions, "START", KeyEvent.KEYCODE_BUTTON_START, 0, 60, 76);
        addButton(actions, "B", KeyEvent.KEYCODE_BUTTON_B, 80, 60, 56);
        addButton(actions, "A", KeyEvent.KEYCODE_BUTTON_A, 140, 60, 56);
        LayoutParams right = new LayoutParams(dp(196), dp(112), Gravity.RIGHT | Gravity.BOTTOM);
        right.setMargins(dp(12), dp(12), dp(12), dp(12));
        addView(actions, right);

        FrameLayout shoulders = new FrameLayout(owner);
        shoulders.setMotionEventSplittingEnabled(true);
        addButton(shoulders, "L1", KeyEvent.KEYCODE_BUTTON_L1, 0, 0, 76);
        addButton(shoulders, "R1", KeyEvent.KEYCODE_BUTTON_R1, 100, 0, 76);
        addButton(shoulders, "L2", -1, 0, 56, 76);
        addButton(shoulders, "R2", -2, 100, 56, 76);
        addButton(shoulders, "L3", KeyEvent.KEYCODE_BUTTON_THUMBL, 0, 112, 76);
        addButton(shoulders, "R3", KeyEvent.KEYCODE_BUTTON_THUMBR, 100, 112, 76);
        LayoutParams shoulderLayout = new LayoutParams(dp(196), dp(164),
                Gravity.RIGHT | Gravity.BOTTOM);
        shoulderLayout.setMargins(dp(12), dp(12), dp(12), dp(136));
        addView(shoulders, shoulderLayout);

        setOnApplyWindowInsetsListener(new OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                int l = insets.getSystemWindowInsetLeft();
                int t = insets.getSystemWindowInsetTop();
                int r = insets.getSystemWindowInsetRight();
                int b = insets.getSystemWindowInsetBottom();
                if (android.os.Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                    l = Math.max(l, insets.getDisplayCutout().getSafeInsetLeft());
                    t = Math.max(t, insets.getDisplayCutout().getSafeInsetTop());
                    r = Math.max(r, insets.getDisplayCutout().getSafeInsetRight());
                    b = Math.max(b, insets.getDisplayCutout().getSafeInsetBottom());
                }
                if (l != lastInsetLeft || t != lastInsetTop ||
                        r != lastInsetRight || b != lastInsetBottom) {
                    Log.i(TAG, "safe insets px=" + l + "," + t + "," + r + "," + b +
                            " view=" + getWidth() + "x" + getHeight());
                    lastInsetLeft = l;
                    lastInsetTop = t;
                    lastInsetRight = r;
                    lastInsetBottom = b;
                    loggedLayout = false;
                }
                setPadding(l, t, r, b);
                return insets;
            }
        });
    }

    public static void attach(Activity owner) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            Log.e(TAG, "attach must run on UI thread");
            return;
        }
        LaunchControls old = active.get();
        if (old != null && old.activity == owner && !old.disposed) return;
        if (old != null) old.dispose();
        try {
            View decor = owner.getWindow().getDecorView();
            if (!(decor instanceof ViewGroup)) return;
            LaunchControls controls = new LaunchControls(owner);
            active = new WeakReference<>(controls);
            ((ViewGroup) decor).addView(controls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            controls.focused = owner.hasWindowFocus();
            controls.inputManager.registerInputDeviceListener(controls, new Handler(Looper.getMainLooper()));
            controls.refreshOwnership();
            controls.requestApplyInsets();
            Log.i(TAG, "Attached provisional dual-stick controls, controller=" +
                    CONTROLLER);
            if (android.os.Build.VERSION.SDK_INT >= 37 &&
                    owner.checkSelfPermission("android.permission.ACCESS_LOCAL_NETWORK") != 0) {
                owner.requestPermissions(new String[]{"android.permission.ACCESS_LOCAL_NETWORK"}, 0x4C41);
            }
        } catch (ReflectiveOperationException | RuntimeException error) {
            Log.e(TAG, "Cannot attach launch controls", error);
        }
    }

    public static void attach(Activity owner, View surface) {
        attach(owner);
        NativeTouchControls.attach(owner, surface);
        NativeTouchControls.setRouteListener(owner, owns -> setNativeTouchOwns(owner, owns));
    }

    public static void setNativeTouchOwns(Activity owner, boolean owns) {
        LaunchControls controls = active.get();
        if (controls == null || controls.activity != owner || controls.disposed) return;
        if (owns && !controls.nativeTouchOwns) controls.releaseAll();
        controls.nativeTouchOwns = owns;
        controls.setVisibility(controls.virtualConnected && !owns ? View.VISIBLE : View.GONE);
    }

    public static void focus(Activity owner, boolean focused) {
        NativeTouchControls.focus(owner, focused);
        LaunchControls controls = active.get();
        if (controls == null || controls.activity != owner || controls.disposed) return;
        controls.focused = focused;
        if (!focused) controls.releaseAll();
    }

    public static void detach(Activity owner) {
        NativeTouchControls.detach(owner);
        LaunchControls controls = active.get();
        if (controls != null && controls.activity == owner) controls.dispose();
    }

    public static boolean isConnected(Activity owner, int controller) {
        LaunchControls controls = active.get();
        boolean connected = controller == CONTROLLER && controls != null &&
                controls.activity == owner && controls.virtualConnected;
        if (loggedPresence.getAndIncrement() < 24) {
            Log.i(TAG, "presence query controller=" + controller + " connected=" + connected);
        }
        return connected;
    }

    public static void refresh(Activity owner) {
        // This callback can come from the native render thread. It only reads
        // volatile presence; all Android view/input ownership changes stay on UI.
        LaunchControls controls = active.get();
        if (controls != null && controls.activity == owner && controls.virtualConnected) {
            controls.sendDevice(true);
        }
    }

    private void refreshOwnership() {
        boolean physicalOwnsSlot = false;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (device != null && device.getControllerNumber() == CONTROLLER) {
                physicalOwnsSlot = true;
                break;
            }
        }
        boolean next = !physicalOwnsSlot && !disposed;
        if (next != virtualConnected) {
            // Never interleave virtual key releases with a physically held key
            // in the same controller slot. This milestone disables the overlay
            // while a real controller owns slot1; physical handlers are unchanged.
            releaseAll();
            virtualConnected = next;
            if (next) sendDevice(true);
            Log.i(TAG, "Virtual controller presence=" + next + " physicalSlot1=" + physicalOwnsSlot);
        }
        setVisibility(next && !nativeTouchOwns ? View.VISIBLE : View.GONE);
    }

    private void sendDevice(boolean connected) {
        TouchSources.nativeBeginLegacy();
        try {
            Object accepted = deviceEvent.invoke(activity, CONTROLLER, GENERIC_GAMEPAD, connected);
            Log.i(TAG, "device controller=" + CONTROLLER + " type=" + GENERIC_GAMEPAD +
                    " connected=" + connected + " accepted=" + accepted);
        } catch (ReflectiveOperationException error) {
            Log.e(TAG, "Device event failed", error);
        } finally {
            TouchSources.nativeEndLegacy();
        }
    }

    private void sendKey(int key, boolean down) {
        TouchSources.nativeBeginLegacy();
        try {
            Object accepted = keyEvent.invoke(activity, CONTROLLER,
                    down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                    key, 0, SystemClock.uptimeMillis());
            if (loggedKeys++ < 48) Log.i(TAG, "key=" + key + " down=" + down + " accepted=" + accepted);
        } catch (ReflectiveOperationException error) {
            Log.e(TAG, "Key event failed key=" + key, error);
        } finally {
            TouchSources.nativeEndLegacy();
        }
    }

    private void sendMotionState(int pointer) {
        TouchSources.nativeBeginLegacy();
        try {
            Object accepted = motionEvent.invoke(activity, CONTROLLER, MotionEvent.ACTION_MOVE,
                    0f, 0f, stick.x, stick.y, cameraStick.x, cameraStick.y,
                    leftTrigger, rightTrigger,
                    pointer, SystemClock.uptimeMillis());
            int activeMask = (stick.x != 0f ? 1 : 0) |
                    (stick.y != 0f ? 2 : 0) |
                    (cameraStick.x != 0f ? 4 : 0) |
                    (cameraStick.y != 0f ? 8 : 0) |
                    (leftTrigger != 0f ? 16 : 0) |
                    (rightTrigger != 0f ? 32 : 0);
            if (loggedMotions++ < 16 || activeMask != lastMotionMask) {
                Log.i(TAG, "axes lx=" + stick.x + " ly=" + stick.y +
                        " rx=" + cameraStick.x + " ry=" + cameraStick.y +
                        " l2=" + leftTrigger + " r2=" + rightTrigger +
                        " pointer=" + pointer + " accepted=" + accepted);
            }
            lastMotionMask = activeMask;
        } catch (ReflectiveOperationException error) {
            Log.e(TAG, "Motion event failed", error);
        } finally {
            TouchSources.nativeEndLegacy();
        }
    }

    private void releaseAll() {
        for (KeyButton button : buttons) button.release();
        stick.release();
        cameraStick.release();
    }

    private void dispose() {
        if (disposed) return;
        releaseAll();
        disposed = true;
        virtualConnected = false;
        inputManager.unregisterInputDeviceListener(this);
        if (getParent() instanceof ViewGroup) ((ViewGroup) getParent()).removeView(this);
        if (active.get() == this) active = new WeakReference<>(null);
    }

    @Override public void onInputDeviceAdded(int id) { refreshOwnership(); }
    @Override public void onInputDeviceRemoved(int id) { refreshOwnership(); }
    @Override public void onInputDeviceChanged(int id) { refreshOwnership(); }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        super.onLayout(changed, l, t, r, b);
        fitControls();
        if (changed) loggedLayout = false;
        if (!loggedLayout && getWidth() > 0) {
            loggedLayout = true;
            for (KeyButton button : buttons) logBounds(button, "key=" + button.key);
            logBounds(stick, "left-stick");
            logBounds(cameraStick, "right-stick");
        }
    }

    private void logBounds(View view, String label) {
        Rect bounds = new Rect();
        if (!view.getGlobalVisibleRect(bounds)) return;
        int[] rootPosition = new int[2];
        view.getRootView().getLocationOnScreen(rootPosition);
        bounds.offset(rootPosition[0], rootPosition[1]);
        Log.i(TAG, "bounds " + label + " screen=" + bounds.left + "," + bounds.top +
                " size=" + bounds.width() + "x" + bounds.height());
    }

    private void fitControls() {
        int width = getWidth() - getPaddingLeft() - getPaddingRight();
        int height = getHeight() - getPaddingTop() - getPaddingBottom();
        if (width <= 0 || height <= 0) return;
        LayoutParams left = (LayoutParams) stick.getLayoutParams();
        LayoutParams right = (LayoutParams) cameraStick.getLayoutParams();
        int requiredWidth = left.leftMargin + stick.getWidth() +
                right.rightMargin + cameraStick.getWidth() + dp(12);
        int requiredHeight = 1;
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            LayoutParams params = (LayoutParams) child.getLayoutParams();
            requiredHeight = Math.max(requiredHeight,
                    child.getHeight() + params.topMargin + params.bottomMargin);
        }
        float scale = Math.min(1f, Math.min((float) width / requiredWidth,
                (float) height / requiredHeight));
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            LayoutParams params = (LayoutParams) child.getLayoutParams();
            boolean rightSide = (params.gravity & Gravity.HORIZONTAL_GRAVITY_MASK) == Gravity.RIGHT;
            if (scale != child.getScaleX()) loggedLayout = false;
            child.setPivotX(rightSide ? child.getWidth() : 0f);
            child.setPivotY(child.getHeight());
            child.setScaleX(scale);
            child.setScaleY(scale);
            child.setTranslationX((rightSide ? params.rightMargin : -params.leftMargin) * (1f - scale));
            child.setTranslationY(params.bottomMargin * (1f - scale));
        }
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN && loggedTouches++ < 12) {
            Log.i(TAG, "touch raw=" + event.getRawX() + "," + event.getRawY() +
                    " local=" + event.getX() + "," + event.getY() + " focused=" + focused +
                    " connected=" + virtualConnected);
        }
        return super.dispatchTouchEvent(event);
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private void addButton(FrameLayout parent, String label, int key, int x, int y, int width) {
        KeyButton button = new KeyButton(label, key);
        buttons.add(button);
        LayoutParams params = new LayoutParams(dp(width - 4), dp(48));
        params.leftMargin = dp(x + 2);
        params.topMargin = dp(y + 2);
        parent.addView(button, params);
    }

    private final class StickView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final boolean left;
        private int pointer = -1;
        private float x;
        private float y;
        private Runnable repeat;

        StickView(Context context, boolean isLeft) {
            super(context);
            left = isLeft;
            setContentDescription(left ? "Game left stick" : "Game right stick");
            setFocusable(false);
            setClickable(true);
        }

        @Override protected void onDraw(Canvas canvas) {
            float cx = getWidth() * 0.5f;
            float cy = getHeight() * 0.5f;
            float radius = Math.min(cx, cy) - dp(8);
            paint.setColor(0x88202020);
            canvas.drawCircle(cx, cy, radius, paint);
            paint.setColor(0xBBEEEEEE);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(1));
            canvas.drawCircle(cx, cy, radius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xCC27616C);
            canvas.drawCircle(cx + x * radius * 0.55f, cy + y * radius * 0.55f,
                    dp(24), paint);
            paint.setColor(Color.WHITE);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(dp(14));
            canvas.drawText(left ? "LS" : "RS", cx, cy + dp(5), paint);
        }

        private void update(float touchX, float touchY) {
            float cx = getWidth() * 0.5f;
            float cy = getHeight() * 0.5f;
            float radius = Math.max(1f, Math.min(cx, cy) - dp(24));
            float nx = (touchX - cx) / radius;
            float ny = (touchY - cy) / radius;
            float magnitude = (float) Math.hypot(nx, ny);
            if (magnitude > 1f) {
                nx /= magnitude;
                ny /= magnitude;
            } else if (magnitude < 0.12f) {
                nx = 0f;
                ny = 0f;
            }
            if (nx != x || ny != y) {
                x = nx;
                y = ny;
                sendMotionState(pointer);
                invalidate();
            }
        }

        void release() {
            if (repeat != null) {
                uiHandler.removeCallbacks(repeat);
                repeat = null;
            }
            boolean wasHeld = pointer >= 0;
            pointer = -1;
            x = 0f;
            y = 0f;
            if (wasHeld) sendMotionState(-1);
            invalidate();
        }

        private void startRepeat() {
            repeat = new Runnable() {
                @Override public void run() {
                    if (pointer < 0 || !focused || !virtualConnected || disposed) {
                        release();
                        return;
                    }
                    sendMotionState(pointer);
                    uiHandler.postDelayed(this, STICK_REPEAT_MS);
                }
            };
            uiHandler.postDelayed(repeat, STICK_REPEAT_MS);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (!virtualConnected || !focused || disposed) {
                release();
                return false;
            }
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                release();
                pointer = event.getPointerId(event.getActionIndex());
                update(event.getX(event.getActionIndex()), event.getY(event.getActionIndex()));
                startRepeat();
            } else if (action == MotionEvent.ACTION_MOVE && pointer >= 0) {
                int index = event.findPointerIndex(pointer);
                if (index >= 0) update(event.getX(index), event.getY(index));
                else release();
            } else if (action == MotionEvent.ACTION_CANCEL) {
                release();
            } else if ((action == MotionEvent.ACTION_UP ||
                    action == MotionEvent.ACTION_POINTER_UP) &&
                    event.getPointerId(event.getActionIndex()) == pointer) {
                release();
            }
            return true;
        }
    }

    private final class KeyButton extends TextView {
        private final int key;
        private int pointer = -1;
        private boolean held;
        private long pressedAt;
        private Runnable pendingRelease;
        private boolean touchClick;

        KeyButton(String label, int keyCode) {
            super(activity);
            key = keyCode;
            setText(label);
            setContentDescription("Game " + label);
            setTextSize(14);
            setTextColor(Color.WHITE);
            setGravity(Gravity.CENTER);
            setFocusable(false);
            setClickable(true);
            paint(false);
        }

        private void paint(boolean pressed) {
            GradientDrawable background = new GradientDrawable();
            background.setColor(pressed ? 0xCC27616C : 0x88202020);
            background.setCornerRadius(dp(10));
            background.setStroke(dp(1), 0xBBEEEEEE);
            setBackground(background);
        }

        void release() {
            if (pendingRelease != null) {
                uiHandler.removeCallbacks(pendingRelease);
                pendingRelease = null;
            }
            if (held) {
                held = false;
                sendButtonState(false);
                paint(false);
            }
            pointer = -1;
        }

        private void releaseAfterTap() {
            pointer = -1;
            if (!held) return;
            // ponytail: timed dwell can lose edges below 7 FPS; use the guest input-frame seam if observed.
            long minimum = key < 0 ? MIN_TRIGGER_PRESS_MS :
                    (key == KeyEvent.KEYCODE_BUTTON_A || key == KeyEvent.KEYCODE_BUTTON_START
                            ? MIN_MENU_PRESS_MS : MIN_BUTTON_PRESS_MS);
            long remaining = minimum -
                    (SystemClock.uptimeMillis() - pressedAt);
            if (remaining <= 0) {
                release();
                return;
            }
            if (pendingRelease != null) uiHandler.removeCallbacks(pendingRelease);
            pendingRelease = new Runnable() {
                @Override public void run() {
                    pendingRelease = null;
                    if (pointer < 0) release();
                }
            };
            uiHandler.postDelayed(pendingRelease, remaining);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (!virtualConnected || !focused || disposed) {
                release();
                return false;
            }
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                if (pendingRelease != null) {
                    uiHandler.removeCallbacks(pendingRelease);
                    pendingRelease = null;
                }
                pointer = event.getPointerId(event.getActionIndex());
                if (!held) {
                    held = true;
                    pressedAt = SystemClock.uptimeMillis();
                    sendButtonState(true);
                    paint(true);
                }
            } else if (action == MotionEvent.ACTION_CANCEL) {
                release();
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                if (event.getPointerId(event.getActionIndex()) == pointer) {
                    releaseAfterTap();
                    touchClick = true;
                    try {
                        performClick();
                    } finally {
                        touchClick = false;
                    }
                }
            } else if (action == MotionEvent.ACTION_MOVE && held) {
                int index = event.findPointerIndex(pointer);
                if (index < 0 || event.getX(index) < 0 || event.getY(index) < 0 ||
                        event.getX(index) >= getWidth() || event.getY(index) >= getHeight()) release();
            }
            return true;
        }

        @Override public boolean performClick() {
            if (!touchClick && virtualConnected && focused && !disposed) {
                if (!held) {
                    held = true;
                    pressedAt = SystemClock.uptimeMillis();
                    sendButtonState(true);
                    paint(true);
                }
                releaseAfterTap();
            }
            return super.performClick();
        }

        private void sendButtonState(boolean down) {
            if (key >= 0) {
                sendKey(key, down);
            } else {
                if (key == -1) leftTrigger = down ? 1f : 0f;
                else rightTrigger = down ? 1f : 0f;
                sendMotionState(pointer);
            }
        }
    }
}
