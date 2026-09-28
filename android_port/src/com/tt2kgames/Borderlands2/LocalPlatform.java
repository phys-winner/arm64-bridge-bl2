package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Build;
import android.provider.Settings;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/** Guest JNI compatibility without a vendor SDK or license service. */
public final class LocalPlatform {
    private static final LocalPlatform INSTANCE = new LocalPlatform();
    private Activity activity;
    private BroadcastReceiver receiver;
    private String uniqueID;
    private volatile long callbackStatus = -4; // Original CS_NOT_CONNECTED.
    private String googleResponse;
    private String googleSignature;

    public static LocalPlatform getInstance() { return INSTANCE; }
    public void setActivity(Activity value) { cleanup(); activity = value; init(); }
    private void init() {
        if (activity == null || receiver != null) return;
        receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (Intent.ACTION_SCREEN_OFF.equals(intent.getAction())) native_nvsiOnScreenOff();
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else activity.registerReceiver(receiver, filter);
    }
    private void cleanup() {
        if (activity != null && receiver != null) {
            try { activity.unregisterReceiver(receiver); } catch (IllegalArgumentException ignored) { }
        }
        receiver = null;
    }
    private byte[] readSnippet(String name) {
        if (activity == null) return null;
        int id = activity.getResources().getIdentifier(name, "raw", activity.getPackageName());
        if (id == 0) return null;
        try (InputStream in = activity.getResources().openRawResource(id);
             ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            return out.size() == 0 ? null : out.toByteArray();
        } catch (Exception failure) { return null; }
    }
    private byte[] getSnippetCode() { return readSnippet("game_snippet_code"); }
    private byte[] getSnippetData() { return readSnippet("game_snippet_data"); }
    private String getFilesPath() { return GameStorage.root(activity).getAbsolutePath(); }
    private String getCachePath() { return activity.getCacheDir().getAbsolutePath(); }
    private String getPackageName() { return activity.getPackageName(); }
    private String getGPUClassFeature() { return "unknown"; }
    private String getUniqueID() {
        if (uniqueID == null) {
            String id = Settings.Secure.getString(activity.getContentResolver(), Settings.Secure.ANDROID_ID);
            uniqueID = id == null ? UUID.randomUUID().toString()
                    : UUID.nameUUIDFromBytes(id.getBytes(StandardCharsets.UTF_8)).toString();
        }
        return uniqueID;
    }
    private boolean isOnline() {
        if (activity == null) return false;
        try {
            ConnectivityManager manager = (ConnectivityManager) activity.getSystemService(Context.CONNECTIVITY_SERVICE);
            NetworkInfo info = manager == null ? null : manager.getActiveNetworkInfo();
            return info != null && info.isConnected();
        } catch (SecurityException denied) { return false; }
    }
    private boolean isDevelopmentMode() { return false; }
    private boolean isFakeGoogleMode() { return false; }
    private void checkLicense(long nonce) {
        googleResponse = null; googleSignature = null; callbackStatus = -4;
    }
    private void httpPost(String url, String type, byte[] data) { }
    private byte[] httpGetResult() { return null; }
    private void showProgressDialog() { }
    private void dismissProgressDialog(boolean result, String message) { }
    private void showNetworkFailedToast() { }
    private void setUIFlags(int flags) { }
    native void native_nvsiCancelVerification();
    native void native_nvsiForceRecheck(int flags);
    native void native_nvsiOnScreenOff();
}
