package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

/** Requests the fastest OS-supported mode at the current physical resolution. */
public final class DisplayModePolicy implements DisplayManager.DisplayListener {
    private static final String TAG = "BL2_DisplayMode";
    private static DisplayModePolicy active;

    private final Activity activity;
    private final DisplayManager manager;
    private final int displayId;
    private boolean disposed;
    private float activeRefreshRate;

    private DisplayModePolicy(Activity owner, DisplayManager displayManager, int id) {
        activity = owner;
        manager = displayManager;
        displayId = id;
    }

    public static void attach(Activity owner) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            Log.e(TAG, "Display mode setup must run on the UI thread");
            return;
        }
        DisplayModePolicy old = active;
        if (old != null && old.activity == owner && !old.disposed) {
            old.requestBestMode();
            return;
        }
        if (old != null) old.dispose();
        DisplayManager manager = (DisplayManager) owner.getSystemService(Context.DISPLAY_SERVICE);
        Display display = owner.getWindowManager().getDefaultDisplay();
        if (manager == null || display == null) {
            Log.e(TAG, "Display mode service unavailable");
            return;
        }
        DisplayModePolicy policy = new DisplayModePolicy(owner, manager, display.getDisplayId());
        active = policy;
        manager.registerDisplayListener(policy, new Handler(Looper.getMainLooper()));
        policy.requestBestMode();
    }

    public static void refresh(Activity owner) {
        DisplayModePolicy policy = active;
        if (policy != null && policy.activity == owner && !policy.disposed) {
            policy.requestBestMode();
        }
    }

    public static void detach(Activity owner) {
        DisplayModePolicy policy = active;
        if (policy != null && policy.activity == owner) policy.dispose();
    }

    private void requestBestMode() {
        Display display = manager.getDisplay(displayId);
        if (display == null) {
            Log.e(TAG, "Active display unavailable");
            return;
        }
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth() &&
                    mode.getPhysicalHeight() == current.getPhysicalHeight() &&
                    mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams attributes = activity.getWindow().getAttributes();
        if (attributes.preferredDisplayModeId != best.getModeId() ||
                attributes.preferredRefreshRate != best.getRefreshRate()) {
            attributes.preferredDisplayModeId = best.getModeId();
            attributes.preferredRefreshRate = best.getRefreshRate();
            activity.getWindow().setAttributes(attributes);
        }
        Log.i(TAG, "requested mode=" + best.getModeId() + " " +
                best.getPhysicalWidth() + "x" + best.getPhysicalHeight() +
                "@" + best.getRefreshRate() + "Hz");
        readActiveMode();
    }

    private void readActiveMode() {
        Display display = manager.getDisplay(displayId);
        if (display == null) return;
        Display.Mode mode = display.getMode();
        activeRefreshRate = mode.getRefreshRate();
        Log.i(TAG, "active mode=" + mode.getModeId() + " " +
                mode.getPhysicalWidth() + "x" + mode.getPhysicalHeight() +
                "@" + activeRefreshRate + "Hz");
    }

    private void dispose() {
        if (disposed) return;
        disposed = true;
        manager.unregisterDisplayListener(this);
        if (active == this) active = null;
    }

    @Override public void onDisplayAdded(int id) {}
    @Override public void onDisplayRemoved(int id) {}
    @Override public void onDisplayChanged(int id) {
        if (!disposed && id == displayId) readActiveMode();
    }
}
