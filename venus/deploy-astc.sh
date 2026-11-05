#!/bin/sh
# Install or roll back the Venus build with ASTC emulation (Mesa patch 1008)
# in the Waydroid overlay, without touching anything else of the release
# install. Only the Android driver changes; the host server is unchanged.
#
#   sudo sh venus/deploy-astc.sh install    # Android: Venus from src/mesa/build-android
#   sudo sh venus/deploy-astc.sh rollback   # Android: restore the saved driver
#   sh venus/deploy-astc.sh status
#
# Backup: /var/lib/waydroid/overlay/vendor/lib64/hw/vulkan.virtio.so.pre-astc
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SO=$ROOT/src/mesa/build-android/src/virtio/vulkan/libvulkan_virtio.so
HW=/var/lib/waydroid/overlay/vendor/lib64/hw
ACTION=${1:-status}

need_root() {
    [ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0 $ACTION" >&2; exit 1; }
}

case "$ACTION" in
install)
    need_root
    [ -f "$SO" ] || { echo "missing $SO, run venus/build-venus-android.sh first" >&2; exit 1; }
    waydroid session stop 2>/dev/null || true
    [ -f "$HW/vulkan.virtio.so.pre-astc" ] || cp -a "$HW/vulkan.virtio.so" "$HW/vulkan.virtio.so.pre-astc"
    install -m644 "$SO" "$HW/vulkan.virtio.so"
    ls -l "$HW"/vulkan.virtio.so*
    ;;
rollback)
    need_root
    [ -f "$HW/vulkan.virtio.so.pre-astc" ] || { echo "no backup $HW/vulkan.virtio.so.pre-astc" >&2; exit 1; }
    waydroid session stop 2>/dev/null || true
    mv "$HW/vulkan.virtio.so.pre-astc" "$HW/vulkan.virtio.so"
    ls -l "$HW"/vulkan.virtio.so*
    ;;
status)
    ls -l "$HW"/vulkan.virtio.so* 2>/dev/null || echo "overlay not readable"
    sha256sum "$HW/vulkan.virtio.so" "$SO" 2>/dev/null
    ;;
*) echo "usage: $0 install|rollback|status" >&2; exit 2 ;;
esac
