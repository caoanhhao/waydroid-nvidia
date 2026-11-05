#!/bin/sh
# Stage B display-path test: server-allocated NVIDIA buffer, drawn by Venus,
# shown in Hyprland via hwcomposer-style linux-dmabuf v3 (see wl-show-hostalloc.c).
# Private server socket; output in venus/tests/build/wl-show.log.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VB=$ROOT/src/virglrenderer/build
MB=$ROOT/src/mesa-dmabuf/build-host
OUT=$ROOT/venus/tests/build

SOCK=${XDG_RUNTIME_DIR:-/tmp}/vtest-wlshow-test.sock
mkdir -p "$OUT"
for x in linux-dmabuf-v1:stable/linux-dmabuf xdg-shell:stable/xdg-shell; do
    n=${x%%:*}; p=/usr/share/wayland-protocols/${x#*:}/$n.xml
    wayland-scanner client-header $p "$OUT/$n-client.h"
    wayland-scanner private-code $p "$OUT/$n.c"
done
cc -O1 -g -Wall -Wno-unused-parameter -I "$OUT" -I "$ROOT/src/minigbm-vtest" \
    -I "$ROOT/src/mesa-dmabuf/include" -I /usr/include/libdrm -o "$OUT/wl-show-hostalloc" \
    "$ROOT/venus/tests/wl-show-hostalloc.c" "$ROOT/src/minigbm-vtest/vtest_alloc_client.c" \
    "$OUT/linux-dmabuf-v1.c" "$OUT/xdg-shell.c" -lwayland-client -lvulkan || exit 1
rm -f "$SOCK"
RENDER_SERVER_EXEC_PATH=$VB/server/virgl_render_server LD_LIBRARY_PATH=$VB/src \
    "$VB/vtest/virgl_test_server" --venus --no-virgl --multi-clients \
    --rendernode /dev/dri/renderD129 --socket-path "$SOCK" >"$OUT/wl-show-server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -f "$SOCK"' EXIT
i=0; while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
VK_ICD_FILENAMES=$MB/src/virtio/vulkan/virtio_devenv_icd.x86_64.json VN_DEBUG=vtest \
VTEST_SOCKET_NAME=$SOCK ALLOC_SOCKET=$SOCK \
    "$OUT/wl-show-hostalloc" "$@" >"$OUT/wl-show.log" 2>&1
