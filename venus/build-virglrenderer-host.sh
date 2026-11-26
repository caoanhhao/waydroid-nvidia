#!/bin/sh
# Build the patched virglrenderer (vtest dma-buf import, protocol v5) for the host.
# Nothing is installed: run the server from the build dir, e.g.
#
#   B=src/virglrenderer/build
#   RENDER_SERVER_EXEC_PATH=$B/server/virgl_render_server \
#   LD_LIBRARY_PATH=$B/src \
#   $B/vtest/virgl_test_server --venus --no-virgl --multi-clients \
#       --rendernode /dev/dri/renderD129 --socket-path /tmp/vtest-dmabuf.sock
#
# The system /usr/lib/virgl_render_server and the live Waydroid server are untouched.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src/virglrenderer
BUILD=$SRC/build

if [ ! -d "$SRC" ]; then
    git clone --branch 1.3.0 https://gitlab.freedesktop.org/virgl/virglrenderer.git "$SRC"
    git -C "$SRC" switch -c vtest-dmabuf
    git -C "$SRC" am "$ROOT"/venus/patches/vrend-0001-*.patch "$ROOT"/venus/patches/vrend-0002-*.patch
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" "$SRC" \
        --buildtype=debugoptimized \
        -Dvenus=true \
        -Dvideo=false \
        -Drender-server-worker=process
fi

ninja -C "$BUILD"

echo
echo "built: $BUILD/vtest/virgl_test_server"
echo "       $BUILD/server/virgl_render_server"
