package com.tt2kgames.Borderlands2;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import java.io.File;

/** Discovers local game data; the native bridge verifies the selected payload. */
public final class Downloader extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        continueCreate();
    }

    private void continueCreate() {
        // This runs before the bridge loads, so final builds cannot drop these
        // messages: they appear only on request (setprop log.tag.BL2 D).
        final boolean verbose = Log.isLoggable("BL2", Log.DEBUG);
        if (hasInstalledObbSplits()) {
            if (verbose) Log.d("BL2", "Installed OBB cache splits found; starting native verification");
        } else if (hasExternalObbPair()) {
            if (verbose) Log.d("BL2", "Local OBB pair found; starting native verification");
        } else {
            new AlertDialog.Builder(this)
                    .setTitle(0x7f0700bd)
                    .setMessage(0x7f0700be)
                    .setPositiveButton(0x7f0700bf, (dialog, which) -> continueCreate())
                    .setNegativeButton(0x7f07003d, (dialog, which) -> finish())
                    .setOnCancelListener(dialog -> finish())
                    .show();
            return;
        }
        if (verbose) Log.d("BL2", "Launching Game");
        Intent game = new Intent().setClassName(this, "com.tt2kgames.Borderlands2.UE3JavaApp");
        game.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        startActivity(game);
        finish();
    }

    private boolean hasInstalledObbSplits() {
        String[] names = getApplicationInfo().splitNames;
        if (names == null) return false;
        boolean main = false, patch = false;
        for (String name : names) {
            main |= "obb_main".equals(name);
            patch |= "obb_patch".equals(name);
        }
        return main && patch;
    }

    private boolean hasExternalObbPair() {
        try {
            File directory = getObbDir();
            if (directory == null) return false;
            File main = new File(directory, "main.31.com.tt2kgames.Borderlands2.obb");
            File patch = new File(directory, "patch.31.com.tt2kgames.Borderlands2.obb");
            return main.isFile() && main.length() == 0x7dfd5000L
                    && patch.isFile() && patch.length() == 0x9dcc4000L;
        } catch (SecurityException error) {
            Log.e("BL2", "Cannot inspect local OBB pair", error);
            return false;
        }
    }
}
