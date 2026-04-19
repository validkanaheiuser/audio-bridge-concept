#!/system/bin/sh
# Audio Bridge - service.sh
# Runs during late_start service mode (safe, non-blocking)

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

# Start daemon if not running
# Use /system/bin/ path (mounted overlay) for correct SELinux context
# Fallback to $MODDIR path if overlay not available
if [ ! -f /data/local/tmp/audio_bridge.pid ] || ! kill -0 $(cat /data/local/tmp/audio_bridge.pid 2>/dev/null) 2>/dev/null; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    if [ -f /system/bin/audio-bridge ]; then
        /system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    else
        echo "$(date) /system/bin/audio-bridge not found, trying MODDIR" >> $LOG
        chmod 755 $MODDIR/system/bin/audio-bridge 2>/dev/null
        $MODDIR/system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    fi
    sleep 1
    if [ -f /data/local/tmp/audio_bridge.pid ]; then
        echo "$(date) Daemon started, PID: $(cat /data/local/tmp/audio_bridge.pid)" >> $LOG
    else
        echo "$(date) WARNING: Daemon failed to start" >> $LOG
    fi
fi
