package com.audiobridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Starts AudioBridgeService in response to:
 *   - android.intent.action.BOOT_COMPLETED (system fires on boot)
 *   - com.audiobridge.START              (shell broadcast from service.sh —
 *                                          lets us start the FGS after
 *                                          systemless-module install, where
 *                                          BOOT_COMPLETED already fired)
 *
 * Why a broadcast receiver and not `am start-foreground-service`? On Android
 * 12+ the shell process is treated as background and cannot launch an FGS
 * directly (`app is in background uid null`). A broadcast is delivered inside
 * the app's own UID, from which startForegroundService() is permitted.
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "AudioBridge-Boot";
    private static final String DIAG = "/data/local/tmp/audio_bridge_java.log";
    private static final SimpleDateFormat TS =
        new SimpleDateFormat("HH:mm:ss", Locale.US);

    private static void diag(String msg) {
        android.util.Log.i(TAG, msg);
        try (FileWriter w = new FileWriter(DIAG, true)) {
            w.write(TS.format(new Date()) + " [boot] " + msg + "\n");
        } catch (IOException ignored) {}
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        diag("onReceive action=" + action
             + " pid=" + android.os.Process.myPid()
             + " uid=" + android.os.Process.myUid());
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action)
                && !"com.audiobridge.START".equals(action)) {
            diag("ignored (unknown action)");
            return;
        }
        Intent svc = new Intent(context, AudioBridgeService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(svc);
            } else {
                context.startService(svc);
            }
            diag("startForegroundService returned normally");
        } catch (Throwable e) {
            diag("startForegroundService FAILED: " + e.getClass().getSimpleName()
                 + ": " + e.getMessage());
        }
    }
}
