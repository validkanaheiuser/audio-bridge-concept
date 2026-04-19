#!/system/bin/sh
# Audio Bridge - service.sh
# Runs during late_start service mode

MODDIR=${0%/*}
LOG=/data/local/tmp/audio_bridge_service.log

echo "$(date) Audio Bridge service.sh started" >> $LOG

# Ensure daemon is running
if [ ! -f /data/local/tmp/audio_bridge.pid ] || ! kill -0 $(cat /data/local/tmp/audio_bridge.pid) 2>/dev/null; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    $MODDIR/system/bin/audio-bridge --daemon &
fi
