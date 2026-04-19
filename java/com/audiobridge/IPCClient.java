package com.audiobridge;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class IPCClient {
    private static final String TAG = "AudioBridge-IPC";
    private static final String SOCKET_NAME = "/data/local/tmp/audio_bridge.sock";

    private static IPCClient sInstance;
    
    private LocalSocket mSocket;
    private PrintWriter mOut;
    private BufferedReader mIn;
    
    private boolean mRunning = false;
    private ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private Handler mMainHandler = new Handler(Looper.getMainLooper());
    
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
        mExecutor.execute(() -> {
            while (mRunning) {
                try {
                    connectAndListen();
                } catch (Exception e) {
                    Log.w(TAG, "IPC Connection dropped, retrying in 5s: " + e.getMessage());
                    try { Thread.sleep(5000); } catch (InterruptedException ie) {}
                }
            }
        });
    }

    private void connectAndListen() throws IOException, JSONException {
        Log.i(TAG, "Attempting to connect to daemon UDS...");
        
        mSocket = new LocalSocket();
        mSocket.connect(new LocalSocketAddress(SOCKET_NAME, LocalSocketAddress.Namespace.FILESYSTEM));
        
        mOut = new PrintWriter(new OutputStreamWriter(mSocket.getOutputStream()), true);
        mIn = new BufferedReader(new InputStreamReader(mSocket.getInputStream()));
        
        // Handshake
        mOut.println("HELO_JAVA");
        Log.i(TAG, "Connected to native daemon via UDS");
        
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
        
        disconnect();
        throw new IOException("Socket closed by remote");
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
                } else if ("send_sms".equals(cmd)) {
                    th.sendSMS(json.getString("number"), json.getString("message"));
                } else {
                    Log.w(TAG, "Unknown command from daemon: " + cmd);
                }
            } catch (Exception e) {
                Log.e(TAG, "Error handling command", e);
            }
        });
    }

    public void sendEvent(JSONObject json) {
        mExecutor.execute(() -> {
            if (mOut != null && mSocket != null && mSocket.isConnected()) {
                mOut.println(json.toString());
            } else {
                Log.w(TAG, "Cannot send event, IPC not connected: " + json.toString());
            }
        });
    }

    public void disconnect() {
        mRunning = false;
        try {
            if (mSocket != null) {
                mSocket.close();
                mSocket = null;
            }
        } catch (IOException e) {
            // ignore
        }
    }
}
