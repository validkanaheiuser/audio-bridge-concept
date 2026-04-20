#!/system/bin/sh
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
pm grant com.audiobridge android.permission.POST_NOTIFICATIONS 2>/dev/null
appops set com.audiobridge SYSTEM_ALERT_WINDOW allow 2>/dev/null

# Apply SELinux rules at runtime. sepolicy.rule is read by Magisk/KernelSU on
# boot, but a live push covers reinstalls and testing. The rule set covers
# every (app_domain -> daemon_domain) pair we might encounter.
APP_DOMAINS="priv_app system_app platform_app radio"
DAEMON_DOMAINS="ksu magisk su init"
if command -v magiskpolicy >/dev/null 2>&1; then
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        magiskpolicy --live "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via magiskpolicy" >> $LOG
elif [ -f /data/adb/ksud ]; then
    # KernelSU: apply-sepolicy accepts one rule per -- invocation.
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        /data/adb/ksud apply-sepolicy "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via ksud" >> $LOG
elif command -v supolicy >/dev/null 2>&1; then
    for APP in $APP_DOMAINS; do for D in $DAEMON_DOMAINS; do
        supolicy --live "allow $APP $D unix_stream_socket { connectto read write getattr }" 2>/dev/null
    done; done
    echo "$(date) SELinux rules applied via supolicy" >> $LOG
else
    echo "$(date) WARNING: no sepolicy tool found" >> $LOG
fi

# Start daemon if not running
if ! pidof audio-bridge >/dev/null 2>&1; then
    echo "$(date) Starting audio bridge daemon" >> $LOG
    if [ -f /system/bin/audio-bridge ]; then
        /system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    else
        chmod 755 $MODDIR/system/bin/audio-bridge 2>/dev/null
        $MODDIR/system/bin/audio-bridge --daemon >> $LOG 2>&1 &
    fi
    sleep 3
    if pidof audio-bridge >/dev/null 2>&1; then
        echo "$(date) Daemon started, PID: $(pidof audio-bridge)" >> $LOG
    else
        echo "$(date) WARNING: Daemon failed to start" >> $LOG
    fi
else
    echo "$(date) Daemon already running, PID: $(pidof audio-bridge)" >> $LOG
fi

# Background: wait for framework, install APK if needed, start service.
(
    for i in $(seq 1 60); do
        if [ "$(getprop sys.boot_completed)" = "1" ]; then break; fi
        sleep 2
    done
    sleep 3

    # Fallback install: if priv-app overlay didn't register com.audiobridge
    # (e.g. signing mismatch on restrictive ROMs), pm install from MODDIR.
    if ! pm path com.audiobridge >/dev/null 2>&1; then
        if [ -f "$MODDIR/AudioBridge.apk" ]; then
            echo "$(date) com.audiobridge not registered as priv-app; pm install fallback" >> $LOG
            pm install -r -g "$MODDIR/AudioBridge.apk" >> $LOG 2>&1
        else
            echo "$(date) WARNING: AudioBridge.apk missing from module" >> $LOG
        fi
    else
        echo "$(date) com.audiobridge present at $(pm path com.audiobridge)" >> $LOG
    fi

    # Trigger service start via our custom broadcast. Broadcasts run in the
    # app's own UID so they're exempt from Android 12+'s BG-FGS guard that
    # makes `am start-foreground-service` fail with "app is in background
    # uid null" when fired from the shell.
    for i in 1 2 3 4 5; do
        OUT=$(am broadcast --user 0 -a com.audiobridge.START \
              -n com.audiobridge/.BootReceiver 2>&1)
        echo "$(date) broadcast try $i: $OUT" >> $LOG
        if echo "$OUT" | grep -q "result=0"; then
            echo "$(date) AudioBridgeService start broadcast delivered" >> $LOG
            exit 0
        fi
        sleep 3
    done
    echo "$(date) WARNING: START broadcast never delivered" >> $LOG
) &
