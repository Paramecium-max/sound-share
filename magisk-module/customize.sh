# Kitsune Magisk / Magisk / Recovery installer script for SoundFlow Receiver
SKIPUNZIP=0
ui_print "- Installing SoundFlow Standalone Audio Receiver (/product/priv-app)..."
mkdir -p "$MODPATH/system/product/priv-app/SoundFlowReceiver" \
         "$MODPATH/system/product/etc/permissions" \
         /data/adb/service.d

# Keep service.d boot guard script synced
cp -f "$MODPATH/service.sh" /data/adb/service.d/soundflow_receiver_guard.sh
set_perm /data/adb/service.d/soundflow_receiver_guard.sh 0 0 0755
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755

ui_print "- Preserving original Xiaomi interconnect APKs untouched."
ui_print "- Reboot to activate system priv-app hardening and anti-kill boot guard."

