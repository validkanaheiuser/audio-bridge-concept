#!/system/bin/sh
# Audio Bridge Installation Script

echo "Audio Bridge v3.0 Installation"
echo "================================"

# Check root
if [ "$(id -u)" != "0" ]; then
    echo "Error: Root required"
    exit 1
fi

# Create directories
mkdir -p /data/local/tmp
mkdir -p /data/adb/modules/audio_bridge

# Copy binary
cp /sdcard/audio-bridge /data/local/tmp/
chmod 755 /data/local/tmp/audio-bridge

# Copy Zygisk module
cp -r /sdcard/zygisk_module/* /data/adb/modules/audio_bridge/
chmod -R 755 /data/adb/modules/audio_bridge

# Create config
echo "YOUR_SERVER_IP" > /data/local/tmp/audio_bridge.conf
chmod 600 /data/local/tmp/audio_bridge.conf

# Start service
/data/local/tmp/audio-bridge --daemon &

echo "Installation complete!"
echo "Edit /data/local/tmp/audio_bridge.conf with your server IP"
echo "Reboot to activate Zygisk module"
