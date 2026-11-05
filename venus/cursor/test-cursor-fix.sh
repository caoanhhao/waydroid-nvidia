#!/bin/sh
# Waydroid hwcomposer cursor crash: reproduce, then apply the fix.
# Cause and details: venus/notes-cursor.md.
#
#   sh venus/cursor/test-cursor-fix.sh test    # needs a RUNNING session; no restart
#   sh venus/cursor/test-cursor-fix.sh apply   # install the .rc, cursor_force_shm=false, restart
#   sh venus/cursor/test-cursor-fix.sh check   # after apply: composer env, tombstones, logcat
#   sh venus/cursor/test-cursor-fix.sh revert  # remove the .rc, cursor_force_shm=true, restart
#
# Output is also written to venus/cursor/test-cursor-fix.txt.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/venus/cursor/test-cursor-fix.txt
TEST=$ROOT/venus/tests/build/android-wrap-handle-test
RC=$ROOT/venus/cursor/zz-waydroid-hwcomposer-nativewindow.rc
RC_DST=/var/lib/waydroid/overlay/vendor/etc/init/zz-waydroid-hwcomposer-nativewindow.rc
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
    sudo install -D -m644 "$RC" "$RC_DST"
    set_prop persist.waydroid.cursor_force_shm false
    restart_waydroid
    check
    } 2>&1 | tee "$OUT"
    ;;
check)
    check 2>&1 | tee "$OUT"
    ;;
revert)
    sudo rm -f "$RC_DST"
    set_prop persist.waydroid.cursor_force_shm true
    restart_waydroid
    ;;
*)
    sed -n 2,10p "$0"
    exit 1
    ;;
esac
