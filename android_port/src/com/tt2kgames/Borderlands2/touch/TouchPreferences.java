package com.tt2kgames.Borderlands2.touch;

import android.app.Activity;
import android.content.SharedPreferences;
import android.util.Log;
import com.tt2kgames.Borderlands2.GameStorage;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Map;
import java.util.Properties;

/** Layout is saved in normalized safe-rect coordinates under the game data root. */
public final class TouchPreferences {
    private static final String TAG = "BL2TouchPreferences";
    private static final int MAX_SETTINGS_BYTES = 64 * 1024;
    private final Activity activity;
    private final Properties store = new Properties();
    public float opacity, scale, look, ads;
    public boolean leftHanded, invertY;
    public TouchPreferences(Activity owner) {
        activity = owner;
        File target = new File(GameStorage.root(activity), "touch-controls.properties");
        if (target.isFile()) {
            try (FileInputStream input = new FileInputStream(target)) {
                if (target.length() > MAX_SETTINGS_BYTES) throw new IOException("Touch settings too large");
                byte[] bytes = new byte[MAX_SETTINGS_BYTES + 1];
                int used = 0, count;
                while (used < bytes.length && (count = input.read(bytes, used, bytes.length - used)) != -1) used += count;
                if (used > MAX_SETTINGS_BYTES) throw new IOException("Touch settings grew beyond size limit");
                store.load(new ByteArrayInputStream(bytes, 0, used));
            } catch (IOException | IllegalArgumentException error) {
                store.clear();
                Log.e(TAG, "Cannot read touch settings; retaining file", error);
            }
        } else {
            SharedPreferences legacy = activity.getSharedPreferences("bl2_native_touch_v1", 0);
            for (Map.Entry<String, ?> entry : legacy.getAll().entrySet()) {
                store.setProperty(entry.getKey(), String.valueOf(entry.getValue()));
            }
            // Legacy preferences remain intact until restart/readback acceptance.
            persist();
        }
        reload();
    }
    private float number(String key, float fallback) {
        try { return Float.parseFloat(store.getProperty(key, Float.toString(fallback))); }
        catch (NumberFormatException error) { return fallback; }
    }
    public void reload() {
        opacity = TouchInput.finiteClamp(number("opacity", .42f), .15f, .9f);
        scale = TouchInput.finiteClamp(number("scale", 1), .85f, 1.4f);
        look = TouchInput.finiteClamp(number("look", .22f), .04f, 1);
        ads = TouchInput.finiteClamp(number("ads", .55f), .1f, 1);
        leftHanded = Boolean.parseBoolean(store.getProperty("leftHanded", "false"));
        invertY = Boolean.parseBoolean(store.getProperty("invertY", "false"));
    }
    public float x(int action, float fallback) {
        return TouchInput.finiteClamp(number("x_" + action, fallback), 0, 1);
    }
    public float y(int action, float fallback) {
        return TouchInput.finiteClamp(number("y_" + action, fallback), 0, 1);
    }
    public void position(int action, float x, float y) {
        store.setProperty("x_" + action, Float.toString(x));
        store.setProperty("y_" + action, Float.toString(y));
        persist();
    }
    public void save() {
        store.setProperty("opacity", Float.toString(opacity));
        store.setProperty("scale", Float.toString(scale));
        store.setProperty("look", Float.toString(look));
        store.setProperty("ads", Float.toString(ads));
        store.setProperty("leftHanded", Boolean.toString(leftHanded));
        store.setProperty("invertY", Boolean.toString(invertY));
        persist();
    }
    public void reset() { store.clear(); persist(); reload(); }
    private void persist() {
        File temporary = null;
        try {
            File external = activity.getExternalFilesDir(null);
            File target = GameStorage.file(activity, "touch-controls.properties");
            if (external == null || !target.getParentFile().getCanonicalFile().equals(external.getCanonicalFile())) {
                throw new IOException("External game data unavailable; retaining legacy preferences");
            }
            temporary = new File(target.getParentFile(), target.getName() + ".tmp");
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                store.store(output, "Borderlands 2 touch settings");
                output.getFD().sync();
            }
            Properties check = new Properties();
            try (FileInputStream input = new FileInputStream(temporary)) { check.load(input); }
            if (!store.equals(check)) throw new IOException("Touch settings verification failed");
            if (!temporary.renameTo(target)) throw new IOException("Cannot commit touch settings");
        } catch (IOException | SecurityException error) {
            Log.e(TAG, "Cannot save external touch settings", error);
        } finally {
            if (temporary != null && temporary.exists()) temporary.delete();
        }
    }
}
