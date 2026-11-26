#!/bin/sh
# Install or roll back the wwm_vkhook Vulkan HAL shim on top of the running release.
# The shim replaces /vendor/lib{64,}/hw/vulkan.virtio.so; the real Venus driver is
# renamed to vulkan.virtio_real.so (what the shim dlopens). The config lives at
# /vendor/etc/wwm_vkhook.conf.
#
#   sudo sh venus/wwm-vkhook/deploy.sh install
#   sudo sh venus/wwm-vkhook/deploy.sh rollback
#   sh venus/wwm-vkhook/deploy.sh status
#
# Backups: vulkan.virtio_real.so (the real driver). Not touched on rollback: if it was
# replaced out of band (another driver patch), that replacement stays in place.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/venus/wwm-vkhook
OV=/var/lib/waydroid/overlay/vendor
ACTION=${1:-status}

need_root() { [ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0 $ACTION" >&2; exit 1; }; }

install_one() {
    hw=$OV/$1/hw
    shim=$HERE/build/$1/vulkan.virtio.so
    [ -f "$shim" ] || { echo "missing $shim, run build.sh first" >&2; exit 1; }
    [ -f "$hw/vulkan.virtio.so" ] || { echo "missing $hw/vulkan.virtio.so" >&2; exit 1; }
    if [ ! -f "$hw/vulkan.virtio_real.so" ]; then
        mv "$hw/vulkan.virtio.so" "$hw/vulkan.virtio_real.so"
        echo "saved real driver to $hw/vulkan.virtio_real.so"
    else
        rm -f "$hw/vulkan.virtio.so"
    fi
    install -m644 "$shim" "$hw/vulkan.virtio.so"
    ls -l "$hw"/vulkan.virtio*.so
}

rollback_one() {
    hw=$OV/$1/hw
    [ -f "$hw/vulkan.virtio_real.so" ] || { echo "no $hw/vulkan.virtio_real.so, nothing to roll back"; return; }
    rm -f "$hw/vulkan.virtio.so"
    mv "$hw/vulkan.virtio_real.so" "$hw/vulkan.virtio.so"
    ls -l "$hw"/vulkan.virtio*.so
}

case "$ACTION" in
install)
    need_root
    waydroid session stop 2>/dev/null || true
    install_one lib64
    install_one lib
    install -D -m644 "$HERE/wwm_vkhook.conf" "$OV/etc/wwm_vkhook.conf"
    ls -l "$OV/etc/wwm_vkhook.conf"
    ;;
rollback)
    need_root
    waydroid session stop 2>/dev/null || true
    rollback_one lib64
    rollback_one lib
    rm -f "$OV/etc/wwm_vkhook.conf"
    ;;
status)
    for a in lib64 lib; do
        echo "--- $OV/$a/hw ---"
        ls -l "$OV/$a/hw"/vulkan.virtio*.so 2>/dev/null || echo "(not readable, try sudo)"
    done
    echo "--- conf ---"
    ls -l "$OV/etc/wwm_vkhook.conf" 2>/dev/null || echo "(no conf installed)"
    ;;
*) echo "usage: $0 install|rollback|status" >&2; exit 2 ;;
esac
