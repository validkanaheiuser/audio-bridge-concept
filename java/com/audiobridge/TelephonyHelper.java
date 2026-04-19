package com.audiobridge;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.app.PendingIntent;
import android.provider.Telephony;
import android.telecom.TelecomManager;
import android.telephony.PhoneStateListener;
import android.telephony.SmsManager;
import android.telephony.SmsMessage;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.Manifest;
import android.os.Bundle;

import java.util.ArrayList;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

/**
 * TelephonyHelper - Provides call control and SMS functionality
 * Used by Audio Bridge native code via JNI
 */
public class TelephonyHelper {
    private static final String TAG = "AudioBridge-Telephony";
    private static TelephonyHelper sInstance;

    private Context mContext;
    private TelephonyManager mTelephonyManager;
    private TelecomManager mTelecomManager;
    private SmsManager mSmsManager;
    private Handler mMainHandler;

    private final Map<String, SMSInfo> mPendingSMS = new ConcurrentHashMap<>();
    private final Map<String, CallInfo> mActiveCalls = new ConcurrentHashMap<>();
    private String mCurrentActiveCall = null;

    // Native methods
    private native void nativeOnCallStateChanged(int state, String number);
    private native void nativeOnCallWaiting(String incomingNumber, String currentNumber);
    private native void nativeOnSMSSent(String messageId, int resultCode);
    private native void nativeOnSMSDelivered(String messageId);
    private native void nativeOnSMSReceived(String sender, String message, long timestamp);

    static {
        System.loadLibrary("audiobridge");
    }

    // Singleton
    public static synchronized TelephonyHelper getInstance(Context context) {
        if (sInstance == null) {
            sInstance = new TelephonyHelper(context.getApplicationContext());
        }
        return sInstance;
    }

    public static TelephonyHelper getInstance() {
        return sInstance;
    }

    private TelephonyHelper(Context context) {
        mContext = context;
        mMainHandler = new Handler(Looper.getMainLooper());
        mTelephonyManager = (TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE);
        mTelecomManager = (TelecomManager) context.getSystemService(Context.TELECOM_SERVICE);
        mSmsManager = SmsManager.getDefault();

        registerCallListener();
        registerSMSReceiver();

        android.util.Log.i(TAG, "TelephonyHelper initialized");
    }

    private void registerCallListener() {
        PhoneStateListener listener = new PhoneStateListener() {
            @Override
            public void onCallStateChanged(int state, String incomingNumber) {
                handleCallStateChange(state, incomingNumber);
            }
        };
        mTelephonyManager.listen(listener, PhoneStateListener.LISTEN_CALL_STATE);
    }

    private void handleCallStateChange(int state, String number) {
        mMainHandler.post(() -> {
            boolean isCallWaiting = !mActiveCalls.isEmpty() &&
                                   state == TelephonyManager.CALL_STATE_RINGING;

            if (isCallWaiting) {
                String currentNumber = mCurrentActiveCall != null ?
                    mActiveCalls.get(mCurrentActiveCall).number : "";
                nativeOnCallWaiting(number, currentNumber);
            }

            updateCallTracking(state, number);
            nativeOnCallStateChanged(state, number);
        });
    }

    private void updateCallTracking(int state, String number) {
        if (state == TelephonyManager.CALL_STATE_IDLE) {
            mActiveCalls.clear();
            mCurrentActiveCall = null;
        } else if (state == TelephonyManager.CALL_STATE_RINGING) {
            String callId = "call_" + System.currentTimeMillis();
            mActiveCalls.put(callId, new CallInfo(number, state, true));
        } else if (state == TelephonyManager.CALL_STATE_OFFHOOK) {
            if (mCurrentActiveCall == null) {
                String callId = "call_" + System.currentTimeMillis();
                mCurrentActiveCall = callId;
                mActiveCalls.put(callId, new CallInfo(number, state, false));
            }
        }
    }

