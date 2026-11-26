#!/bin/sh
# Waydroid hwcomposer cursor crash: reproduce, then apply the fix.
# Cause and details: venus/notes-cursor.md.
#
#   sh venus/cursor/test-cursor-fix.sh test    # needs a RUNNING session; no restart
#   sh venus/cursor/test-cursor-fix.sh apply   # install the composer wrapper, cursor_force_shm=false, restart
#   sh venus/cursor/test-cursor-fix.sh check   # after apply: composer env, tombstones, logcat
#   sh venus/cursor/test-cursor-fix.sh revert  # remove the wrapper, cursor_force_shm=true, restart
#
# The fix: the overlay replaces /vendor/bin/hw/android.hardware.graphics.composer@2.1-service
# with composer-preload-wrapper (sets LD_PRELOAD=libnativewindow.so) and puts the vendor.img
# binary next to it as ...-service.real. The earlier .rc override was not applied by init;
# apply removes it.
#
# Output is also written to venus/cursor/test-cursor-fix.txt (test) and
# venus/cursor/test-cursor-fix-apply.txt (apply, check).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/venus/cursor/test-cursor-fix.txt
OUT_APPLY=$ROOT/venus/cursor/test-cursor-fix-apply.txt
TEST=$ROOT/venus/tests/build/android-wrap-handle-test
WRAPPER=$ROOT/venus/cursor/build/composer-preload-wrapper
RC_DST=/var/lib/waydroid/overlay/vendor/etc/init/zz-waydroid-hwcomposer-nativewindow.rc
BIN=/var/lib/waydroid/overlay/vendor/bin/hw/android.hardware.graphics.composer@2.1-service
CFG=/var/lib/waydroid/waydroid.cfg
TMPDIR_A=$HOME/.local/share/waydroid/data/local/tmp

set_prop() {
    sudo sed -i "/^$1[[:space:]]*=/d" "$CFG"
    sudo sed -i "/^\[properties\]/a $1 = $2" "$CFG"
}

restart_waydroid() {
    sudo waydroid session stop 2>/dev/null || true
    sudo waydroid upgrade --offline
    sudo systemctl restart waydroid-container
    waydroid session start &
    for i in $(seq 1 40); do waydroid status | grep -q 'Session:.*RUNNING' && break; sleep 2; done
    sleep 20
}

ws() { sudo waydroid shell -- "$@"; }

check() {
    echo "=== composer process"
    ws sh -c 'p=$(pidof android.hardware.graphics.composer@2.1-service); echo pid $p;
        tr "\0" "\n" < /proc/$p/environ | grep LD_PRELOAD || echo "no LD_PRELOAD";
        ls -l /vendor/bin/hw/android.hardware.graphics.composer@2.1-service*;
        ps -o PID,ELAPSED,NAME -p $p'
    echo "=== props"
    ws getprop persist.waydroid.cursor_force_shm
    ws getprop sys.boot_completed
    echo "=== tombstones (newest 5)"
    ws sh -c 'ls -lt /data/tombstones | head -6'
    echo "=== composer/SF log lines"
    ws sh -c 'logcat -d -b all | grep -iE "eglCreateImage|egl_render|cursor|hwcomposer|DEAD_OBJECT|libGLESv2_angle" | tail -30'
}

case "${1:-}" in
test)
    {
    waydroid status | grep -q 'Session:.*RUNNING' || { echo "start Waydroid first"; exit 1; }
    sudo install -D -m755 "$TEST" "$TMPDIR_A/android-wrap-handle-test"
    for preload in "" libnativewindow.so; do
        for src in wrap ahb; do
            echo "=== SRC=$src LD_PRELOAD=${preload:-<none>}"
            ws sh -c "cd /data/local/tmp && SRC=$src ${preload:+LD_PRELOAD=$preload} ./android-wrap-handle-test 25 32 2>&1; echo exit \$?" || true
        done
    done
    } 2>&1 | tee "$OUT"
    ;;
apply)
    {
    img=$(sed -n 's/^images_path = //p' "$CFG")/vendor.img
    tmp=$(mktemp -d)
    debugfs -R "dump /bin/hw/android.hardware.graphics.composer@2.1-service $tmp/real" "$img" 2>/dev/null
    [ -s "$tmp/real" ] || { echo "cannot read the composer from $img"; exit 1; }
    sudo install -D -m755 "$tmp/real" "$BIN.real"
    sudo install -D -m755 "$WRAPPER" "$BIN"
    rm -rf "$tmp"
    sudo rm -f "$RC_DST"
    set_prop persist.waydroid.cursor_force_shm false
    restart_waydroid
    check
    } 2>&1 | tee "$OUT_APPLY"
    ;;
check)
    check 2>&1 | tee "$OUT_APPLY"
    ;;
revert)
    sudo rm -f "$RC_DST" "$BIN" "$BIN.real"
    set_prop persist.waydroid.cursor_force_shm true
    restart_waydroid
    ;;
*)
    sed -n 2,10p "$0"
    exit 1
    ;;
esac
