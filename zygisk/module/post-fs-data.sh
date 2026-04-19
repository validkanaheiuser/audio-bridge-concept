#!/system/bin/sh
# Audio Bridge - post-fs-data.sh
# Wait for audio bridge daemon and start if not running
sleep 5
if [ ! -S /data/local/tmp/audio_bridge.sock ]; then
    /data/local/tmp/audio-bridge --daemon &
fi
