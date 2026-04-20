package com.audiobridge;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;

import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Headless (Theme.NoDisplay) activity whose only job is to kick the
 * foreground service into existence. Started by service.sh via `am start`.
 *
 * Why not a BroadcastReceiver? Since Android 12 (API 31), startForegroundService
 * from a custom broadcast is blocked with ForegroundServiceStartNotAllowedException
 * unless the broadcast is a system broadcast (BOOT_COMPLETED, etc.). Activities
 * don't have this restriction — an activity is considered "foreground" the
 * moment it starts, even with Theme.NoDisplay, so startForegroundService()
 * from its onCreate is permitted.
 */
public class LauncherActivity extends Activity {
    private static final String TAG = "AudioBridge-Launcher";
    private static final String DIAG = "/data/local/tmp/audio_bridge_java.log";
    private static final SimpleDateFormat TS =
        new SimpleDateFormat("HH:mm:ss", Locale.US);

    private static void diag(String msg) {
        android.util.Log.i(TAG, msg);
        try (FileWriter w = new FileWriter(DIAG, true)) {
            w.write(TS.format(new Date()) + " [launcher] " + msg + "\n");
        } catch (IOException ignored) {}
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        diag("onCreate — starting foreground service");
        Intent svc = new Intent(this, AudioBridgeService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(svc);
            } else {
                startService(svc);
            }
            diag("startForegroundService returned normally");
        } catch (Throwable t) {
            diag("startForegroundService FAILED: "
                 + t.getClass().getSimpleName() + ": " + t.getMessage());
        }
        finish();
    }
}
