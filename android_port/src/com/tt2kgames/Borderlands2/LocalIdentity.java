package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InvalidClassException;
import java.io.ObjectInputStream;
import java.io.ObjectStreamClass;

/** Read the existing local identity; never claim Google connectivity or rewrite it. */
public final class LocalIdentity {
    public interface Listener {
        void onSignInCompleted(Object client, Object invitation, String id, String name,
                               int age, String[] friends, String token);
    }
    private final Listener listener;
    private String id = "0";
    private String name = "Local Player";
    private int age;

    public LocalIdentity(String tag, Activity activity, Listener target, String ignoredAppId) {
        listener = target;
        try (FileInputStream file = GameStorage.openInput(activity, "signin.dat")) {
            if (file.getChannel().size() > 65536) throw new IOException("Legacy identity exceeds 64 KiB");
            try (ObjectInputStream in = new LegacyInput(file)) {
            String storedId = (String) in.readObject();
            String storedName = (String) in.readObject();
            int storedAge = in.readInt();
            String[] storedFriends = (String[]) in.readObject();
            String storedToken = (String) in.readObject();
            if (storedFriends != null && storedFriends.length > 4096) throw new IOException("Legacy friends too large");
            // Preserve the exact legacy types, but do not expose remote friends/tokens as offline services.
            if (storedId != null && !storedId.isEmpty() && !storedId.equals(".") && !storedId.equals("..")
                    && storedId.indexOf('/') < 0 && storedId.indexOf('\\') < 0 && storedId.indexOf(0) < 0) {
                id = storedId;
                if (storedName != null) name = storedName;
                age = storedAge;
            }
            }
        } catch (Exception absentOrInvalid) { }
    }
    private static final class LegacyInput extends ObjectInputStream {
        LegacyInput(FileInputStream input) throws IOException { super(input); }
        @Override protected Class<?> resolveClass(ObjectStreamClass type) throws IOException, ClassNotFoundException {
            if (!type.getName().equals("[Ljava.lang.String;") && !type.getName().equals("java.lang.String"))
                throw new InvalidClassException("Unexpected legacy identity type", type.getName());
            return super.resolveClass(type);
        }
        @Override protected Class<?> resolveProxyClass(String[] types) throws IOException {
            throw new InvalidClassException("Legacy identity contains a proxy");
        }
    }
    public void signIn(boolean userInitiated, boolean ignoredConnect) {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override public void run() {
                if (listener != null) listener.onSignInCompleted(null, null, id, name, age, new String[0], null);
            }
        });
    }
    public void signOut() { }
    public void onStart() { }
    public void onStop() { }
    public boolean onActivityResult(int request, int result, Intent data) { return false; }
    public boolean isShowingSignIn() { return false; }
    public void showProfile(String id) { }
}
