#!/bin/sh
# Stage B host test: host-allocated (NVIDIA block-linear) window buffers via
# VCMD_ALLOC_DMABUF_IMAGE, allocated through the gralloc shim, rendered by
# Venus and read back by Venus and by NVIDIA EGL (Hyprland's import path).
# Starts its own patched server on a private socket, never the Waydroid one.
#
# usage: sh venus/tests/run-hostalloc-test.sh [width height]
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VB=$ROOT/src/virglrenderer/build
MB=$ROOT/src/mesa-dmabuf/build-host
SHIM=$ROOT/src/minigbm-vtest
OUT=$ROOT/venus/tests/build
SOCK=${XDG_RUNTIME_DIR:-/tmp}/vtest-hostalloc-test.sock
LOG=$OUT/hostalloc-server.log

mkdir -p "$OUT"
cc -O1 -g -Wall -shared -fPIC -fvisibility=hidden -I "$SHIM" \
    -o "$OUT/libgbm_mesa_wrapper_vtest_host.so" \
    "$SHIM/vtest_gbm_shim.c" "$SHIM/vtest_alloc_client.c" -ldl -lpthread || exit 1
cc -O1 -g -Wall -shared -fPIC -I "$SHIM" -o "$OUT/libfake_inner_wrapper.so" \
    "$ROOT/venus/tests/fake-inner-wrapper.c" || exit 1
cc -O1 -g -Wall -I "$SHIM" -I "$ROOT/src/mesa-dmabuf/include" -I /usr/include/libdrm \
    -o "$OUT/vtest-hostalloc" "$ROOT/venus/tests/vtest-hostalloc.c" \
    -lvulkan -lEGL -lGLESv2 -ldl || exit 1

rm -f "$SOCK"
RENDER_SERVER_EXEC_PATH=$VB/server/virgl_render_server \
LD_LIBRARY_PATH=$VB/src \
    "$VB/vtest/virgl_test_server" --venus --no-virgl --multi-clients \
    --rendernode /dev/dri/renderD129 --socket-path "$SOCK" >"$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null; rm -f "$SOCK"' EXIT

i=0
while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done

# Hyprland's live list for AB24 (waydroid.modifiers.34324241.*), as hwcomposer
# sets it: block-linear 0x...60601x and compressed 0x...e0801x
n=0
for kind in 606 e08; do
    for h in 0 1 2 3 4 5; do
        export "waydroid_modifiers_34324241_$n=300000000${kind}01$h"
        n=$((n + 1))
    done
done

VK_ICD_FILENAMES=$MB/src/virtio/vulkan/virtio_devenv_icd.x86_64.json \
VN_DEBUG=${VN_DEBUG:-vtest,result} \
VTEST_SOCKET_NAME=$SOCK \
waydroid_gralloc_vtest_socket=$SOCK \
    "$OUT/vtest-hostalloc" "$OUT/libgbm_mesa_wrapper_vtest_host.so" \
    "$OUT/libfake_inner_wrapper.so" "$@"
RET=$?

echo "--- server log ($LOG) ---"
tail -n 20 "$LOG"
exit $RET
