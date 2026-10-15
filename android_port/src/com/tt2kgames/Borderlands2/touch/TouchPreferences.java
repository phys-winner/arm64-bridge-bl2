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
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Layout is saved in normalized safe-rect coordinates under the game data root. */
public final class TouchPreferences {
    private static final String TAG = "BL2TouchPreferences";
    private static final int MAX_SETTINGS_BYTES = 64 * 1024;
    /**
     * A change is written this long after the last one, off the UI thread: the
     * settings exchange polls every 100 ms while a slider moves, and one FUSE
     * write with fsync and read-back costs tens of milliseconds on device.
     */
    static final long SAVE_DELAY_MS = 500;
    private static final ScheduledExecutorService WRITER = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "BL2TouchSettings");
        thread.setDaemon(true);
        return thread;
    });
    static final int SETTINGS_VERSION = 8, SETTINGS_FIELDS = 37;
    /** Settings 32 onward, all ON by default. */
    private static final String[] PERFORMANCE_KEYS = {"fasterVisibility", "fasterAudioLimiter", "framePacing"};
    private static final String[] GRAPHICS_KEYS = {"bloom", "motionBlur", "lensFlares", "dynamicShadows",
            "lightEnvironmentShadows", "staticDecals", "unbatchedDecals", "lightShafts", "distortion",
            "radialBlur", "anisotropy"};
    static final int HINT_MAP = 1, HINT_WEAPON = 2, HINT_GAME_MORE = 3,
            HINT_MENU_MORE = 4, HINT_DEBUG_MORE = 5, HINT_AUTORUN = 6,
            HINT_MAP_NAVIGATION = 7, HINT_LAYOUT = 8, HINT_SLOTS = 9, HINT_SKILL_TAP = 10, HINT_STATUS_TAPS = 11;
    private static final String[] HINT_KEYS = {"", "mapTapHintSeen", "weaponGestureHintSeen",
            "gameMoreHintSeen", "menuMoreHintSeen", "debugMoreHintSeen", "autoRunHintSeen",
            "mapNavigationHintSeen", "layoutHintSeen", "weaponSlotsHintSeen", "skillTapHintSeen", "statusTapsHintSeen"};
    private final Activity activity;
    /** Host checks only: written without the external game data check. */
    private final File hostFile;
    private final Properties store = new Properties();
    private final Object writeLock = new Object();
    private boolean dirty;
    private ScheduledFuture<?> scheduledWrite;
    /** Completed file commits, for host checks. */
    volatile int writes;
    public float opacity, scale, look, ads;
    public volatile int renderPercent, uiPercent, fpsLimit;
    public boolean showFPS;
    public boolean potatoGraphics;
    public final int[] advancedGraphics = new int[GRAPHICS_KEYS.length];
    public final boolean[] performance = new boolean[PERFORMANCE_KEYS.length];
    public volatile boolean microphoneEnabled;
    public boolean leftHanded, invertY, autoSprint;
    public boolean gyroEnabled, gyroAdsOnly, gyroInvertY, haptics, aimToggle;
    /** Show Reload while the native route reports that a reload is possible. */
    public boolean reloadButton;
    /** A short tap on More keeps the radial open for a second tap (RADIAL BY TAP). */
    public boolean radialTap;
    public boolean debugMenuVisible;
    public boolean mapTapHintSeen, weaponGestureHintSeen;
    public boolean gameMoreHintSeen, menuMoreHintSeen, debugMoreHintSeen, autoRunHintSeen;
    public boolean mapNavigationHintSeen, layoutHintSeen;
    public boolean weaponSlotsHintSeen, skillTapHintSeen, statusTapsHintSeen;
    private boolean debugMenuStored;
    public float gyroSensitivity, gyroDeadzone;
    /** In-memory preferences for host protocol checks. */
    TouchPreferences() { activity = null; hostFile = null; reload(); }
    /** File-backed preferences for host checks of the deferred writer. */
    TouchPreferences(File file) {
        activity = null; hostFile = file;
        load(file);
        reload();
    }
    public TouchPreferences(Activity owner) {
        activity = owner; hostFile = null;
        File target = new File(GameStorage.root(activity), "touch-controls.properties");
        if (target.isFile()) load(target);
        else {
            SharedPreferences legacy = activity.getSharedPreferences("bl2_native_touch_v1", 0);
            for (Map.Entry<String, ?> entry : legacy.getAll().entrySet()) {
                store.setProperty(entry.getKey(), String.valueOf(entry.getValue()));
            }
            // Legacy preferences remain intact until restart/readback acceptance.
            persist();
        }
        reload();
    }
    private void load(File target) {
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
        }
    }
    private float number(String key, float fallback) {
        try { return Float.parseFloat(store.getProperty(key, Float.toString(fallback))); }
        catch (NumberFormatException error) { return fallback; }
    }
    public void reload() {
        renderPercent = renderPercent(number("renderPercent", 50));
        uiPercent = uiPercent(number("uiPercent", 125));
        fpsLimit = fpsLimit(number("fpsLimit", 0));
        showFPS = Boolean.parseBoolean(store.getProperty("showFPS", "false"));
        potatoGraphics = Boolean.parseBoolean(store.getProperty("potatoGraphics", "false"));
        for (int i = 0; i < performance.length; ++i)
            performance[i] = Boolean.parseBoolean(store.getProperty(PERFORMANCE_KEYS[i], "true"));
        for (int i = 0; i < advancedGraphics.length; ++i) {
            int value = -1;
            try { value = Integer.parseInt(store.getProperty(GRAPHICS_KEYS[i], "-1")); }
            catch (NumberFormatException ignored) { }
            advancedGraphics[i] = value >= 0 && value <= (i == 10 ? 4 : 1) ? value : -1;
        }
        advancedGraphics[1] = 0; // Motion Blur is disabled, including saved values from older builds.
        opacity = TouchInput.finiteClamp(number("opacity", .42f), .15f, .9f);
        scale = TouchInput.finiteClamp(number("scale", 1), .85f, 1.4f);
        look = TouchInput.finiteClamp(number("look", .22f), .04f, 1);
        ads = TouchInput.finiteClamp(number("ads", .55f), .1f, 1);
        leftHanded = Boolean.parseBoolean(store.getProperty("leftHanded", "false"));
        invertY = Boolean.parseBoolean(store.getProperty("invertY", "false"));
        // Old builds saved autoSprint=true even without a deliberate opt-in.
        // A separate key makes the new optional latch default OFF on migration.
        autoSprint = Boolean.parseBoolean(store.getProperty("autoRunLatch", "false"));
        microphoneEnabled = Boolean.parseBoolean(store.getProperty("microphoneEnabled", "false"));
        gyroEnabled = Boolean.parseBoolean(store.getProperty("gyroEnabled", "true"));
        gyroAdsOnly = Boolean.parseBoolean(store.getProperty("gyroAdsOnly", "true"));
        gyroInvertY = Boolean.parseBoolean(store.getProperty("gyroInvertY", "false"));
        haptics = Boolean.parseBoolean(store.getProperty("haptics", "true"));
        aimToggle = Boolean.parseBoolean(store.getProperty("aimToggle", "true"));
        reloadButton = Boolean.parseBoolean(store.getProperty("reloadButton", "true"));
        radialTap = Boolean.parseBoolean(store.getProperty("radialTap", "false"));
        debugMenuStored = store.containsKey("debugMenuVisible");
        debugMenuVisible = Boolean.parseBoolean(store.getProperty("debugMenuVisible", "false"));
        mapTapHintSeen = Boolean.parseBoolean(store.getProperty("mapTapHintSeen", "false"));
        weaponGestureHintSeen = Boolean.parseBoolean(store.getProperty("weaponGestureHintSeen", "false"));
        gameMoreHintSeen = Boolean.parseBoolean(store.getProperty("gameMoreHintSeen", "false"));
        menuMoreHintSeen = Boolean.parseBoolean(store.getProperty("menuMoreHintSeen", "false"));
        debugMoreHintSeen = Boolean.parseBoolean(store.getProperty("debugMoreHintSeen", "false"));
        autoRunHintSeen = Boolean.parseBoolean(store.getProperty("autoRunHintSeen", "false"));
        mapNavigationHintSeen = Boolean.parseBoolean(store.getProperty("mapNavigationHintSeen", "false"));
        layoutHintSeen = Boolean.parseBoolean(store.getProperty("layoutHintSeen", "false"));
        weaponSlotsHintSeen = Boolean.parseBoolean(store.getProperty("weaponSlotsHintSeen", "false"));
        skillTapHintSeen = Boolean.parseBoolean(store.getProperty("skillTapHintSeen", "false"));
        statusTapsHintSeen = Boolean.parseBoolean(store.getProperty("statusTapsHintSeen", "false"));
        gyroSensitivity = TouchInput.finiteClamp(number("gyroSensitivity", 1), .25f, 3);
        gyroDeadzone = TouchInput.finiteClamp(number("gyroDeadzone", .4f), 0, 3);
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
    }
    public void save() {
        storeGraphics();
        store.setProperty("renderPercent", Integer.toString(renderPercent));
        uiPercent = uiPercent(uiPercent);
        store.setProperty("uiPercent", Integer.toString(uiPercent));
        fpsLimit = fpsLimit(fpsLimit);
        store.setProperty("fpsLimit", Integer.toString(fpsLimit));
        store.setProperty("showFPS", Boolean.toString(showFPS));
        store.setProperty("opacity", Float.toString(opacity));
        store.setProperty("scale", Float.toString(scale));
        store.setProperty("look", Float.toString(look));
        store.setProperty("ads", Float.toString(ads));
        store.setProperty("leftHanded", Boolean.toString(leftHanded));
        store.setProperty("invertY", Boolean.toString(invertY));
        store.setProperty("autoRunLatch", Boolean.toString(autoSprint));
        store.setProperty("microphoneEnabled", Boolean.toString(microphoneEnabled));
        store.setProperty("gyroEnabled", Boolean.toString(gyroEnabled));
        store.setProperty("gyroAdsOnly", Boolean.toString(gyroAdsOnly));
        store.setProperty("gyroInvertY", Boolean.toString(gyroInvertY));
        store.setProperty("haptics", Boolean.toString(haptics));
        store.setProperty("aimToggle", Boolean.toString(aimToggle));
        store.setProperty("reloadButton", Boolean.toString(reloadButton));
        store.setProperty("radialTap", Boolean.toString(radialTap));
        if (debugMenuStored) store.setProperty("debugMenuVisible", Boolean.toString(debugMenuVisible));
        store.setProperty("gyroSensitivity", Float.toString(gyroSensitivity));
        store.setProperty("gyroDeadzone", Float.toString(gyroDeadzone));
        persist();
    }
    public void reset() { store.clear(); persist(); reload(); }
    /** Acknowledge a hint only after it was actually presented in gameplay. */
    public void markHintSeen(boolean weapon) {
        markHintSeen(weapon ? HINT_WEAPON : HINT_MAP);
    }
    public boolean hintSeen(int kind) {
        switch (kind) {
            case HINT_MAP: return mapTapHintSeen;
            case HINT_WEAPON: return weaponGestureHintSeen;
            case HINT_GAME_MORE: return gameMoreHintSeen;
            case HINT_MENU_MORE: return menuMoreHintSeen;
            case HINT_DEBUG_MORE: return debugMoreHintSeen;
            case HINT_AUTORUN: return autoRunHintSeen;
            case HINT_MAP_NAVIGATION: return mapNavigationHintSeen;
            case HINT_LAYOUT: return layoutHintSeen;
            case HINT_SLOTS: return weaponSlotsHintSeen;
            case HINT_SKILL_TAP: return skillTapHintSeen;
            case HINT_STATUS_TAPS: return statusTapsHintSeen;
            default: return false;
        }
    }
    public void markHintSeen(int kind) {
        switch (kind) {
            case HINT_MAP: mapTapHintSeen = true; break;
            case HINT_WEAPON: weaponGestureHintSeen = true; break;
            case HINT_GAME_MORE: gameMoreHintSeen = true; break;
            case HINT_MENU_MORE: menuMoreHintSeen = true; break;
            case HINT_DEBUG_MORE: debugMoreHintSeen = true; break;
            case HINT_AUTORUN: autoRunHintSeen = true; break;
            case HINT_MAP_NAVIGATION: mapNavigationHintSeen = true; break;
            case HINT_LAYOUT: layoutHintSeen = true; break;
            case HINT_SLOTS: weaponSlotsHintSeen = true; break;
            case HINT_SKILL_TAP: skillTapHintSeen = true; break;
            case HINT_STATUS_TAPS: statusTapsHintSeen = true; break;
            default: return;
        }
        store.setProperty(HINT_KEYS[kind], "true");
        persist();
    }
    /** Reset controls while keeping deliberate graphics, interface and microphone choices. */
    public void resetTouch() {
        boolean debugVisible = debugMenuVisible;
        boolean[] seen = new boolean[HINT_KEYS.length];
        for (int i = 1; i < seen.length; ++i) seen[i] = hintSeen(i);
        store.clear();
        storeGraphics();
        for (int i = 1; i < seen.length; ++i) if (seen[i]) store.setProperty(HINT_KEYS[i], "true");
        store.setProperty("renderPercent", Integer.toString(renderPercent));
        store.setProperty("uiPercent", Integer.toString(uiPercent));
        store.setProperty("fpsLimit", Integer.toString(fpsLimit));
        store.setProperty("showFPS", Boolean.toString(showFPS));
        store.setProperty("microphoneEnabled", Boolean.toString(microphoneEnabled));
        if (debugMenuStored) store.setProperty("debugMenuVisible", Boolean.toString(debugMenuVisible));
        reload(); debugMenuVisible = debugVisible; save();
    }
    private void storeGraphics() {
        for (int i = 0; i < performance.length; ++i)
            store.setProperty(PERFORMANCE_KEYS[i], Boolean.toString(performance[i]));
        store.setProperty("potatoGraphics", Boolean.toString(potatoGraphics));
        for (int i = 0; i < advancedGraphics.length; ++i) {
            if (advancedGraphics[i] < 0) store.remove(GRAPHICS_KEYS[i]);
            else store.setProperty(GRAPHICS_KEYS[i], Integer.toString(advancedGraphics[i]));
        }
    }
    /** Shared with bl2_touch_settings: version 8, acknowledged revision, 37 fixed fields. */
    public int[] settingsWire(int revision) {
        int[] controls = new int[]{SETTINGS_VERSION, revision, renderPercent, Float.floatToIntBits(opacity), Float.floatToIntBits(scale),
            Float.floatToIntBits(look), Float.floatToIntBits(ads), leftHanded?1:0, invertY?1:0,
            autoSprint?1:0, microphoneEnabled?1:0, gyroEnabled?1:0, gyroAdsOnly?1:0,
            Float.floatToIntBits(gyroSensitivity), Float.floatToIntBits(gyroDeadzone), gyroInvertY?1:0,
            haptics?1:0, aimToggle?1:0, debugMenuStored?(debugMenuVisible?1:0):-1, uiPercent, showFPS?1:0, fpsLimit};
        int[] values = java.util.Arrays.copyOf(controls, SETTINGS_FIELDS + 2);
        values[22] = potatoGraphics ? 1 : 0;
        System.arraycopy(advancedGraphics, 0, values, 23, advancedGraphics.length);
        values[24] = 0;
        for (int i = 0; i < performance.length; ++i) values[34+i] = performance[i] ? 1 : 0;
        values[37] = reloadButton ? 1 : 0;
        values[38] = radialTap ? 1 : 0;
        return values;
    }
    private static boolean wireFloat(int bits, float low, float high) {
        float value = Float.intBitsToFloat(bits);
        return Float.isFinite(value) && value >= low && value <= high;
    }
    public static boolean validSettingsWire(int[] words) {
        // Command bits 1-8, plus 16 while the native video options page is shown.
        if (words == null || words.length != SETTINGS_FIELDS+3 || words[0] != SETTINGS_VERSION || words[1] < 1 || (words[2]&~31) != 0)
            return false;
        int base = 3;
        if (words[base] < 20 || words[base] > 150 || words[base]%10 != 0
                || !wireFloat(words[base+1], .15f, .9f) || !wireFloat(words[base+2], .85f, 1.4f)
                || !wireFloat(words[base+3], .04f, 1) || !wireFloat(words[base+4], .1f, 1)
                || !wireFloat(words[base+11], .25f, 3) || !wireFloat(words[base+12], 0, 3)
                || words[base+17] < 100 || words[base+17] > 200
                || (words[base+17] != 125 && words[base+17]%10 != 0)
                || (words[base+19] != 0 && words[base+19] != 30 && words[base+19] != 60 && words[base+19] != 90)) return false;
        for (int i : new int[]{5,6,7,8,9,10,13,14,15,16,18}) if (words[base+i] != 0 && words[base+i] != 1) return false;
        if (words[base+20] != 0 && words[base+20] != 1) return false;
        for (int i = 21; i < 32; ++i)
            if (words[base+i] != -1 && (words[base+i] < 0 || words[base+i] > (i == 31 ? 4 : 1))) return false;
        for (int i = 32; i < SETTINGS_FIELDS; ++i)
            if (words[base+i] != 0 && words[base+i] != 1) return false;
        return true;
    }
    /** The game-thread provider owns changes; Java commits the same values externally. */
    public boolean applySettingsWire(int[] words) {
        if (!validSettingsWire(words)) return false;
        renderPercent=words[3]; opacity=Float.intBitsToFloat(words[4]); scale=Float.intBitsToFloat(words[5]);
        look=Float.intBitsToFloat(words[6]); ads=Float.intBitsToFloat(words[7]);
        leftHanded=words[8]!=0; invertY=words[9]!=0; autoSprint=words[10]!=0; microphoneEnabled=words[11]!=0;
        gyroEnabled=words[12]!=0; gyroAdsOnly=words[13]!=0; gyroSensitivity=Float.intBitsToFloat(words[14]);
        gyroDeadzone=Float.intBitsToFloat(words[15]); gyroInvertY=words[16]!=0; haptics=words[17]!=0; aimToggle=words[18]!=0;
        debugMenuVisible=words[19]!=0;
        uiPercent=words[20]; showFPS=words[21]!=0; fpsLimit=words[22];
        potatoGraphics=words[23]!=0;
        System.arraycopy(words, 24, advancedGraphics, 0, advancedGraphics.length);
        advancedGraphics[1] = 0;
        for (int i = 0; i < performance.length; ++i) performance[i] = words[35+i] != 0;
        reloadButton = words[38] != 0;
        radialTap = words[39] != 0;
        debugMenuStored |= (words[2]&8)!=0;
        // Reset wins over edits returned in the same poll, preserving non-touch choices.
        if ((words[2]&2)!=0) resetTouch(); else save();
        return true;
    }
    public static int renderPercent(float value) {
        if (!Float.isFinite(value)) return 50;
        return Math.round(Math.max(20, Math.min(150, value)) / 10) * 10;
    }
    public static int uiPercent(float value) {
        if (!Float.isFinite(value) || value == 125) return 125;
        return Math.round(Math.max(100, Math.min(200, value)) / 10) * 10;
    }
    public static int fpsLimit(float value) {
        return value == 30 || value == 60 || value == 90 ? (int)value : 0;
    }
    /** Marks the store changed; the writer commits it SAVE_DELAY_MS after the last change. */
    private void persist() {
        if (activity == null && hostFile == null) return;
        synchronized (writeLock) {
            dirty = true;
            if (scheduledWrite != null) scheduledWrite.cancel(false);
            scheduledWrite = WRITER.schedule(this::writePending, SAVE_DELAY_MS, TimeUnit.MILLISECONDS);
        }
    }
    /**
     * Commits a pending change now and waits for it: the activity pauses, the
     * overlay goes away or the process is about to be restarted.
     */
    public void flush() {
        Future<?> done;
        synchronized (writeLock) {
            if (!dirty) return;
            if (scheduledWrite != null) scheduledWrite.cancel(false);
            scheduledWrite = null;
            done = WRITER.submit(this::writePending);
        }
        try {
            done.get(2, TimeUnit.SECONDS);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        } catch (ExecutionException | TimeoutException error) {
            Log.e(TAG, "Touch settings flush incomplete", error);
        }
    }
    /** Writer thread: the latest snapshot, never an older one, wins the file. */
    private void writePending() {
        Properties snapshot;
        synchronized (writeLock) {
            if (!dirty) return;
            dirty = false;
            snapshot = (Properties) store.clone();
        }
        File temporary = null;
        try {
            File target = hostFile;
            if (activity != null) {
                File external = activity.getExternalFilesDir(null);
                target = GameStorage.file(activity, "touch-controls.properties");
                if (external == null || !target.getParentFile().getCanonicalFile().equals(external.getCanonicalFile())) {
                    throw new IOException("External game data unavailable; retaining legacy preferences");
                }
            }
            temporary = new File(target.getParentFile(), target.getName() + ".tmp");
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                snapshot.store(output, "Borderlands 2 touch settings");
                output.getFD().sync();
            }
            Properties check = new Properties();
            try (FileInputStream input = new FileInputStream(temporary)) { check.load(input); }
            if (!snapshot.equals(check)) throw new IOException("Touch settings verification failed");
            if (!temporary.renameTo(target)) throw new IOException("Cannot commit touch settings");
            synchronized (writeLock) { ++writes; }
        } catch (IOException | SecurityException error) {
            Log.e(TAG, "Cannot save external touch settings", error);
        } finally {
            if (temporary != null && temporary.exists()) temporary.delete();
        }
    }
}
