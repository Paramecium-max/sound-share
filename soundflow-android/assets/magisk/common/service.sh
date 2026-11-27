#!/system/bin/sh
# SoundFlow Standalone Receiver boot guard & HyperOS anti-kill protection
PKG="io.soundflow.receiver"
PROC_SVC="io.soundflow.receiver:receiver"

# Wait for Android framework & settings provider
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done
sleep 6

dumpsys deviceidle whitelist +"$PKG" >/dev/null 2>&1
cmd appops set "$PKG" RUN_IN_BACKGROUND allow >/dev/null 2>&1
cmd appops set "$PKG" RUN_ANY_IN_BACKGROUND allow >/dev/null 2>&1
cmd appops set "$PKG" START_FOREGROUND allow >/dev/null 2>&1
am set-standby-bucket "$PKG" active >/dev/null 2>&1

CUR=$(settings get system perf_proc_protect_list 2>/dev/null)
[ "$CUR" = "null" ] && CUR=""
NEW="$CUR"
case ",$NEW," in *,"$PKG",*) ;; *) NEW="${NEW:+$NEW,}$PKG" ;; esac
case ",$NEW," in *,"$PROC_SVC",*) ;; *) NEW="${NEW:+$NEW,}$PROC_SVC" ;; esac
settings put system perf_proc_protect_list "$NEW" >/dev/null 2>&1

# Auto-start :receiver service if user enabled persistence (wait for FBE user-0 unlock up to 120s)
PREF_FILE="/data/user/0/$PKG/shared_prefs/soundflow_cfg.xml"
WAIT=0
while [ ! -f "$PREF_FILE" ] && [ "$WAIT" -lt 120 ]; do
    sleep 2
    WAIT=$((WAIT + 2))
done
if [ -f "$PREF_FILE" ] && grep -q 'name="enabled" value="true"' "$PREF_FILE"; then
    am start-foreground-service -n "$PKG/.ReceiverService" -a "io.soundflow.receiver.ACTION_START_RECEIVER" >/dev/null 2>&1
fi

