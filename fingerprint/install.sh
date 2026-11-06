#!/bin/sh
# Install AOSP's virtual fingerprint HAL into the Waydroid vendor overlay, so Android
# gets a fingerprint sensor driven by sysprops (see fingerprint/fp.sh).
#
#   sudo sh fingerprint/install.sh
#
# Copies fingerprint/hal (HAL binary, libs, init .rc, VINTF fragment) and
# fingerprint/overlay (feature flag) into /var/lib/waydroid/overlay/vendor, then
# restarts waydroid-container. Undo with fingerprint/uninstall.sh.
#
# The sensor is an under-display (UDFPS) one: its HAL waits for a press on the sensor spot,
# so `fp.sh touch` works whenever something is listening. The rear type decides 0.4 s after
# the lock screen starts listening and then goes idle, so a later touch does nothing.
set -eu
FP=$(cd "$(dirname "$0")" && pwd)
VENDOR=/var/lib/waydroid/overlay/vendor
CFG=/var/lib/waydroid/waydroid.cfg

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }
[ -x "$FP/hal/bin/hw/android.hardware.biometrics.fingerprint-service.example" ] ||
    { echo "missing $FP/hal: the HAL binaries are not there" >&2; exit 1; }

echo "=== fingerprint HAL -> $VENDOR"
waydroid session stop 2>/dev/null || true
# hal/orig and hal/src are reference copies, not for the device
for f in bin/hw/android.hardware.biometrics.fingerprint-service.example \
    $(cd "$FP/hal" && ls lib64/*.so etc/init/*.rc etc/vintf/manifest/*.xml); do
    case $f in bin/*) mode=755 ;; *) mode=644 ;; esac
    install -D -m$mode "$FP/hal/$f" "$VENDOR/$f"
done
(cd "$FP/overlay" && find . -type f) | while read -r f; do
    install -D -m644 "$FP/overlay/$f" "$VENDOR/$f"
done
sed -i 's/^mount_overlays = .*/mount_overlays = True/' "$CFG"

# HAL settings as default props; values later set with setprop (fp.sh place) win
del_prop() { sed -i "/^$1[[:space:]]*=/d" "$CFG"; }
set_prop() {
    del_prop "$1"
    sed -i "/^\[properties\]/a $1 = $2" "$CFG"
}
set_prop persist.vendor.fingerprint.virtual.type udfps
# no display illumination step: the HAL acts on the press itself
set_prop persist.vendor.fingerprint.virtual.udfps.control_illumination true
# x:y:radius in display pixels; fits a display down to ~360x650. Move it with fp.sh place
set_prop persist.vendor.fingerprint.virtual.sensor_location 180:560:36

waydroid upgrade --offline

systemctl restart waydroid-container
echo "=== done. Start Waydroid, then: sh $FP/fp.sh status"
