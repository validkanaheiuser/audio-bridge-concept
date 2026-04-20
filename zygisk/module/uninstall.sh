#!/system/bin/sh
# Runs when the user removes the module via Magisk Manager / KernelSU Manager.
# Module is still mounted at $MODDIR when this executes, so we can rely on
# its contents for cleanup paths.
LOG=/data/local/tmp/audio_bridge_uninstall.log
echo "$(date) uninstall.sh started" >> $LOG

# Stop the running daemon
if pidof audio-bridge >/dev/null 2>&1; then
    PID=$(pidof audio-bridge)
    kill $PID 2>/dev/null
    sleep 1
    kill -9 $PID 2>/dev/null
    echo "$(date) daemon killed (was PID $PID)" >> $LOG
fi

# Stop the foreground service; remove the app if it was installed to /data
# via pm install. The priv-app overlay copy clears itself when Magisk/KernelSU
# unmounts the module, but a pm-install copy is persistent.
am stopservice --user 0 -n com.audiobridge/.AudioBridgeService 2>&1 >> $LOG
APK_PATH=$(pm path com.audiobridge 2>/dev/null)
if echo "$APK_PATH" | grep -q "/data/app/"; then
    pm uninstall --user 0 com.audiobridge >> $LOG 2>&1
    echo "$(date) pm uninstall com.audiobridge (was $APK_PATH)" >> $LOG
else
    echo "$(date) priv-app copy at $APK_PATH — will be removed by overlay unmount" >> $LOG
fi

# Remove diagnostic + runtime files we created in /data/local/tmp
rm -f /data/local/tmp/audio_bridge.log \
      /data/local/tmp/audio_bridge_java.log \
      /data/local/tmp/audio_bridge_service.log \
      /data/local/tmp/audio_bridge.sock \
      /data/local/tmp/audio_bridge.pid \
      /data/local/tmp/audio_bridge.conf \
      /data/local/tmp/audio_bridge_id

echo "$(date) uninstall.sh complete" >> $LOG
