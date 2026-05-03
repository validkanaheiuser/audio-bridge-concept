package com.audiobridge;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.util.Log;

import java.io.IOException;
import java.io.OutputStream;

/**
 * Cellular-call audio capture using {@link MediaRecorder.AudioSource#VOICE_CALL}.
 *
 * <p>Why this exists. The Zygisk module hooks {@code AudioTrack::write} /
 * {@code AudioRecord::read} in user-side processes — that path captures
 * VoIP/app-call audio but never sees cellular voice, because Android routes
 * cellular audio through the audio HAL straight to the modem DSP and bypasses
 * AudioFlinger entirely. This class fills that gap by opening a system
 * AudioRecord on the {@code VOICE_CALL} source, which Android forwards from
 * the HAL during {@code MODE_IN_CALL}.
 *
 * <p>The capture is gated by {@code CAPTURE_AUDIO_OUTPUT}, a signature-only
 * permission. The module's overlay at
 * {@code /system/etc/permissions/privapp-permissions-com.audiobridge.xml}
 * grants it, but only when AudioBridge.apk is installed under
 * {@code /system/priv-app/}. If that's not the case we silently degrade to
 * "no cellular capture" and let the operator know via call-state events.
 *
 * <p>The capture lifecycle is driven by {@link TelephonyHelper}: it calls
 * {@link #start} when the call goes ACTIVE/OFFHOOK and {@link #stop} when it
 * returns to IDLE.
 */
public class VoiceCallCapture {
    private static final String TAG = "AudioBridge-VoiceCap";
    private static final String SOCKET_NAME = "audio_bridge";
    private static final int SAMPLE_RATE   = 48000;     // matches FRAME_SAMPLES in audio_bridge.h
    private static final int FRAME_SAMPLES = 960;       // 20ms @ 48kHz mono
    private static final int FRAME_BYTES   = FRAME_SAMPLES * 2;
    private static final int CHANNELS      = AudioFormat.CHANNEL_IN_MONO;
    private static final int ENCODING      = AudioFormat.ENCODING_PCM_16BIT;

    private final Context mContext;
    private Thread mPump;
    private volatile boolean mRunning;
    private AudioRecord mRecord;
    private LocalSocket mSocket;

    public VoiceCallCapture(Context ctx) {
        mContext = ctx.getApplicationContext();
    }

    /** True iff the helper APK currently has CAPTURE_AUDIO_OUTPUT granted. */
    public boolean isAvailable() {
        // CAPTURE_AUDIO_OUTPUT is signature-only; checkSelfPermission returns
        // the right answer even though it isn't a runtime permission, because
        // the framework treats it as install-time once granted.
        return mContext.checkSelfPermission("android.permission.CAPTURE_AUDIO_OUTPUT")
                == PackageManager.PERMISSION_GRANTED;
    }

    /** Idempotent. Returns true if capture is now running, false on any failure. */
    public synchronized boolean start() {
        if (mRunning) return true;
        if (!isAvailable()) {
            Log.w(TAG, "CAPTURE_AUDIO_OUTPUT not granted — install APK as priv-app to enable");
            return false;
        }
        try {
            mSocket = new LocalSocket();
            mSocket.connect(new LocalSocketAddress(
                    SOCKET_NAME, LocalSocketAddress.Namespace.ABSTRACT));
            // Daemon's accept loop dispatches by command keyword on first recv().
            mSocket.getOutputStream().write("HELO_AUDIO_CELL".getBytes());
            mSocket.getOutputStream().flush();
        } catch (IOException ioe) {
            Log.w(TAG, "Daemon socket connect failed: " + ioe.getMessage());
            closeSocketQuiet();
            return false;
        }

        // AudioRecord buffer: at least 4 frames so we tolerate scheduler jitter.
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNELS, ENCODING);
        int bufBytes = Math.max(minBuf, FRAME_BYTES * 8);
        try {
            mRecord = new AudioRecord(
                    MediaRecorder.AudioSource.VOICE_CALL,
                    SAMPLE_RATE, CHANNELS, ENCODING, bufBytes);
        } catch (Throwable t) {
            // Some OneUI builds throw SecurityException even when the
            // permission appears granted (vendor audio policy override).
            // Fall back to VOICE_DOWNLINK first, then VOICE_COMMUNICATION
            // as a last-resort that still gives the far-end audio.
            mRecord = tryFallbackSources(bufBytes);
        }
        if (mRecord == null || mRecord.getState() != AudioRecord.STATE_INITIALIZED) {
            Log.w(TAG, "AudioRecord init failed");
            releaseRecordQuiet();
            closeSocketQuiet();
            return false;
        }
        try {
            mRecord.startRecording();
        } catch (IllegalStateException ise) {
            Log.w(TAG, "AudioRecord.startRecording: " + ise.getMessage());
            releaseRecordQuiet();
            closeSocketQuiet();
            return false;
        }

