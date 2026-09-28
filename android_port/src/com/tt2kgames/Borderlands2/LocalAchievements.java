package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.util.AtomicFile;
import android.util.Log;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/** Preserve the historical encrypted oad<playerId>.bin offline event format. */
public final class LocalAchievements {
    private final Activity activity;
    private AtomicFile database;
    private String key;
    private boolean writable;
    private final LinkedHashSet<String> unlocked = new LinkedHashSet<>();
    private final LinkedHashSet<String> revealed = new LinkedHashSet<>();
    private final LinkedHashMap<String, Integer> incremented = new LinkedHashMap<>();

    public LocalAchievements(String tag, Activity owner) { activity = owner; }
    public synchronized void initialize(Object ignoredClient, File root, String player, String encryptionKey) {
        key = encryptionKey.length() >= 16 ? encryptionKey.substring(0, 16) : String.format("%1$-16s", encryptionKey);
        File value = new File(root, "oad" + player + ".bin");
        database = new AtomicFile(value);
        unlocked.clear(); revealed.clear(); incremented.clear(); writable = true;
        if (!value.exists() && !new File(value.getPath() + ".bak").exists()) return;
        try (InputStream in = database.openRead(); ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096]; int count;
            while ((count = in.read(buffer)) != -1) bytes.write(buffer, 0, count);
            if (bytes.size() == 0) return; // Original loadOfflineDB treats an empty file as an empty queue.
            String plain = new String(crypt(Cipher.DECRYPT_MODE, bytes.toByteArray()), StandardCharsets.UTF_8);
            try (BufferedReader lines = new BufferedReader(new StringReader(plain))) {
                String line;
                while ((line = lines.readLine()) != null) {
                    if (line.startsWith("unlocked=")) unlocked.add(line.substring(9));
                    else if (line.startsWith("revealed=")) revealed.add(line.substring(9));
                    else if (line.startsWith("incremented=")) {
                        int split = line.indexOf(',', 12);
                        if (split < 0) throw new IllegalArgumentException("Invalid achievement increment");
                        incremented.put(line.substring(12, split), Integer.parseInt(line.substring(split + 1)));
                    }
                }
            }
        } catch (Exception invalid) {
            writable = false; // Preserve an unreadable old DB rather than overwrite it with empty events.
            Log.w("BL2", "Cannot read local achievements; original DB preserved", invalid);
        }
    }
    private byte[] crypt(int mode, byte[] bytes) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/CBC/PKCS5Padding");
        cipher.init(mode, new SecretKeySpec(key.getBytes(StandardCharsets.UTF_8), "AES"),
                new IvParameterSpec("6B1A7314410C4f6b".getBytes(StandardCharsets.UTF_8)));
        return cipher.doFinal(bytes);
    }
    private boolean save() {
        if (database == null || !writable) return false;
        StringBuilder plain = new StringBuilder();
        for (String id : unlocked) plain.append("unlocked=").append(id).append('\n');
        for (String id : revealed) plain.append("revealed=").append(id).append('\n');
        for (Map.Entry<String, Integer> event : incremented.entrySet())
            plain.append("incremented=").append(event.getKey()).append(',').append(event.getValue()).append('\n');
        FileOutputStream out = null;
        try {
            byte[] bytes = crypt(Cipher.ENCRYPT_MODE, plain.toString().getBytes(StandardCharsets.UTF_8));
            out = database.startWrite(); out.write(bytes); database.finishWrite(out); return true;
        } catch (Exception failure) { database.failWrite(out); Log.w("BL2", "Cannot write local achievements", failure); return false; }
    }
    private String id(String name) {
        int value = activity.getResources().getIdentifier(name, "string", activity.getPackageName());
        return value == 0 ? null : activity.getString(value);
    }
    public synchronized boolean unlockByName(String name) {
        String id = id(name);
        if (id == null || !writable) return false;
        unlocked.add(id); return save();
    }
    public synchronized boolean incrementByName(String name, int steps) {
        String id = id(name);
        if (id == null || !writable) return false;
        incremented.put(id, steps); // Preserve the old last-value queue semantics, not a guessed cumulative counter.
        return save();
    }
    public boolean isShowingAchievements() { return false; }
    public void resetShowingAchievements() { }
    public boolean showUI() { return false; }
}
