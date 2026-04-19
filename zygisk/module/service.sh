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

# Start daemon if not running (background, will not block boot)
if [ ! -f /data/local/tmp/audio_bridge.pid ] || ! kill -0 $(cat /data/local/tmp/audio_bridge.pid 2>/dev/null) 2>/dev/null; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    $MODDIR/system/bin/audio-bridge --daemon &
fi
