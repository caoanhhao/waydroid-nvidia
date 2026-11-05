#!/bin/sh
# Remove Waydroid-on-NVIDIA (Venus) and go back to Intel GLES (the pre-Venus setup).
#
#   sudo sh release/uninstall.sh --system   # 1. Android part: overlay files, waydroid.cfg props
#   sh release/uninstall.sh                 # 2. host part: waydroid-vtest service and files
set -eu
CFG=/var/lib/waydroid/waydroid.cfg
LIB=/var/lib/waydroid/overlay/vendor/lib64

del_prop() { sed -i "/^$1[[:space:]]*=/d" "$CFG"; }

# Android 13 HWUI keeps a per-app Skia shader cache tagged with the GPU driver's identity.
# When the driver changes, its BlobCache::clear() drops the entries but not the byte count,
# so an app whose old cache was over 512 KiB crashes with SIGFPE in BlobCache::clean()
# before it saves a new one, on every launch (venus/notes-blobcache.md). Delete the caches
# whenever the driver changes; apps rebuild them.
clear_shader_caches() {
    data=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)/.local/share/waydroid/data
    [ -d "$data" ] || { echo "no $data: HWUI shader caches not cleared" >&2; return 0; }
    n=$(find "$data/data" "$data/user_de" -maxdepth 4 -path '*/code_cache/com.android.skia.shaders_cache' \
        -print -delete 2>/dev/null | wc -l)
    echo "removed $n HWUI shader caches"
}

uninstall_system() {
    [ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0 --system" >&2; exit 1; }
    waydroid session stop 2>/dev/null || true
    rm -f "$LIB/hw/vulkan.virtio.so" "$LIB/libgbm_mesa_wrapper.so" "$LIB/libgbm_mesa_wrapper_intel.so"
    rm -f "$LIB/hw/hwcomposer.waydroid.so" "$LIB/hw/hwcomposer.waydroid.real.so"
    rm -f /var/lib/waydroid/overlay/vendor/bin/hw/android.hardware.graphics.composer@2.1-service \
          /var/lib/waydroid/overlay/vendor/bin/hw/android.hardware.graphics.composer@2.1-service.real
    for p in ro.hardware.vulkan mesa.vtest.socket.name gralloc.gbm.linear waydroid.gralloc.vtest \
             debug.hwui.renderer persist.waydroid.cursor_force_shm mesa.vn.debug ro.logd.size \
             debug.renderengine.backend debug.mesa.log.level ro.hardware.egl; do
        del_prop "$p"
    done
    sed -i "/^\[properties\]/a ro.hardware.egl = mesa" "$CFG"
    clear_shader_caches
    waydroid upgrade --offline
    systemctl restart waydroid-container
    echo "Android part removed; Waydroid uses Intel GLES again. Next: sh $0"
}

uninstall_user() {
    [ "$(id -u)" != 0 ] || { echo "run the host part as your user, without sudo" >&2; exit 1; }
    systemctl --user disable --now waydroid-vtest.service 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/waydroid-vtest.service"
    systemctl --user daemon-reload
    rm -rf "$HOME/.local/lib/waydroid-venus"
    rm -f "$HOME/.local/share/waydroid/data/vtest/vtest.sock"
    echo "host part removed"
}

case "${1:-}" in
"") uninstall_user ;;
--system) uninstall_system ;;
*) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
