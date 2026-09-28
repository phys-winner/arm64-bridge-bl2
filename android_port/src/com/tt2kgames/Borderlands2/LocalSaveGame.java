package com.tt2kgames.Borderlands2;

import android.util.AtomicFile;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;

/** Real local snapshot IO; the removed cloud synchronizer has no pending work. */
public final class LocalSaveGame {
    private File directory;
    public LocalSaveGame(String tag) { }
    public synchronized boolean initialize(Object ignoredClient, File value) {
        try {
            if (value == null || (!value.isDirectory() && !value.mkdirs())) return false;
            directory = value;
            return true;
        } catch (SecurityException denied) { return false; }
    }
    private File file(String name) throws IOException {
        if (directory == null || name == null || name.isEmpty()) throw new IOException("No snapshot path");
        File root = directory.getCanonicalFile();
        File result = new File(root, name).getCanonicalFile();
        if (!root.equals(result.getParentFile()) || result.isDirectory())
            throw new IOException("Snapshot name is not a file in its directory");
        return result;
    }
    public synchronized String[] enumerate() {
        if (directory == null) return null;
        File[] files;
        try { files = directory.listFiles(); } catch (SecurityException denied) { return null; }
        if (files == null) return null;
        ArrayList<String> names = new ArrayList<>();
        for (File value : files) {
            String name = value.getName();
            if (name.endsWith(".bak") && value.isFile()) {
                String original = name.substring(0, name.length() - 4);
                if (!new File(directory, original).exists()) name = original; // AtomicFile.openRead restores this backup.
            }
            if (value.isFile() && !name.endsWith(".needsSync") && !name.endsWith(".synced")
                    && !name.endsWith(".bak") && !name.endsWith(".new")) names.add(name);
        }
        return names.toArray(new String[0]);
    }
    public synchronized byte[] read(String name, long[] timestamp) {
        try {
            File value = file(name);
            try (InputStream in = new AtomicFile(value).openRead();
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                if (timestamp != null && timestamp.length != 0) timestamp[0] = value.lastModified();
                return out.toByteArray();
            }
        } catch (IOException | SecurityException failure) { return null; }
    }
    public synchronized boolean write(String name, byte[] data) {
        if (data == null) return false;
        AtomicFile atomic;
        try { atomic = new AtomicFile(file(name)); } catch (IOException | SecurityException invalid) { return false; }
        FileOutputStream out = null;
        try {
            out = atomic.startWrite(); out.write(data); atomic.finishWrite(out);
            return Arrays.equals(data, read(name, null));
        } catch (IOException | SecurityException failure) {
            try { atomic.failWrite(out); } catch (SecurityException ignored) { }
            return false;
        }
    }
    public synchronized boolean delete(String name) {
        try {
            File value = file(name);
            if (!value.delete()) return false;
            new AtomicFile(value).delete();
            return true;
        } catch (IOException | SecurityException failure) { return false; }
    }
    public void synchronize() { }
    public void synchronizeWait(boolean ignoredCancel) { }
}
