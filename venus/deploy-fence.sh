#!/bin/sh
# Install or roll back the real sync-fd fence build (vtest protocol 7) on top of
# the release install (release/install.sh). Nothing in release/ or waydroid.cfg
# is changed. Old and new halves interoperate (each falls back to the patch 0001
# CPU wait), so the two parts can be installed or rolled back independently.
#
#   sh venus/deploy-fence.sh install             # host: server from src/virglrenderer-fence
#   sudo sh venus/deploy-fence.sh install --system   # Android: Venus from src/mesa-fence
#   sh venus/deploy-fence.sh rollback            # host: restore the saved release server
#   sudo sh venus/deploy-fence.sh rollback --system  # Android: restore the saved driver
#   sh venus/deploy-fence.sh status
#
# Backups: /usr/local/lib/waydroid-venus.pre-fence and
# /var/lib/waydroid/overlay/vendor/lib64/hw/vulkan.virtio.so.pre-fence
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VB=$ROOT/src/virglrenderer-fence/build
SO=$ROOT/src/mesa-fence/build-android/src/virtio/vulkan/libvulkan_virtio.so
HW=/var/lib/waydroid/overlay/vendor/lib64/hw
ACTION=${1:-status}
PART=${2:-host}

stop_session() {
    if waydroid status 2>/dev/null | grep -q 'Session:.*RUNNING'; then
        echo "stopping the running Waydroid session (Android loses its GPU when the server restarts)"
        waydroid session stop
    fi
}

host_install() {
    [ "$(id -u)" != 0 ] || { echo "run the host part as your user, without sudo" >&2; exit 1; }
    P=/usr/local/lib/waydroid-venus
    [ -x "$VB/vtest/virgl_test_server" ] || { echo "missing $VB, build src/virglrenderer-fence first" >&2; exit 1; }
    [ -d "$P" ] || { echo "missing $P, run release/install.sh first" >&2; exit 1; }
    stop_session
    if [ ! -d "$P.pre-fence" ]; then
        cp -a "$P" "$P.pre-fence"
        echo "saved the release server to $P.pre-fence"
    fi
    # running binaries cannot be overwritten in place ("Text file busy")
    systemctl --user stop waydroid-vtest.service
    rm -f "$P/bin/virgl_test_server" "$P/bin/virgl_render_server" "$P"/lib/libvirglrenderer.so.1*
    cp "$VB/vtest/virgl_test_server" "$VB/server/virgl_render_server" "$P/bin/"
    cp -P "$VB"/src/libvirglrenderer.so.1 "$VB"/src/libvirglrenderer.so.1.*[0-9] "$P/lib/"
    echo "virglrenderer $(git -C "$ROOT/src/virglrenderer-fence" describe --always --dirty) (vtest-fence)" >"$P/VERSION.fence"
    systemctl --user start waydroid-vtest.service
    sleep 1
    systemctl --user --no-pager status waydroid-vtest.service | head -3
}

host_rollback() {
    [ "$(id -u)" != 0 ] || { echo "run the host part as your user, without sudo" >&2; exit 1; }
    P=/usr/local/lib/waydroid-venus
    [ -d "$P.pre-fence" ] || { echo "no backup at $P.pre-fence, nothing to roll back" >&2; exit 1; }
    stop_session
    systemctl --user stop waydroid-vtest.service
    rm -rf "$P"
    mv "$P.pre-fence" "$P"
    systemctl --user start waydroid-vtest.service
    sleep 1
    systemctl --user --no-pager status waydroid-vtest.service | head -3
    echo "release server restored"
}

android_install() {
    [ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0 install --system" >&2; exit 1; }
    [ -f "$SO" ] || { echo "missing $SO, build src/mesa-fence/build-android first" >&2; exit 1; }
    waydroid session stop 2>/dev/null || true
    [ -f "$HW/vulkan.virtio.so.pre-fence" ] || cp -a "$HW/vulkan.virtio.so" "$HW/vulkan.virtio.so.pre-fence"
    install -m644 "$SO" "$HW/vulkan.virtio.so"
    ls -l "$HW"/vulkan.virtio.so*
}

android_rollback() {
    [ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0 rollback --system" >&2; exit 1; }
    [ -f "$HW/vulkan.virtio.so.pre-fence" ] || { echo "no backup $HW/vulkan.virtio.so.pre-fence" >&2; exit 1; }
    waydroid session stop 2>/dev/null || true
    mv "$HW/vulkan.virtio.so.pre-fence" "$HW/vulkan.virtio.so"
    ls -l "$HW"/vulkan.virtio.so*
}

case "$ACTION:$PART" in
install:host) host_install ;;
install:--system) android_install ;;
rollback:host) host_rollback ;;
rollback:--system) android_rollback ;;
status:*)
    P=/usr/local/lib/waydroid-venus
    echo "host: $( [ -f "$P/VERSION.fence" ] && cat "$P/VERSION.fence" || echo 'release server (no fence build)')"
    [ -d "$P.pre-fence" ] && echo "      backup: $P.pre-fence"
    ls -l "$HW"/vulkan.virtio.so* 2>/dev/null || echo "android: overlay not readable (try sudo)"
    sha256sum "$SO" 2>/dev/null | sed 's/^/fence build: /'
    ;;
*) echo "usage: $0 install|rollback [--system] | status" >&2; exit 2 ;;
esac
