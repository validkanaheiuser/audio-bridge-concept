#!/system/bin/sh
# Audio Bridge - Start Script

echo "Starting Audio Bridge..."

if [ -f /data/local/tmp/audio_bridge.pid ]; then
    PID=$(cat /data/local/tmp/audio_bridge.pid)
    if kill -0 $PID 2>/dev/null; then
        echo "Already running (PID: $PID)"
        exit 0
    fi
fi

/data/local/tmp/audio-bridge --daemon &
sleep 1

if [ -f /data/local/tmp/audio_bridge.pid ]; then
    echo "Started (PID: $(cat /data/local/tmp/audio_bridge.pid))"
else
    echo "Failed to start"
    exit 1
fi
