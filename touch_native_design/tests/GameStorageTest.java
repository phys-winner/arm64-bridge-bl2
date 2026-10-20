package com.tt2kgames.Borderlands2;

import android.app.Activity;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** Run with storage_stubs/android/util/Log.java before android.jar on the classpath. */
public final class GameStorageTest {
    private static final class TestActivity extends Activity {
        File internal, external;
        @Override public File getFilesDir() { return internal; }
        @Override public File getExternalFilesDir(String type) { return external; }
    }

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
    }

    private static void write(File root, String name, String content) throws Exception {
        File file = new File(root, name);
        Files.createDirectories(file.toPath().getParent());
        Files.write(file.toPath(), content.getBytes(StandardCharsets.UTF_8));
    }

    private static void checkContent(File root, String name, String content) throws Exception {
        check(Arrays.equals(Files.readAllBytes(new File(root, name).toPath()),
                content.getBytes(StandardCharsets.UTF_8)), name + " changed");
    }

    private static File coldStart(TestActivity activity) throws Exception {
        Field cached = GameStorage.class.getDeclaredField("readyRoot");
        cached.setAccessible(true);
        cached.set(null, null);
        // Windows renameTo cannot replace a marker; Android uses POSIX rename semantics.
        if (File.separatorChar == '\\') Files.deleteIfExists(new File(activity.external,
                ".legacy-migration.properties").toPath());
        return GameStorage.root(activity);
    }

    public static void main(String[] args) throws Exception {
        File temporary = Files.createTempDirectory("bl2-save-migration-").toFile();
        // Android's SDK Activity constructor is a throwing stub on the host JVM.
        Class<?> unsafeType = Class.forName("sun.misc.Unsafe");
        Field singleton = unsafeType.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        TestActivity activity = (TestActivity) unsafeType.getMethod("allocateInstance", Class.class)
                .invoke(singleton.get(null), TestActivity.class);
        activity.internal = new File(temporary, "internal");
        activity.external = new File(temporary, "external");
        write(activity.internal, "savegame/0/Player1", "legacy profile");
        write(activity.internal, "savegame/0/Save0001.sav", "legacy character");
        write(activity.external, "savegame/0/Player1", "current profile");
        write(activity.external, "savegame/0/Save0002.sav", "current character");

        check(coldStart(activity).equals(activity.external),
                "A divergent legacy profile must not hide external characters by changing the root");
        checkContent(activity.external, "savegame/0/Player1", "current profile");
        checkContent(activity.external, "savegame/0/Save0001.sav", "legacy character");
        checkContent(activity.external, "savegame/0/Save0002.sav", "current character");
        checkContent(activity.internal, "savegame/0/Player1", "legacy profile");

        write(activity.internal, "savegame/0/Player1", "profile written by a fallback build");
        write(activity.external, "savegame/0/Save0002.sav", "current character after playing");
        check(coldStart(activity).equals(activity.external),
                "A later internal write must not redirect the next launch to a different profile");
        checkContent(activity.external, "savegame/0/Player1", "current profile");
        checkContent(activity.external, "savegame/0/Save0002.sav", "current character after playing");
        checkContent(activity.internal, "savegame/0/Player1", "profile written by a fallback build");
        check(coldStart(activity).equals(activity.external), "Repeated cold start retains external root");
        System.out.println("PASS: legacy conflicts preserve both copies and the external save root across cold starts");
    }
}