        mRunning = true;
        mPump = new Thread(this::pumpLoop, "VoiceCallCapture-Pump");
        mPump.setDaemon(true);
        mPump.start();
        Log.i(TAG, "Cellular capture started (source=" + sourceName() + ")");
        return true;
    }

    public synchronized void stop() {
        if (!mRunning) return;
        mRunning = false;
        if (mPump != null) {
            try { mPump.join(500); } catch (InterruptedException ignored) {}
            mPump = null;
        }
        if (mRecord != null) {
            try { mRecord.stop(); } catch (IllegalStateException ignored) {}
            releaseRecordQuiet();
        }
        closeSocketQuiet();
        Log.i(TAG, "Cellular capture stopped");
    }

    private AudioRecord tryFallbackSources(int bufBytes) {
        int[] fallbacks = new int[] {
            MediaRecorder.AudioSource.VOICE_DOWNLINK,
            MediaRecorder.AudioSource.VOICE_UPLINK,
            MediaRecorder.AudioSource.VOICE_COMMUNICATION,
        };
        for (int src : fallbacks) {
            try {
                AudioRecord r = new AudioRecord(src, SAMPLE_RATE, CHANNELS, ENCODING, bufBytes);
                if (r.getState() == AudioRecord.STATE_INITIALIZED) {
                    Log.i(TAG, "Falling back to source " + src);
                    return r;
                }
                r.release();
            } catch (Throwable ignored) {}
        }
        return null;
    }

    private String sourceName() {
        if (mRecord == null) return "?";
        int s = mRecord.getAudioSource();
        switch (s) {
            case MediaRecorder.AudioSource.VOICE_CALL:          return "VOICE_CALL";
            case MediaRecorder.AudioSource.VOICE_DOWNLINK:      return "VOICE_DOWNLINK";
            case MediaRecorder.AudioSource.VOICE_UPLINK:        return "VOICE_UPLINK";
            case MediaRecorder.AudioSource.VOICE_COMMUNICATION: return "VOICE_COMMUNICATION";
            default: return "src=" + s;
        }
    }

    private void pumpLoop() {
        short[] frame = new short[FRAME_SAMPLES];
        byte[]  bytes = new byte[FRAME_BYTES];
        OutputStream out;
        try {
            out = mSocket.getOutputStream();
        } catch (IOException ioe) {
            Log.w(TAG, "getOutputStream failed: " + ioe.getMessage());
            return;
        }
        long sentFrames = 0;
        while (mRunning) {
            int got = 0;
            // AudioRecord.read may return short reads; loop until we have a full frame.
            while (got < FRAME_SAMPLES && mRunning) {
                int n = mRecord.read(frame, got, FRAME_SAMPLES - got);
                if (n <= 0) {
                    // ERROR_INVALID_OPERATION (-3), DEAD_OBJECT (-6), etc.
                    // — break out, the outer loop will exit on mRunning.
                    Log.w(TAG, "AudioRecord.read returned " + n);
                    return;
                }
                got += n;
            }
            if (!mRunning) break;

            // int16 native-endian → little-endian byte stream (matches the
            // daemon's int16_t buf[FRAME_SAMPLES] memcpy on the other side
            // since both sides are arm64 LE).
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                short v = frame[i];
                bytes[i * 2]     = (byte)(v & 0xFF);
                bytes[i * 2 + 1] = (byte)((v >> 8) & 0xFF);
            }
            try {
                out.write(bytes, 0, FRAME_BYTES);
            } catch (IOException ioe) {
                Log.w(TAG, "Daemon write failed: " + ioe.getMessage());
                return;
            }
            sentFrames++;
            if ((sentFrames % 250) == 0) {  // every ~5s
                Log.d(TAG, "cell audio frames sent: " + sentFrames);
            }
        }
    }

    private void releaseRecordQuiet() {
        try { if (mRecord != null) mRecord.release(); } catch (Throwable ignored) {}
        mRecord = null;
    }
    private void closeSocketQuiet() {
        try { if (mSocket != null) mSocket.close(); } catch (IOException ignored) {}
        mSocket = null;
    }
}
