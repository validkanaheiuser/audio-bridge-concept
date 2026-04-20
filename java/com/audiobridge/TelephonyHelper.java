package com.audiobridge;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.media.AudioManager;
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

import org.json.JSONException;
import org.json.JSONObject;

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
    private AudioManager mAudioManager;
    private SmsManager mSmsManager;
    private Handler mMainHandler;

    private final Map<String, SMSInfo> mPendingSMS = new ConcurrentHashMap<>();
    private final Map<String, CallInfo> mActiveCalls = new ConcurrentHashMap<>();
    private String mCurrentActiveCall = null;

    // ── Call state machine ─────────────────────────────────────────────────
    // Android's CALL_STATE_* doesn't distinguish incoming vs outgoing. We
    // maintain the direction ourselves based on who initiated the state
    // transition (placeCall from dashboard → outgoing; RINGING without a
    // prior placeCall → incoming).
    private enum Dir { UNKNOWN, INCOMING, OUTGOING }
    private Dir    mDir          = Dir.UNKNOWN;
    private String mActiveNumber = "";
    private long   mStartedAt    = 0;      // ms since epoch
    private boolean mMuted       = false;

    // Native methods removed in favor of IPCClient

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
        mAudioManager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        mSmsManager = SmsManager.getDefault();

        registerCallListener();
        registerSMSReceiver();

        // Emit an initial IDLE state so the dashboard doesn't sit on stale data.
        emitCallState("IDLE", "unknown", "");

        android.util.Log.i(TAG, "TelephonyHelper initialized");
    }

    // ── Event emitters ─────────────────────────────────────────────────────
    private void emitCallState(String state, String dir, String number) {
        try {
            JSONObject e = new JSONObject();
            e.put("type", "call");
            e.put("state", state);
            e.put("direction", dir);
            e.put("number", number != null ? number : "");
            e.put("started_at", mStartedAt);
            e.put("duration_ms", mStartedAt > 0 ? (System.currentTimeMillis() - mStartedAt) : 0);
            e.put("muted", mMuted);
            IPCClient.getInstance().sendEvent(e);
        } catch (JSONException je) {
            android.util.Log.w(TAG, "emitCallState: " + je.getMessage());
        }
    }

    private void emitError(String op, String code, String msg) {
        try {
            JSONObject e = new JSONObject();
            e.put("type", "error");
            e.put("op", op);
            e.put("code", code);
            e.put("message", msg != null ? msg : "");
            IPCClient.getInstance().sendEvent(e);
            android.util.Log.w(TAG, "emitError " + op + " " + code + ": " + msg);
        } catch (JSONException je) {
            android.util.Log.w(TAG, "emitError(json): " + je.getMessage());
        }
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
            updateCallTracking(state, number);

            // Map Android CALL_STATE_* → our richer state + direction.
            String stateName;
            switch (state) {
                case TelephonyManager.CALL_STATE_RINGING:
                    stateName     = "RINGING";
                    // Only overwrite direction if we weren't mid-dial.
                    if (mDir != Dir.OUTGOING) mDir = Dir.INCOMING;
                    if (!number.isEmpty()) mActiveNumber = number;
                    if (mStartedAt == 0) mStartedAt = System.currentTimeMillis();
                    break;
                case TelephonyManager.CALL_STATE_OFFHOOK:
                    stateName = "ACTIVE";
                    if (mDir == Dir.UNKNOWN) mDir = Dir.OUTGOING;  // edge case
                    if (mStartedAt == 0) mStartedAt = System.currentTimeMillis();
                    break;
                case TelephonyManager.CALL_STATE_IDLE:
                default:
                    stateName = "IDLE";
                    break;
            }

            String num = mActiveNumber.isEmpty() ? (number != null ? number : "") : mActiveNumber;
            emitCallState(stateName, dirString(mDir), num);

            if ("IDLE".equals(stateName)) {
                resetCallState();
            }
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

    /** Returns true on dispatch success; emits error event + returns false on failure. */
    public boolean placeCall(String number) {
        if (number == null || number.trim().isEmpty()) {
            emitError("dial", "INVALID_NUMBER", "number is empty");
            return false;
        }
        if (mContext.checkSelfPermission(Manifest.permission.CALL_PHONE)
                != PackageManager.PERMISSION_GRANTED) {
            emitError("dial", "PERMISSION_DENIED", "CALL_PHONE not granted");
            return false;
        }

        String clean = number.replaceAll("[\\s()\\-]", "");
        Uri uri = Uri.fromParts("tel", clean, null);

        // Preemptively mark outgoing so the very next onCallStateChanged
        // (→ OFFHOOK on dispatch) is labelled correctly.
        mDir          = Dir.OUTGOING;
        mActiveNumber = clean;
        mStartedAt    = System.currentTimeMillis();
        emitCallState("DIALING", "outgoing", clean);

        if (mTelecomManager != null) {
            try {
                Bundle extras = new Bundle();
                if (Build.VERSION.SDK_INT >= 34) {
                    extras.putInt(TelecomManager.EXTRA_CALL_SOURCE,
                        TelecomManager.CALL_SOURCE_UNSPECIFIED);
                }
                mTelecomManager.placeCall(uri, extras);
                android.util.Log.i(TAG, "placeCall(Telecom) → " + clean);
                return true;
            } catch (SecurityException se) {
                emitError("dial", "SECURITY", se.getMessage());
                resetCallState();
                return false;
            } catch (Exception e) {
                android.util.Log.w(TAG, "TelecomManager.placeCall failed, trying intent", e);
                // fall through
            }
        }

        Intent intent = new Intent(Intent.ACTION_CALL, uri);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            mContext.startActivity(intent);
            android.util.Log.i(TAG, "placeCall(intent) → " + clean);
            return true;
        } catch (Exception e) {
            emitError("dial", "INTENT_FAILED", e.getMessage());
            resetCallState();
            return false;
        }
    }

    public void setMute(boolean on) {
        try {
            if (mAudioManager != null) {
                mAudioManager.setMicrophoneMute(on);
                mMuted = on;
                emitCallState(
                    mActiveNumber.isEmpty() ? "IDLE" : "ACTIVE",
                    dirString(mDir),
                    mActiveNumber);
                android.util.Log.i(TAG, "setMicrophoneMute(" + on + ")");
            } else {
                emitError("mute", "NO_AUDIO_MGR", "AudioManager unavailable");
            }
        } catch (Exception e) {
            emitError("mute", "EXCEPTION", e.getMessage());
        }
    }

    private void resetCallState() {
        mDir = Dir.UNKNOWN;
        mActiveNumber = "";
        mStartedAt = 0;
        mMuted = false;
    }

    private static String dirString(Dir d) {
        switch (d) {
            case INCOMING: return "incoming";
            case OUTGOING: return "outgoing";
            default:       return "unknown";
        }
    }

    public boolean endCall() {
        if (mContext.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS)
                != PackageManager.PERMISSION_GRANTED) {
            emitError("hangup", "PERMISSION_DENIED", "ANSWER_PHONE_CALLS not granted");
            return false;
        }
        if (mTelecomManager == null) {
            emitError("hangup", "NO_TELECOM", "TelecomManager unavailable");
            return false;
        }
        try {
            mTelecomManager.endCall();
            android.util.Log.i(TAG, "endCall()");
            return true;
        } catch (Exception e) {
            emitError("hangup", "EXCEPTION", e.getMessage());
            return false;
        }
    }

    public boolean answerCall() {
        if (mContext.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS)
                != PackageManager.PERMISSION_GRANTED) {
            emitError("answer", "PERMISSION_DENIED", "ANSWER_PHONE_CALLS not granted");
            return false;
        }
        if (mTelecomManager == null) {
            emitError("answer", "NO_TELECOM", "TelecomManager unavailable");
            return false;
        }
        try {
            mTelecomManager.acceptRingingCall();
            android.util.Log.i(TAG, "answerCall()");
            return true;
        } catch (Exception e) {
            emitError("answer", "EXCEPTION", e.getMessage());
            return false;
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
            try {
                JSONObject event = new JSONObject();
                event.put("event", "sms_sent");
                event.put("message_id", messageId);
                event.put("result_code", 1);
                IPCClient.getInstance().sendEvent(event);
            } catch (JSONException je) { je.printStackTrace(); }
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
                            try {
                                JSONObject event = new JSONObject();
                                event.put("event", "sms_sent");
                                event.put("message_id", id);
                                event.put("result_code", -1);
                                IPCClient.getInstance().sendEvent(event);
                            } catch (JSONException je) { je.printStackTrace(); }
                        }
                    } else {
                        try {
                            JSONObject event = new JSONObject();
                            event.put("event", "sms_sent");
                            event.put("message_id", id);
                            event.put("result_code", resultCode);
                            IPCClient.getInstance().sendEvent(event);
                        } catch (JSONException je) { je.printStackTrace(); }
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
                        try {
                            JSONObject event = new JSONObject();
                            event.put("event", "sms_delivered");
                            event.put("message_id", id);
                            IPCClient.getInstance().sendEvent(event);
                        } catch (JSONException je) { je.printStackTrace(); }
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

                            try {
                                JSONObject event = new JSONObject();
                                event.put("event", "sms_received");
                                event.put("sender", sender);
                                event.put("message", message);
                                event.put("timestamp", timestamp);
                                IPCClient.getInstance().sendEvent(event);
                            } catch (JSONException je) { je.printStackTrace(); }
                        }
                    }
                }
            }
        }
    }
}
