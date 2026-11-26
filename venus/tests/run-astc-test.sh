#!/bin/sh
# Run the Venus ASTC emulation test on the host against the patched server.
# Starts its own server on a private socket (never the live Waydroid one),
# runs the test with a host build of src/mesa (build-host), then stops the
# server.
#
# Host Venus build (gcc 16 hits an ICE in vk_image.c, use clang):
#   CC=clang CXX=clang++ meson setup src/mesa/build-host -Dplatforms= \
#     -Dvulkan-drivers=virtio -Dgallium-drivers= -Dopengl=false -Degl=disabled \
#     -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dgbm=disabled \
#     -Dllvm=disabled -Dzstd=disabled -Dvulkan-layers= -Dtools=
#   meson compile -C src/mesa/build-host src/virtio/vulkan/vulkan_virtio
#
# usage: sh venus/tests/run-astc-test.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VB=$ROOT/src/virglrenderer/build
MB=$ROOT/src/mesa/build-host
B=$ROOT/venus/tests/build
TEST=$B/vn-astc-upload
SOCK=${XDG_RUNTIME_DIR:-/tmp}/vtest-astc-test.sock
LOG=$B/server-astc.log

mkdir -p "$B"
cc -O1 -g -Wall -I "$ROOT/src/mesa/include" -o "$TEST" "$ROOT/venus/tests/vn-astc-upload.c" -lvulkan || exit 1
cat >"$B/venus-host-icd.json" <<EOF
{ "file_format_version": "1.0.0",
  "ICD": { "library_path": "$MB/src/virtio/vulkan/libvulkan_virtio.so",
           "api_version": "1.4.0" } }
EOF

rm -f "$SOCK"
RENDER_SERVER_EXEC_PATH=$VB/server/virgl_render_server \
LD_LIBRARY_PATH=$VB/src \
    "$VB/vtest/virgl_test_server" --venus --no-virgl --multi-clients \
    --rendernode /dev/dri/renderD129 --socket-path "$SOCK" >"$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null; rm -f "$SOCK"' EXIT

i=0
while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done

VK_ICD_FILENAMES=$B/venus-host-icd.json \
VN_DEBUG=${VN_DEBUG:-vtest,result} \
VTEST_SOCKET_NAME=$SOCK \
    "$TEST"
RET=$?

echo "--- server log ($LOG) ---"
tail -n 10 "$LOG"
exit $RET
