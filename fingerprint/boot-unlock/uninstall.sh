#!/bin/sh
# Remove the overlay from boot-unlock/install.sh: Android asks for the PIN/password once
# after every start again before a fingerprint may unlock.
#
#   sudo sh fingerprint/boot-unlock/uninstall.sh
set -eu
VENDOR=/var/lib/waydroid/overlay/vendor

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }

waydroid session stop 2>/dev/null || true
rm -fv "$VENDOR/overlay/FingerprintBootUnlock.apk"
rmdir "$VENDOR/overlay" 2>/dev/null || true
systemctl restart waydroid-container
echo "=== boot unlock removed; start Waydroid again"
