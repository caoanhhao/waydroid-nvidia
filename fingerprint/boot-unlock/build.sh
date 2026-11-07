#!/bin/sh
# Build the overlay that lets a fingerprint unlock right after Android starts, into
# fingerprint/boot-unlock/ (installed to /vendor/overlay by boot-unlock/install.sh).
#
#   sh fingerprint/boot-unlock/build.sh
#
# Needs Android build-tools (aapt2, zipalign, apksigner; $BUILD_TOOLS, default the newest
# in /opt/android-sdk/build-tools), keytool, and a running Waydroid for framework-res.apk.
# Static overlays on /vendor are not checked against any key, so a throwaway key signs it.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=$SRC/FingerprintBootUnlock.apk
BT=${BUILD_TOOLS:-$(ls -d /opt/android-sdk/build-tools/* | sort -V | tail -1)}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

sudo waydroid shell -- cat /system/framework/framework-res.apk >"$T/framework-res.apk"
"$BT/aapt2" compile --dir "$SRC/res" -o "$T/res.zip"
"$BT/aapt2" link -o "$T/unsigned.apk" --manifest "$SRC/AndroidManifest.xml" \
    -I "$T/framework-res.apk" --min-sdk-version 33 --target-sdk-version 33 "$T/res.zip"
"$BT/zipalign" -f 4 "$T/unsigned.apk" "$T/aligned.apk"
keytool -genkeypair -keystore "$T/key.jks" -storepass android -alias k -keyalg RSA \
    -keysize 2048 -validity 10000 -dname CN=waydroid-overlay >/dev/null 2>&1
"$BT/apksigner" sign --v4-signing-enabled false --ks "$T/key.jks" --ks-pass pass:android \
    --out "$OUT" "$T/aligned.apk"
echo "built $OUT"
