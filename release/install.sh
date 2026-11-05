#!/bin/sh
# Install Waydroid-on-NVIDIA (Venus) from release/dist (run release/build.sh first).
#
#   sh release/install.sh                 # 1. host part, as your user (no sudo):
#                                         #    vtest server in ~/.local/lib/waydroid-venus
#                                         #    + systemd user service waydroid-vtest
#   sudo sh release/install.sh --system   # 2. Android part, as root: driver + gralloc shim
#                                         #    into the Waydroid overlay, waydroid.cfg props
#
# Afterwards start/stop Waydroid as usual (app menu / waydroid show-full-ui /
# waydroid session stop). The service starts with your login session.
set -eu
REL=$(cd "$(dirname "$0")" && pwd)
D=$REL/dist

[ -f "$D/android/vulkan.virtio.so" ] || { echo "missing $D, run release/build.sh" >&2; exit 1; }

install_user() {
    [ "$(id -u)" != 0 ] || { echo "run the host part as your user, without sudo" >&2; exit 1; }
    PREFIX=$HOME/.local/lib/waydroid-venus
    UNIT=$HOME/.config/systemd/user/waydroid-vtest.service

    # Android loses its GPU when the server restarts, so stop a running session first
    if waydroid status 2>/dev/null | grep -q 'Session:.*RUNNING'; then
        echo "stopping the running Waydroid session"
        waydroid session stop
    fi
    # a server started by hand would hold the socket
    if pgrep -f 'virgl_test_server.*waydroid/data/vtest' >/dev/null &&
        ! systemctl --user -q is-active waydroid-vtest.service; then
        echo "a vtest server not run by waydroid-vtest.service is serving the socket; stop it first" >&2
        exit 1
    fi

    echo "=== host: vtest server -> $PREFIX"
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX/bin" "$PREFIX/lib" "$(dirname "$UNIT")"
    cp "$D"/host/bin/* "$PREFIX/bin/"
    cp -P "$D"/host/lib/* "$PREFIX/lib/"
    cp "$D/VERSION" "$PREFIX/"
    cp "$REL/waydroid-vtest.service" "$UNIT"
    systemctl --user daemon-reload
    systemctl --user enable waydroid-vtest.service
    systemctl --user restart waydroid-vtest.service
    sleep 1
    systemctl --user --no-pager status waydroid-vtest.service | head -3
    ls -l "$HOME/.local/share/waydroid/data/vtest/vtest.sock"
    echo "next: sudo sh $0 --system"
}

# replace every copy of "key = value" in [properties]: waydroid refuses to parse
# a config with a duplicate key
CFG=/var/lib/waydroid/waydroid.cfg
del_prop() { sed -i "/^$1[[:space:]]*=/d" "$CFG"; }
set_prop() {
    del_prop "$1"
    sed -i "/^\[properties\]/a $1 = $2" "$CFG"
}

install_system() {
    [ "$(id -u)" = 0 ] || { echo "run the Android part with sudo: sudo sh $0 --system" >&2; exit 1; }
    LIB=/var/lib/waydroid/overlay/vendor/lib64

    echo "=== Android: overlay + props"
    waydroid session stop 2>/dev/null || true
    install -D -m644 "$D/android/vulkan.virtio.so" "$LIB/hw/vulkan.virtio.so"
    install -D -m644 "$D/android/libgbm_mesa_wrapper.so" "$LIB/libgbm_mesa_wrapper.so"
    install -D -m644 "$D/android/libgbm_mesa_wrapper_intel.so" "$LIB/libgbm_mesa_wrapper_intel.so"
    sed -i 's/^mount_overlays = .*/mount_overlays = True/' "$CFG"

    # Vulkan: Venus over the vtest socket (host waydroid-vtest.service)
    set_prop ro.hardware.vulkan virtio
    set_prop mesa.vtest.socket.name /data/vtest/vtest.sock
    # all GLES through ANGLE on Venus: Intel must never render into NVIDIA buffers
    set_prop ro.hardware.egl angle
    # gralloc: window buffers allocated by the host (shim); the rest via Intel, linear
    set_prop ro.hardware.gralloc minigbm_gbm_mesa
    set_prop gralloc.gbm.device /dev/dri/renderD128
    set_prop gralloc.gbm.linear 1
    set_prop waydroid.gralloc.vtest 1
    set_prop debug.hwui.renderer skiavk
    # cursor as dma-buf: the composer's EGL path crashes under ANGLE (see README)
    set_prop persist.waydroid.cursor_force_shm true
    # leftovers from debugging
    for p in mesa.vn.debug ro.logd.size debug.renderengine.backend debug.mesa.log.level; do
        del_prop "$p"
    done

    waydroid upgrade --offline
    systemctl restart waydroid-container
    echo "=== done ($(head -1 "$D/VERSION"))"
    echo "Start Waydroid as usual: waydroid show-full-ui (or the app menu). Stop: waydroid session stop"
}

case "${1:-}" in
"") install_user ;;
--system) install_system ;;
*) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