    private void registerSMSReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(Telephony.Sms.Intents.SMS_RECEIVED_ACTION);
        mContext.registerReceiver(new SMSBroadcastReceiver(), filter);
    }

    // Public API - Call Control

    public void placeCall(String number) {
        if (mContext.checkSelfPermission(Manifest.permission.CALL_PHONE)
                != PackageManager.PERMISSION_GRANTED) {
            android.util.Log.w(TAG, "CALL_PHONE permission not granted");
            return;
        }

        Intent intent = new Intent(Intent.ACTION_CALL);
        intent.setData(Uri.parse("tel:" + number));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        mContext.startActivity(intent);

        android.util.Log.i(TAG, "Placing call to: " + number);
    }

    public void endCall() {
        if (mTelecomManager != null) {
            if (mContext.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS)
                    == PackageManager.PERMISSION_GRANTED) {
                mTelecomManager.endCall();
                android.util.Log.i(TAG, "Call ended");
            }
        }
    }

    public void answerCall() {
        if (mTelecomManager != null) {
            if (mContext.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS)
                    == PackageManager.PERMISSION_GRANTED) {
                mTelecomManager.acceptRingingCall();
                android.util.Log.i(TAG, "Call answered");
            }
        }
    }

    // Public API - SMS

    public String sendSMS(String phoneNumber, String message) {
        if (mContext.checkSelfPermission(Manifest.permission.SEND_SMS)
                != PackageManager.PERMISSION_GRANTED) {
            android.util.Log.w(TAG, "SEND_SMS permission not granted");
            return null;
        }

        String messageId = UUID.randomUUID().toString();

        SMSInfo info = new SMSInfo();
        info.id = messageId;
        info.number = phoneNumber;
        info.message = message;
        info.timestamp = System.currentTimeMillis();

        ArrayList<String> parts = mSmsManager.divideMessage(message);
        info.parts = parts;
        info.totalParts = parts.size();

        mPendingSMS.put(messageId, info);

        ArrayList<PendingIntent> sentIntents = new ArrayList<>();
        ArrayList<PendingIntent> deliveryIntents = new ArrayList<>();

        for (int i = 0; i < parts.size(); i++) {
            Intent sentIntent = new Intent("SMS_SENT_" + messageId);
            sentIntent.putExtra("message_id", messageId);
            sentIntent.putExtra("part", i);
            PendingIntent sentPI = PendingIntent.getBroadcast(
                mContext, i, sentIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            sentIntents.add(sentPI);

            Intent deliveryIntent = new Intent("SMS_DELIVERED_" + messageId);
            deliveryIntent.putExtra("message_id", messageId);
            PendingIntent deliveryPI = PendingIntent.getBroadcast(
                mContext, i + 1000, deliveryIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            deliveryIntents.add(deliveryPI);
        }

        try {
            mSmsManager.sendMultipartTextMessage(phoneNumber, null, parts,
                                                  sentIntents, deliveryIntents);
            registerSMSReceivers(messageId);
            android.util.Log.i(TAG, "SMS sent to " + phoneNumber + " (ID: " + messageId + ")");
        } catch (Exception e) {
            android.util.Log.e(TAG, "Failed to send SMS", e);
            nativeOnSMSSent(messageId, 1);
            mPendingSMS.remove(messageId);
            return null;
        }

        return messageId;
    }

    private void registerSMSReceivers(String messageId) {
        IntentFilter sentFilter = new IntentFilter("SMS_SENT_" + messageId);
        IntentFilter deliveredFilter = new IntentFilter("SMS_DELIVERED_" + messageId);

        BroadcastReceiver sentReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String id = intent.getStringExtra("message_id");
                SMSInfo info = mPendingSMS.get(id);

                if (info != null) {
                    int resultCode = getResultCode();

                    if (resultCode == Activity.RESULT_OK) {
                        info.sentParts++;
                        if (info.sentParts == info.totalParts) {
                            nativeOnSMSSent(id, -1);
                        }
                    } else {
                        nativeOnSMSSent(id, resultCode);
                        mPendingSMS.remove(id);
                    }
                }

                try {
                    mContext.unregisterReceiver(this);
                } catch (IllegalArgumentException e) {}
            }
        };

        BroadcastReceiver deliveredReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String id = intent.getStringExtra("message_id");
                SMSInfo info = mPendingSMS.get(id);

                if (info != null) {
                    if (getResultCode() == Activity.RESULT_OK) {
                        nativeOnSMSDelivered(id);
                        mPendingSMS.remove(id);
                    }
                }

                try {
                    mContext.unregisterReceiver(this);
                } catch (IllegalArgumentException e) {}
            }
        };

        mContext.registerReceiver(sentReceiver, sentFilter,
                                  Context.RECEIVER_EXPORTED);
        mContext.registerReceiver(deliveredReceiver, deliveredFilter,
                                  Context.RECEIVER_EXPORTED);
    }

    // Inner classes

    private static class CallInfo {
        String number;
        int state;
        long startTime;
        boolean isIncoming;

        CallInfo(String num, int st, boolean incoming) {
            number = num;
            state = st;
            startTime = System.currentTimeMillis();
            isIncoming = incoming;
        }
    }

    private static class SMSInfo {
        String id;
        String number;
        String message;
        ArrayList<String> parts;
        int totalParts;
        int sentParts;
        long timestamp;
    }

    private class SMSBroadcastReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Telephony.Sms.Intents.SMS_RECEIVED_ACTION.equals(intent.getAction())) {
                Bundle bundle = intent.getExtras();
                if (bundle != null) {
                    Object[] pdus = (Object[]) bundle.get("pdus");
                    if (pdus != null) {
                        for (Object pdu : pdus) {
                            SmsMessage sms = SmsMessage.createFromPdu((byte[]) pdu);
                            String sender = sms.getDisplayOriginatingAddress();
                            String message = sms.getDisplayMessageBody();
                            long timestamp = sms.getTimestampMillis();

                            nativeOnSMSReceived(sender, message, timestamp);
                        }
                    }
                }
            }
        }
    }
}
