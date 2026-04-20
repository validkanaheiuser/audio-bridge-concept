package com.audiobridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.content.pm.ServiceInfo;

/**
 * AudioBridgeService - Foreground service for persistent operation
 */
public class AudioBridgeService extends Service {
    private static final String TAG = "AudioBridge-Service";
    private static final String CHANNEL_ID = "audio_bridge_channel";
    private static final int NOTIFICATION_ID = 1001;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        // API 34+ enforces that the type passed to startForeground matches
        // the permissions the app actually holds. We pick DATA_SYNC because
        // its only requirement is FOREGROUND_SERVICE_DATA_SYNC (a normal
        // permission). PHONE_CALL needs an active TelecomManager call,
        // MICROPHONE needs RECORD_AUDIO — neither of which applies to us
        // here; call control goes through intents and audio goes through
        // Zygisk in other processes, not this service.
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, buildNotification(),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, buildNotification(), 0);
            } else {
                startForeground(NOTIFICATION_ID, buildNotification());
            }
        } catch (Exception e) {
            // If the FGS start fails for any reason, keep running as a
            // plain (background) service rather than crashing — the IPC
            // client and telephony hooks still work; we just lose the
            // OS-level priority.
            android.util.Log.w(TAG, "startForeground failed, continuing as bg service", e);
        }
        TelephonyHelper.getInstance(this);
        IPCClient.getInstance();
        android.util.Log.i(TAG, "AudioBridgeService created");
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
