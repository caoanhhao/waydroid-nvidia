#!/bin/sh
# Remove what fingerprint/install.sh put into the Waydroid vendor overlay.
#
#   sudo sh fingerprint/uninstall.sh
#
# Enrollments live in Android's persist.vendor.fingerprint.virtual.* props and in
# /data/system; delete the fingerprints in Settings first if you want them gone too.
set -eu
FP=$(cd "$(dirname "$0")" && pwd)
VENDOR=/var/lib/waydroid/overlay/vendor

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }

waydroid session stop 2>/dev/null || true
for f in bin/hw/android.hardware.biometrics.fingerprint-service.example \
    $(cd "$FP/hal" && ls lib64/*.so etc/init/*.rc etc/vintf/manifest/*.xml) \
    $(cd "$FP/overlay" && find . -type f | sed 's|^\./||'); do
    rm -fv "$VENDOR/$f"
done
CFG=/var/lib/waydroid/waydroid.cfg
for p in type udfps.control_illumination sensor_location; do
    sed -i "/^persist\.vendor\.fingerprint\.virtual\.$p[[:space:]]*=/d" "$CFG"
done
waydroid upgrade --offline
# drop the directories only these files used, if now empty
for d in etc/vintf/manifest etc/vintf etc/permissions; do
    rmdir "$VENDOR/$d" 2>/dev/null || true
done
systemctl restart waydroid-container
echo "=== removed"
