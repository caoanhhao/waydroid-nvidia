#!/bin/sh
# Run the vtest dma-buf import test on the host against the patched server.
# Starts its own server on a private socket (never the live Waydroid one),
# runs the test with the patched Mesa Venus ICD, then stops the server.
#
# usage: sh venus/tests/run-dmabuf-test.sh [gbm:/dev/dri/renderD128 | gbm:/dev/dri/renderD129 | heap:/dev/dma_heap/system]
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VB=$ROOT/src/virglrenderer/build
MB=$ROOT/src/mesa-dmabuf/build-host
TEST=$ROOT/venus/tests/build/vtest-dmabuf-import
SOCK=${XDG_RUNTIME_DIR:-/tmp}/vtest-dmabuf-test.sock
LOG=$ROOT/venus/tests/build/server.log

mkdir -p "$ROOT/venus/tests/build"
cc -O1 -g -Wall -I "$ROOT/src/mesa-dmabuf/include" -o "$TEST" \
    "$ROOT/venus/tests/vtest-dmabuf-import.c" -lvulkan -lgbm || exit 1

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
CLFLUSH=${CLFLUSH:-1} \
VTEST_SOCKET_NAME=$SOCK \
    "$TEST" "$@"
RET=$?

echo "--- server log ($LOG) ---"
tail -n 20 "$LOG"
exit $RET
