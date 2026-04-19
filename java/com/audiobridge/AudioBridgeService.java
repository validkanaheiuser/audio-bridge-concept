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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int type = 0;
            if (Build.VERSION.SDK_INT >= 34) {
                // API 34+ requires explicit types from manifest
                type = ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL | 
                       ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            }
            startForeground(NOTIFICATION_ID, buildNotification(), type);
        } else {
            startForeground(NOTIFICATION_ID, buildNotification());
        }
        TelephonyHelper.getInstance(this);
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
