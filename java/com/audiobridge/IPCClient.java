package com.audiobridge;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class IPCClient {
    private static final String TAG = "AudioBridge-IPC";
    private static final String SOCKET_NAME = "audio_bridge";
    // Diagnostics log: writable by the app, readable by root. Lets you see
    // what the Java side is doing without needing adb logcat.
    private static final String DIAG_LOG = "/data/local/tmp/audio_bridge_java.log";
    private static final SimpleDateFormat TS =
        new SimpleDateFormat("HH:mm:ss", Locale.US);

    private static IPCClient sInstance;

    private LocalSocket mSocket;
    private PrintWriter mOut;
    private BufferedReader mIn;

    private boolean mRunning = false;
    private ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private Handler mMainHandler = new Handler(Looper.getMainLooper());
    private android.content.Context mContext;

    public static synchronized IPCClient init(android.content.Context ctx) {
        if (sInstance == null) {
            sInstance = new IPCClient();
        }
        sInstance.mContext = ctx.getApplicationContext();
        return sInstance;
    }

    private void setStatus(String line) {
        AudioBridgeService.updateStatus(mContext, line);
    }

    private static void diag(String msg) {
        Log.i(TAG, msg);
        try (FileWriter w = new FileWriter(DIAG_LOG, true)) {
            w.write(TS.format(new Date()) + " " + msg + "\n");
        } catch (IOException ignored) {
            // /data/local/tmp is usually priv_app-writable via shell_data_file
            // grants in our sepolicy.rule; if not, we still have logcat.
        }
    }
    
    public static synchronized IPCClient getInstance() {
        if (sInstance == null) {
            sInstance = new IPCClient();
        }
        return sInstance;
    }

    private IPCClient() {
        startConnectionThread();
    }

    private void startConnectionThread() {
        mRunning = true;
        diag("startConnectionThread() — pid=" + android.os.Process.myPid()
             + " uid=" + android.os.Process.myUid());
        setStatus("Connecting to daemon…");
        mExecutor.execute(() -> {
            int attempt = 0;
            while (mRunning) {
                attempt++;
                try {
                    connectAndListen();
                } catch (Exception e) {
                    diag("connect attempt " + attempt + " failed: "
                         + e.getClass().getSimpleName() + ": " + e.getMessage());
                    setStatus("Daemon unreachable · retry " + attempt);
                    try { Thread.sleep(5000); } catch (InterruptedException ie) {}
                }
            }
        });
    }

    private void connectAndListen() throws IOException, JSONException {
        diag("Connecting to abstract socket @" + SOCKET_NAME);

        mSocket = new LocalSocket();
        mSocket.connect(new LocalSocketAddress(SOCKET_NAME, LocalSocketAddress.Namespace.ABSTRACT));

        mOut = new PrintWriter(new OutputStreamWriter(mSocket.getOutputStream()), true);
        mIn = new BufferedReader(new InputStreamReader(mSocket.getInputStream()));

        // Handshake
        mOut.println("HELO_JAVA");
        diag("Connected — HELO_JAVA sent");
        setStatus("Daemon connected · telephony ready");

        try {
            String line;
            while (mRunning && (line = mIn.readLine()) != null) {
                if (line.trim().isEmpty()) continue;

                try {
                    JSONObject json = new JSONObject(line);
                    handleCommand(json);
                } catch (JSONException e) {
                    Log.e(TAG, "Invalid JSON from daemon: " + line);
                }
            }
        } finally {
            // Per-connection cleanup ONLY. Must not flip mRunning, otherwise
            // the outer reconnect loop in startConnectionThread() exits and
            // never comes back when the daemon restarts. The previous code
            // called disconnect() here, which set mRunning=false and turned
            // a dropped socket into a permanent shutdown — symptom: WebUI
            // "Save & Restart" worked once, but every subsequent restart
            // left the helper APK orphaned with no IPC.
            closeSocketQuiet();
        }
        // readLine() returned null = peer closed the socket. Throw so the
        // outer loop sleeps and retries.
        throw new IOException("Socket closed by remote");
    }

    /** Close the current socket + streams without touching mRunning. */
    private void closeSocketQuiet() {
        try { if (mOut != null)    mOut.close(); }    catch (Exception ignored) {}
        try { if (mIn != null)     mIn.close(); }     catch (IOException ignored) {}
        try { if (mSocket != null) mSocket.close(); } catch (IOException ignored) {}
        mOut = null;
        mIn = null;
        mSocket = null;
    }

    private void handleCommand(JSONObject json) {
        mMainHandler.post(() -> {
            try {
                String cmd = json.getString("command");
                TelephonyHelper th = TelephonyHelper.getInstance(null);
                if (th == null) return;
                
                if ("place_call".equals(cmd)) {
                    th.placeCall(json.getString("number"));
                } else if ("end_call".equals(cmd)) {
                    th.endCall();
                } else if ("answer_call".equals(cmd)) {
                    th.answerCall();
                } else if ("mute".equals(cmd)) {
                    th.setMute(json.optBoolean("on", true));
                } else if ("send_sms".equals(cmd)) {
                    th.sendSMS(json.getString("number"), json.getString("message"));
                } else if ("dtmf".equals(cmd)) {
                    th.sendDtmf(json.optString("digits", ""));
                } else if ("speakerphone".equals(cmd)) {
                    th.setSpeakerphone(json.optBoolean("on", true));
                } else {
                    Log.w(TAG, "Unknown command from daemon: " + cmd);
                }
            } catch (Exception e) {
                Log.e(TAG, "Error handling command", e);
            }
        });
    }

    public void sendEvent(JSONObject json) {
        // Snapshot mOut so a concurrent reconnect can't NPE us between the
        // null-check and the write. PrintWriter.println swallows IOException
        // and only sets an internal error flag; the read loop will see EOF
        // and trigger reconnect, so we don't need to reconnect from here.
        // Note: writes happen on the caller's thread rather than mExecutor,
        // because mExecutor is occupied by the long-lived read loop and
        // would queue writes until the next reconnect — which we don't want.
        PrintWriter out = mOut;
        if (out == null) {
            Log.w(TAG, "Cannot send event, IPC not connected: " + json.toString());
            return;
        }
        try {
            out.println(json.toString());
            if (out.checkError()) {
                Log.w(TAG, "PrintWriter error flag set; reconnect will be triggered by read loop");
            }
        } catch (Exception e) {
            Log.w(TAG, "sendEvent failed: " + e.getMessage());
        }
    }

    /** Explicit shutdown — stops the reconnect loop and closes the socket. */
    public void disconnect() {
        mRunning = false;
        closeSocketQuiet();
    }
}
