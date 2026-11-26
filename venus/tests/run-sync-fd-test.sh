#!/bin/sh
# Run the vtest sync fd export test on the host on a private socket (never
# the live Waydroid one), then stop the server.
#
# usage: sh venus/tests/run-sync-fd-test.sh [new|old-server] [iterations]
#   new         patched server (protocol 7) + patched Venus: real sync fds
#   old-server  v6 server (src/virglrenderer) + patched Venus: CPU-wait fallback
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:-new}
ITERS=${2:-5}
case $MODE in
new)        VB=$ROOT/src/virglrenderer-fence/build ;;
old-server) VB=$ROOT/src/virglrenderer/build ;;
*) echo "unknown mode $MODE"; exit 2 ;;
esac
MB=${MB:-$ROOT/src/mesa-fence/build-host}
TEST=$ROOT/venus/tests/build/vtest-sync-fd
SOCK=${XDG_RUNTIME_DIR:-/tmp}/vtest-sync-fd-test.sock
LOG=$ROOT/venus/tests/build/server-sync-fd.log

mkdir -p "$ROOT/venus/tests/build"
cc -O1 -g -Wall -I "$ROOT/src/mesa-fence/include" -o "$TEST" "$ROOT/venus/tests/vtest-sync-fd.c" -lvulkan || exit 1

rm -f "$SOCK"
RENDER_SERVER_EXEC_PATH=$VB/server/virgl_render_server \
LD_LIBRARY_PATH=$VB/src \
    "$VB/vtest/virgl_test_server" --venus --no-virgl --multi-clients \
    --rendernode /dev/dri/renderD129 --socket-path "$SOCK" >"$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null; rm -f "$SOCK"' EXIT

i=0
while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done

VK_ICD_FILENAMES=$MB/src/virtio/vulkan/virtio_devenv_icd.x86_64.json \
VN_DEBUG=${VN_DEBUG:-vtest,result} \
VTEST_SOCKET_NAME=$SOCK \
    "$TEST" "$ITERS"
RET=$?

echo "--- server log ($LOG) ---"
tail -n 20 "$LOG"
exit $RET
