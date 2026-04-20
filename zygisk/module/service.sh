#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/local/tmp/audio_bridge_service.log

echo "$(date) Audio Bridge service.sh started" >> $LOG

# Auto-grant permissions (suppress errors if app not yet installed)
pm grant com.audiobridge android.permission.CALL_PHONE 2>/dev/null
pm grant com.audiobridge android.permission.ANSWER_PHONE_CALLS 2>/dev/null
pm grant com.audiobridge android.permission.READ_PHONE_STATE 2>/dev/null
pm grant com.audiobridge android.permission.SEND_SMS 2>/dev/null
pm grant com.audiobridge android.permission.RECEIVE_SMS 2>/dev/null
pm grant com.audiobridge android.permission.READ_SMS 2>/dev/null
appops set com.audiobridge SYSTEM_ALERT_WINDOW allow 2>/dev/null

# Apply SELinux rules dynamically (Magisk, KernelSU, supolicy)
for TOOL in magiskpolicy supolicy /data/adb/ksud; do
    if command -v $TOOL >/dev/null 2>&1 || [ -f "$TOOL" ]; then
        $TOOL --live "allow radio su unix_stream_socket { connectto read write }" 2>/dev/null
        $TOOL --live "allow radio magisk unix_stream_socket { connectto read write }" 2>/dev/null
        $TOOL --live "allow platform_app su unix_stream_socket { connectto read write }" 2>/dev/null
        $TOOL --live "allow priv_app su unix_stream_socket { connectto read write }" 2>/dev/null
        $TOOL --live "allow system_app su unix_stream_socket { connectto read write }" 2>/dev/null
        echo "$(date) SELinux rules applied via $TOOL" >> $LOG
        break
    fi
done

# Start daemon if not running
if ! pidof audio-bridge >/dev/null 2>&1; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    if [ -f /system/bin/audio-bridge ]; then
        /system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    else
        chmod 755 $MODDIR/system/bin/audio-bridge 2>/dev/null
        $MODDIR/system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    fi
    sleep 3
    if pidof audio-bridge >/dev/null 2>&1; then
        echo "$(date) Daemon started, PID: $(pidof audio-bridge)" >> $LOG
    else
        echo "$(date) WARNING: Daemon failed to start" >> $LOG
    fi
else
    echo "$(date) Daemon already running, PID: $(pidof audio-bridge)" >> $LOG
fi

# Start AudioBridge Java service
am startservice --user 0 -n com.audiobridge/.AudioBridgeService >> $LOG 2>&1
echo "$(date) AudioBridgeService launch requested" >> $LOG
