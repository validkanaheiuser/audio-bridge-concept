package com.audiobridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.content.pm.ServiceInfo;

import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * AudioBridgeService - Foreground service for persistent operation
 */
public class AudioBridgeService extends Service {
    private static final String TAG = "AudioBridge-Service";
    private static final String CHANNEL_ID = "audio_bridge_channel";
    private static final int NOTIFICATION_ID = 1001;
    private static final String DIAG = "/data/local/tmp/audio_bridge_java.log";
    private static final SimpleDateFormat TS =
        new SimpleDateFormat("HH:mm:ss", Locale.US);

    private static void diag(String msg) {
        android.util.Log.i(TAG, msg);
        try (FileWriter w = new FileWriter(DIAG, true)) {
            w.write(TS.format(new Date()) + " [service] " + msg + "\n");
        } catch (IOException ignored) {}
    }

    @Override
    public void onCreate() {
        super.onCreate();
        diag("onCreate entered (sdk=" + Build.VERSION.SDK_INT + ")");
        createNotificationChannel();
        // API 34+ enforces that the type passed to startForeground matches
        // the permissions the app actually holds. We pick DATA_SYNC because
        // its only requirement is FOREGROUND_SERVICE_DATA_SYNC (a normal
        // permission). PHONE_CALL needs an active TelecomManager call,
        // MICROPHONE needs RECORD_AUDIO — neither of which applies to us
        // here; call control goes through intents and audio goes through
        // Zygisk in other processes, not this service.
        boolean fg = false;
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, buildNotification(),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, buildNotification(), 0);
            } else {
                startForeground(NOTIFICATION_ID, buildNotification());
            }
            fg = true;
            diag("startForeground ok");
        } catch (Throwable e) {
            // On API 34+ a mismatch between manifest type and passed type
            // throws MissingForegroundServiceTypeException; a missing runtime
            // permission throws SecurityException. We also can't just skip
            // startForeground because startForegroundService() gave us a 5s
            // deadline — if we miss it, the whole service is killed. Try the
            // untyped overloads as a fallback.
            diag("startForeground(typed) FAILED: " + e.getClass().getSimpleName()
                 + ": " + e.getMessage());
            try {
                startForeground(NOTIFICATION_ID, buildNotification());
                fg = true;
                diag("startForeground(untyped) ok");
            } catch (Throwable e2) {
                diag("startForeground(untyped) FAILED too: "
                     + e2.getClass().getSimpleName() + ": " + e2.getMessage());
            }
        }
        if (!fg) {
            diag("FGS start failed — service will be killed within 5s");
        }
        TelephonyHelper.getInstance(this);
        IPCClient.getInstance();
        diag("onCreate finished (fg=" + fg + ")");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        android.util.Log.i(TAG, "AudioBridgeService destroyed");
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Audio Bridge", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Audio Bridge background service");
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }
        return builder
            .setContentTitle("Audio Bridge")
            .setContentText("Running in background")
            .setSmallIcon(android.R.drawable.ic_menu_call)
            .setOngoing(true)
            .build();
    }
}
