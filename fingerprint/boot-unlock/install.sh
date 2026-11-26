#!/bin/sh
# Optional add-on to fingerprint/install.sh: let a fingerprint unlock right after Android
# starts, without entering the screen lock PIN/password once first. Android normally
# requires it after every boot (config_strongAuthRequiredOnBoot); this overlay turns that
# off, so automation (fp.sh touch) works from a cold start.
#
#   sudo sh fingerprint/boot-unlock/install.sh
#
# Anyone with adb can then unlock Android with fp.sh touch. Undo with uninstall.sh here.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
DST=/var/lib/waydroid/overlay/vendor/overlay/FingerprintBootUnlock.apk

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }

waydroid session stop 2>/dev/null || true
install -D -m644 "$SRC/FingerprintBootUnlock.apk" "$DST"
systemctl restart waydroid-container
echo "=== boot unlock installed; start Waydroid again"
