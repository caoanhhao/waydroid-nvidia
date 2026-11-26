#!/bin/sh
# Test the hwcomposer format shim (venus/notes-tiktok.md). Run as root:
#
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh logs    # save logcat/props/dumpsys of the running session
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh watch   # stream logcat to out/logcat-live.txt (Ctrl-C to stop)
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh yv12test # import a YV12 AHB like SurfaceFlinger (out/yv12-test.txt)
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh venus   # install src/mesa Venus (YV12 plane fix); venus-revert undoes it
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh apply   # logs, then install the shim into the overlay
#   sudo sh venus/hwc-yuv/test-hwc-shim.sh revert  # remove the shim (vendor hwcomposer again)
#
# apply/revert stop the session; start it again as your user (waydroid show-full-ui).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out
HW=/var/lib/waydroid/overlay/vendor/lib64/hw
CFG=/var/lib/waydroid/waydroid.cfg
OWNER=$(stat -c %u:%g "$HERE")

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

logs() {
    mkdir -p "$OUT"
    T=$(date +%H%M%S)
    if waydroid status 2>/dev/null | grep -q 'Session:.*RUNNING'; then
        timeout 60 waydroid shell -- logcat -d -b all > "$OUT/logcat-$T.txt" 2>&1 || true
        timeout 20 waydroid shell -- getprop > "$OUT/props-$T.txt" 2>&1 || true
        timeout 30 waydroid shell -- dumpsys SurfaceFlinger > "$OUT/sf-$T.txt" 2>&1 || true
        echo "saved $OUT/*-$T.txt"
    else
        echo "no running session, no logs"
    fi
    chown -R "$OWNER" "$OUT"
}

case "${1:-}" in
logs)
    logs
    ;;
venus)
    # Venus from src/mesa (with the single-plane YV12 fix); the old one is kept
    # as out/vulkan.virtio.so.orig, "venus-revert" puts it back
    SRC=$HERE/../../src/mesa/build-android/src/virtio/vulkan/libvulkan_virtio.so
    mkdir -p "$OUT"
    [ -f "$OUT/vulkan.virtio.so.orig" ] || cp "$HW/vulkan.virtio.so" "$OUT/vulkan.virtio.so.orig"
    waydroid session stop 2>/dev/null || true
    install -m644 "$SRC" "$HW/vulkan.virtio.so"
    chown -R "$OWNER" "$OUT"
    ls -l "$HW/vulkan.virtio.so"
    echo "installed; start the session as your user: waydroid show-full-ui"
    ;;
venus-revert)
    waydroid session stop 2>/dev/null || true
    install -m644 "$OUT/vulkan.virtio.so.orig" "$HW/vulkan.virtio.so"
    echo "reverted Venus"
    ;;
yv12test)
    # SurfaceFlinger-style import of a YV12 video frame (ANGLE on Venus)
    shift
    mkdir -p "$OUT"
    TMP=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/.local/share/waydroid/data/local/tmp
    install -m755 "$HERE/build/android-yv12-import-test" "$TMP/android-yv12-import-test"
    {
        echo "=== YV12"
        timeout 60 waydroid shell -- env VN_DEBUG=result /data/local/tmp/android-yv12-import-test "$@" 2>&1 || true
        echo "=== control: RGBA_8888, same usage"
        timeout 60 waydroid shell -- env FORMAT=1 /data/local/tmp/android-yv12-import-test "$@" 2>&1 || true
    } > "$OUT/yv12-test.txt"
    chown "$OWNER" "$OUT/yv12-test.txt"
    cat "$OUT/yv12-test.txt"
    ;;
watch)
    # stream logcat into out/logcat-live.txt until Ctrl-C (the logd buffers are small)
    mkdir -p "$OUT"
    chown "$OWNER" "$OUT"
    echo "streaming to $OUT/logcat-live.txt, Ctrl-C to stop"
    waydroid shell -- logcat -b main,system,crash -v threadtime > "$OUT/logcat-live.txt" 2>&1
    ;;
apply)
    logs
    waydroid session stop 2>/dev/null || true
    IMG=$(sed -n 's/^images_path = //p' "$CFG")/vendor.img
    mkdir -p "$HW"
    rm -f "$HW/hwcomposer.waydroid.real.so"
    debugfs -R "dump /lib64/hw/hwcomposer.waydroid.so $HW/hwcomposer.waydroid.real.so" "$IMG" 2>/dev/null
    [ -s "$HW/hwcomposer.waydroid.real.so" ] || { echo "cannot read hwcomposer from $IMG" >&2; exit 1; }
    chmod 644 "$HW/hwcomposer.waydroid.real.so"
    install -m644 "$HERE/build/hwcomposer.waydroid.so" "$HW/hwcomposer.waydroid.so"
    ls -l "$HW"/hwcomposer.waydroid*
    echo "applied; start the session as your user: waydroid show-full-ui"
    ;;
revert)
    waydroid session stop 2>/dev/null || true
    rm -f "$HW/hwcomposer.waydroid.so" "$HW/hwcomposer.waydroid.real.so"
    echo "reverted"
    ;;
*)
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
