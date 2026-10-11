package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;
import java.util.List;

/**
 * Restarts the game. The engine initializes once per process, so this activity
 * runs in its own short-lived process (":restart"): it ends the game process,
 * waits until Android has released it and starts the launcher again. The game
 * itself quits the same way, by killing its process.
 */
public final class GameRestart extends Activity {
    private static final String TAG = "BL2Restart";
    private static final String EXTRA_PID = "com.tt2kgames.Borderlands2.RESTART_PID";
    private static final long POLL_MS = 50, RELEASE_TIMEOUT_MS = 5000;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private int gamePid;
    private long started;

    /** Game process, UI thread. The new activity ends the calling process. */
    public static void restart(Activity game) {
        Log.i(TAG, "Restart requested by pid " + Process.myPid());
        game.startActivity(new Intent(game, GameRestart.class).putExtra(EXTRA_PID, Process.myPid()));
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("ru".equals(GameStorage.appLanguage(this)) ? "Перезапуск игры…" : "Restarting the game…");
        text.setTextColor(Color.LTGRAY);
        text.setBackgroundColor(Color.BLACK);
        text.setGravity(Gravity.CENTER);
        setContentView(text);
        // A recreated instance repeats this: its predecessor's callbacks ended
        // in onDestroy, and a finished one is never recreated.
        gamePid = getIntent().getIntExtra(EXTRA_PID, 0);
        started = SystemClock.uptimeMillis();
        if (gamePid > 0 && gamePid != Process.myPid()) Process.killProcess(gamePid);
        handler.post(this::launchWhenReleased);
    }

    private void launchWhenReleased() {
        // A launch while Android still tracks the old process could be routed to it.
        if (running(gamePid) && SystemClock.uptimeMillis() - started < RELEASE_TIMEOUT_MS) {
            handler.postDelayed(this::launchWhenReleased, POLL_MS);
            return;
        }
        // This process never loads the bridge, so final builds cannot drop the
        // message: it appears only on request (setprop log.tag.BL2Restart D).
        if (Log.isLoggable(TAG, Log.DEBUG)) {
            Log.d(TAG, "Game process " + gamePid + " released after "
                    + (SystemClock.uptimeMillis() - started) + " ms; starting the launcher");
        }
        startActivity(new Intent(this, Downloader.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
        // Nothing else runs in this process: end it rather than leave it cached.
        new Handler(Looper.getMainLooper()).postDelayed(() -> Process.killProcess(Process.myPid()), 1000);
    }

    private boolean running(int pid) {
        if (pid <= 0) return false;
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> processes =
                manager == null ? null : manager.getRunningAppProcesses();
        if (processes != null) {
            for (ActivityManager.RunningAppProcessInfo process : processes) {
                if (process.pid == pid) return true;
            }
        }
        return false;
    }

    @Override protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
