#!/system/bin/sh
# Audio Bridge - Uninstall Script

echo "Uninstalling Audio Bridge..."

# Stop daemon
sh /data/local/tmp/stop.sh 2>/dev/null

# Remove files
rm -f /data/local/tmp/audio-bridge
rm -f /data/local/tmp/audio_bridge.conf
rm -f /data/local/tmp/audio_bridge.log
rm -f /data/local/tmp/audio_bridge.pid
rm -f /data/local/tmp/audio_bridge.sock
rm -f /data/local/tmp/audio_bridge_id

# Remove Zygisk module
rm -rf /data/adb/modules/audio_bridge

echo "Uninstall complete. Reboot to fully remove Zygisk hooks."
