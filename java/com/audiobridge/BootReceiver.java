package com.audiobridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

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

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action)
                && !"com.audiobridge.START".equals(action)) {
            return;
        }
        Intent svc = new Intent(context, AudioBridgeService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(svc);
            } else {
                context.startService(svc);
            }
            android.util.Log.i(TAG, "Service started (action=" + action + ")");
        } catch (Exception e) {
            android.util.Log.e(TAG, "startForegroundService failed", e);
        }
    }
}
