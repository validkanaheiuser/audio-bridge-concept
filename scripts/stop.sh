#!/system/bin/sh
# Audio Bridge - Stop Script

echo "Stopping Audio Bridge..."

if [ -f /data/local/tmp/audio_bridge.pid ]; then
    PID=$(cat /data/local/tmp/audio_bridge.pid)
    if kill -0 $PID 2>/dev/null; then
        kill $PID
        sleep 1
        if kill -0 $PID 2>/dev/null; then
            kill -9 $PID
        fi
        echo "Stopped (PID: $PID)"
    else
        echo "Not running"
    fi
    rm -f /data/local/tmp/audio_bridge.pid
else
    # Try to find by process name
    pkill -f audio-bridge 2>/dev/null
    echo "Stopped"
fi

rm -f /data/local/tmp/audio_bridge.sock
