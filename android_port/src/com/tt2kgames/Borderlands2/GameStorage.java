package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Properties;

/**
 * One root for guest saves and Java-owned player data. Legacy files are copied
 * before the new root is exposed; originals remain available for recovery.
 */
public final class GameStorage {
    private static final String TAG = "BL2_GameStorage";
    private static final String MARKER = ".legacy-migration.properties";
    private static File readyRoot;

    private GameStorage() {}

    /** Android's per-app configuration is authoritative, including system fallback. */
    public static String appLanguage(Context context) {
        return "ru".equals(context.getResources().getConfiguration()
                .getLocales().get(0).getLanguage()) ? "ru" : "en";
    }

    public static synchronized File root(Activity activity) {
        if (readyRoot != null && readyRoot.isDirectory()) return readyRoot;
        File legacy = activity.getFilesDir();
        File external = activity.getExternalFilesDir(null);
        if (external == null || (!external.isDirectory() && !external.mkdirs())) {
            Log.e(TAG, "External app files unavailable; retaining legacy root");
            return legacy;
        }
        try {
            Properties previous = readMarker(new File(external, MARKER));
            Properties current = new Properties();
            migrateDirectory(legacy, external, "", 0, previous, current);
            writeMarker(new File(external, MARKER), current);
            readyRoot = external;
            Log.i(TAG, "Game data root: " + external.getAbsolutePath());
            return external;
        } catch (IOException | SecurityException error) {
            Log.e(TAG, "Migration incomplete; retaining legacy root", error);
            return legacy;
        }
    }

    public static File file(Activity activity, String name) throws IOException {
        if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
            throw new IOException("Invalid game data filename");
        }
        return new File(root(activity), name);
    }

    public static FileInputStream openInput(Activity activity, String name) throws IOException {
        return new FileInputStream(file(activity, name));
    }

    public static FileOutputStream openOutput(Activity activity, String name) throws IOException {
        return new FileOutputStream(file(activity, name));
    }

    public static String getLanguage(Activity activity) {
        File valueFile = new File(root(activity), "language.txt");
        if (valueFile.isFile()) {
            try (FileInputStream input = new FileInputStream(valueFile)) {
                long length = valueFile.length();
                if (length < 1 || length > 64) throw new IOException("Invalid language marker");
                byte[] bytes = new byte[(int) length];
                int offset = 0;
                while (offset < bytes.length) {
                    int count = input.read(bytes, offset, bytes.length - offset);
                    if (count < 0) throw new IOException("Truncated language marker");
                    offset += count;
                }
                return new String(bytes, StandardCharsets.UTF_8);
            } catch (IOException error) {
                Log.e(TAG, "Cannot read external language marker", error);
            }
        }
        SharedPreferences previous = activity.getPreferences(0);
        String value = previous.getString("language", "null");
        setLanguage(activity, value);
        return value;
    }

    public static void setLanguage(Activity activity, String value) {
        File target = new File(root(activity), "language.txt");
        File temporary = new File(target.getParentFile(), "language.txt.tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                output.write(value.getBytes(StandardCharsets.UTF_8));
                output.getFD().sync();
            }
            if (!temporary.renameTo(target)) throw new IOException("Cannot commit language marker");
        } catch (IOException error) {
            Log.e(TAG, "Cannot write language marker", error);
        } finally {
            if (temporary.exists()) temporary.delete();
        }
    }

    private static void migrateDirectory(File source, File target, String relative, int depth,
                                         Properties previous, Properties current) throws IOException {
        if (depth > 16) throw new IOException("Legacy data nesting too deep");
        File[] children = source.listFiles();
        if (children == null) throw new IOException("Cannot list legacy data: " + source);
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("Cannot create external data directory: " + target);
        }
        for (File child : children) {
            // Old builds stored this disposable driver cache among player files.
            // Recompiling it must not redirect saves to the legacy data root.
            if (relative.isEmpty() && (child.getName().equals("bl2-program-cache.bin")
                    || child.getName().startsWith("bl2-program-cache.bin.tmp."))) continue;
            File destination = new File(target, child.getName());
            String childRelative = relative + child.getName();
            if (!source.getCanonicalFile().equals(child.getCanonicalFile().getParentFile())) {
                throw new IOException("Legacy data link refused: " + child);
            }
            if (child.isDirectory()) {
                if (destination.exists() && !destination.isDirectory()) {
                    throw new IOException("External data type conflict: " + destination);
                }
                migrateDirectory(child, destination, childRelative + "/", depth + 1,
                                 previous, current);
            } else if (child.isFile()) {
                migrateFile(child, destination, childRelative, previous, current);
            } else {
                throw new IOException("Unsupported legacy data entry: " + child);
            }
        }
    }

    private static void migrateFile(File source, File destination, String relative,
                                    Properties previous, Properties current) throws IOException {
        byte[] sourceHash = sha256(source);
        String sourceHex = hex(sourceHash);
        if (destination.exists()) {
            if (!destination.isFile()) throw new IOException("External data type conflict: " + destination);
            if (!Arrays.equals(sourceHash, sha256(destination))) {
                // These settings are already owned by the external Java store;
                // a legacy fallback must not replace the user's current choice.
                boolean currentSetting = relative.equals("language.txt")
                        || relative.equals("touch-controls.properties");
                if (!currentSetting && !sourceHex.equals(previous.getProperty(relative))) {
                    // Changing roots here hides current characters and their selected-slot profile.
                    Log.w(TAG, "Keeping external data; divergent legacy copy retained: " + source);
                }
            }
            current.setProperty(relative, sourceHex);
            return;
        }
        File temporary = new File(destination.getParentFile(), destination.getName() + ".migration-tmp");
        if (temporary.exists() && !temporary.delete()) {
            throw new IOException("Cannot replace incomplete migration file: " + temporary);
        }
        try {
            try (FileInputStream input = new FileInputStream(source);
                 FileOutputStream output = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                output.getFD().sync();
            }
            if (!Arrays.equals(sourceHash, sha256(temporary))) {
                throw new IOException("Migration hash mismatch: " + source);
            }
            if (!temporary.renameTo(destination)) {
                throw new IOException("Atomic migration rename failed: " + destination);
            }
            if (!Arrays.equals(sourceHash, sha256(destination))) {
                throw new IOException("Migrated file verification failed: " + destination);
            }
            Log.i(TAG, "Migrated legacy data: " + source.getAbsolutePath());
            current.setProperty(relative, sourceHex);
        } finally {
            if (temporary.exists()) temporary.delete();
        }
    }

    private static Properties readMarker(File marker) throws IOException {
        Properties result = new Properties();
        if (marker.exists()) {
            try (FileInputStream input = new FileInputStream(marker)) {
                result.load(input);
            }
        }
        return result;
    }

    private static void writeMarker(File marker, Properties data) throws IOException {
        File temporary = new File(marker.getParentFile(), marker.getName() + ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                data.store(output, "Legacy game file migration");
                output.getFD().sync();
            }
            if (!temporary.renameTo(marker)) {
                throw new IOException("Cannot commit migration marker");
            }
        } finally {
            if (temporary.exists()) temporary.delete();
        }
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] result = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            result[i * 2] = digits[(bytes[i] >>> 4) & 15];
            result[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return new String(result);
    }

    private static byte[] sha256(File file) throws IOException {
        final MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new IOException("SHA-256 unavailable", error);
        }
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return digest.digest();
    }
}
