#!/bin/sh
# Remove Waydroid-on-NVIDIA (Venus) and go back to Intel GLES (the pre-Venus setup).
#
#   sudo sh release/uninstall.sh
set -eu
CFG=/var/lib/waydroid/waydroid.cfg
LIB=/var/lib/waydroid/overlay/vendor/lib64
PREFIX=/usr/local/lib/waydroid-venus
UNIT=/etc/systemd/system/waydroid-vtest.service
DROPIN_DIR=/etc/systemd/system/waydroid-vtest.service.d

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }

WD_USER=${SUDO_USER:-$(logname 2>/dev/null || true)}
WD_HOME=
[ -n "$WD_USER" ] && [ "$WD_USER" != root ] && WD_HOME=$(getent passwd "$WD_USER" | cut -d: -f6)

del_prop() { sed -i "/^$1[[:space:]]*=/d" "$CFG"; }

clear_shader_caches() {
    [ -n "$WD_HOME" ] && data=$WD_HOME/.local/share/waydroid/data || return 0
    [ -d "$data" ] || { echo "no $data: HWUI shader caches not cleared" >&2; return 0; }
    n=$(find "$data/data" "$data/user_de" -maxdepth 4 -path '*/code_cache/com.android.skia.shaders_cache' \
        -print -delete 2>/dev/null | wc -l)
    echo "removed $n HWUI shader caches"
}

echo "=== stopping waydroid-container.service (vtest follows)"
systemctl stop waydroid-container.service 2>/dev/null || true
systemctl disable waydroid-vtest.service 2>/dev/null || true
rm -f "$UNIT"
rm -rf "$DROPIN_DIR"
rm -rf "$PREFIX"

# retire the old per-user service if a pre-container-bound install left it
if [ -n "$WD_HOME" ] && [ -f "$WD_HOME/.config/systemd/user/waydroid-vtest.service" ]; then
    sudo -u "$WD_USER" XDG_RUNTIME_DIR=/run/user/$(id -u "$WD_USER") \
        systemctl --user disable --now waydroid-vtest.service 2>/dev/null || true
    rm -f "$WD_HOME/.config/systemd/user/waydroid-vtest.service"
    rm -rf "$WD_HOME/.local/lib/waydroid-venus"
fi

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
systemctl daemon-reload
systemctl start waydroid-container.service

echo "Waydroid uses Intel GLES again."
