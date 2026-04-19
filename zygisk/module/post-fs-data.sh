#!/system/bin/sh
MODDIR=${0%/*}

# Run in background to avoid blocking boot
(
    if [ ! -S /data/local/tmp/audio_bridge.sock ]; then
        $MODDIR/system/bin/audio-bridge --daemon
    fi
) &
