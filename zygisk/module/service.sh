#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/local/tmp/audio_bridge_service.log

echo "$(date) Audio Bridge service.sh started" >> $LOG

# Auto-grant permissions (suppress errors if app not yet installed)
pm grant com.audiobridge android.permission.CALL_PHONE 2>/dev/null
pm grant com.audiobridge android.permission.ANSWER_PHONE_CALLS 2>/dev/null
pm grant com.audiobridge android.permission.READ_PHONE_STATE 2>/dev/null
pm grant com.audiobridge android.permission.READ_PHONE_NUMBERS 2>/dev/null
pm grant com.audiobridge android.permission.SEND_SMS 2>/dev/null
pm grant com.audiobridge android.permission.RECEIVE_SMS 2>/dev/null
pm grant com.audiobridge android.permission.READ_SMS 2>/dev/null
pm grant com.audiobridge android.permission.POST_NOTIFICATIONS 2>/dev/null
pm grant com.audiobridge android.permission.RECORD_AUDIO 2>/dev/null
appops set com.audiobridge SYSTEM_ALERT_WINDOW allow 2>/dev/null
# RUN_IN_BACKGROUND / RUN_ANY_IN_BACKGROUND are Samsung-specific app-ops that
# OneUI uses to gate background work. Allowing them is the first step in
# preventing the helper service being put to sleep mid-call.
appops set com.audiobridge RUN_IN_BACKGROUND allow 2>/dev/null
appops set com.audiobridge RUN_ANY_IN_BACKGROUND allow 2>/dev/null

# Signature-protected perms: pm grant works only when the APK is installed
# under /system/priv-app/ AND a matching privapp-permissions overlay exists
# (see module/system/etc/permissions/...). The grants are no-ops on a
# normal /data install; we keep the call so a future priv-app install
# automatically picks them up without re-running the module.
pm grant com.audiobridge android.permission.CAPTURE_AUDIO_OUTPUT 2>/dev/null
pm grant com.audiobridge android.permission.MODIFY_PHONE_STATE 2>/dev/null
pm grant com.audiobridge android.permission.MODIFY_AUDIO_ROUTING 2>/dev/null

# OneUI battery / sleep escapes. Without these, AudioBridgeService is
# routinely killed once the screen is off — even with a foreground
# notification. The settings DB keys are Samsung-specific and tolerated by
# AOSP (writes silently fail). dumpsys deviceidle is AOSP and harmless.
dumpsys deviceidle whitelist +com.audiobridge >/dev/null 2>&1
cmd appops set com.audiobridge RUN_IN_BACKGROUND allow 2>/dev/null
# Samsung "Sleeping apps" / "Deep sleeping apps" lists. Removing the package
# from these lists prevents OneUI from suspending it.
settings put global sleeping_apps "$(settings get global sleeping_apps 2>/dev/null | sed 's/com.audiobridge//g')" 2>/dev/null
settings put global deep_sleeping_apps "$(settings get global deep_sleeping_apps 2>/dev/null | sed 's/com.audiobridge//g')" 2>/dev/null
# OneUI 4+ "Auto-optimization" exclusion — Samsung-specific.
settings put system auto_optimization_app_list_disable "$(settings get system auto_optimization_app_list_disable 2>/dev/null);com.audiobridge" 2>/dev/null

# KR-firmware / non-KR-SIM mismatch warning. Both target devices ship with
# KR CSC; a Vietnamese SIM (Viettel/Vinaphone/MobiFone) won't get VoLTE
# without a CSC swap, which means cellular calls fall back to 3G WCDMA
# where mobile data is suspended for the duration of the call. The daemon
# uses this signal to prefer Wi-Fi over cellular when a call is active.
SALES_CODE=$(getprop ro.csc.sales_code 2>/dev/null)
SIM_OPER=$(getprop gsm.sim.operator.numeric 2>/dev/null)
echo "$(date) CSC=$SALES_CODE  SIM_MCC_MNC=$SIM_OPER" >> $LOG
# Vietnam MCC = 452. Viettel=01, Mobifone=02, Vinaphone=04/05, etc.
case "$SIM_OPER" in
    452*)
        case "$SALES_CODE" in
            SKC|KOO|LUC|KOR|KTC|KTT|SKO)
                echo "$(date) WARNING: KR firmware ($SALES_CODE) + VN SIM ($SIM_OPER) — VoLTE likely unavailable; cellular calls will be 3G WCDMA (no concurrent mobile data). Consider CSC swap to XEV/XXV/DBT." >> $LOG
                # Daemon reads this hint to bias the network strategy.
                echo "1" > /data/local/tmp/audio_bridge.no_volte
                ;;
            *)
                rm -f /data/local/tmp/audio_bridge.no_volte 2>/dev/null
                ;;
        esac
        ;;
    *)
        rm -f /data/local/tmp/audio_bridge.no_volte 2>/dev/null
        ;;
esac

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

    # Decide how to install the APK.
    # Some ROMs (Samsung One UI, certain KernelSU kernels) fail to resolve
    # Resources when the APK is loaded from a systemless /system/priv-app
    # overlay — the app crashes in handleBindApplication with a Resources
    # null-deref before any component runs. We detect this by checking
    # /data/dalvik-cache or recent crashes for com.audiobridge; if the app is
    # advertised as priv-app but crashing, we force a real pm install.
    APK_STATE=$(pm path com.audiobridge 2>/dev/null)
    RECENT_CRASH=$(dumpsys dropbox --print 2>/dev/null | grep -c "Process: com.audiobridge")

    if [ -z "$APK_STATE" ]; then
        echo "$(date) com.audiobridge not registered; pm install from MODDIR" >> $LOG
        [ -f "$MODDIR/AudioBridge.apk" ] && \
            pm install -r -g "$MODDIR/AudioBridge.apk" >> $LOG 2>&1
    elif echo "$APK_STATE" | grep -q "/system/priv-app/" && [ "$RECENT_CRASH" -gt 2 ]; then
        # priv-app path but crashing — fall back to data-app install.
        echo "$(date) priv-app path is crashing ($RECENT_CRASH hits); pm install override" >> $LOG
        if [ -f "$MODDIR/AudioBridge.apk" ]; then
            pm install -r -g "$MODDIR/AudioBridge.apk" >> $LOG 2>&1
        fi
    else
        echo "$(date) com.audiobridge present at $APK_STATE" >> $LOG
    fi

    # Start the FGS via our headless LauncherActivity. Activities started by
    # `am start` count as foreground, so startForegroundService() in the
    # activity's onCreate is exempt from the Android 12+ BG-FGS restriction
    # (ForegroundServiceStartNotAllowedException / mAllowStartForeground=false)
    # that makes both `am startservice` and broadcast receivers fail.
    for i in 1 2 3 4 5; do
        OUT=$(am start --user 0 -n com.audiobridge/.LauncherActivity 2>&1)
        echo "$(date) launcher try $i: $OUT" >> $LOG
        if echo "$OUT" | grep -qE "Starting:|Status: ok"; then
            echo "$(date) AudioBridgeService launch requested" >> $LOG
            exit 0
        fi
        sleep 3
    done
    echo "$(date) WARNING: LauncherActivity never started" >> $LOG
) &
