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

install_system() {
    [ "$(id -u)" = 0 ] || { echo "run the Android part with sudo: sudo sh $0 --system" >&2; exit 1; }
    LIB=/var/lib/waydroid/overlay/vendor/lib64

    echo "=== Android: overlay + props"
    waydroid session stop 2>/dev/null || true
    install -D -m644 "$D/android/vulkan.virtio.so" "$LIB/hw/vulkan.virtio.so"
    # 32-bit processes (x86, and armeabi-v7a apps through the native bridge) load
    # vendor/lib/hw; vendor.img's own 32-bit Venus cannot reach the vtest socket, so they
    # had no GPU at all
    install -D -m644 "$D/android/lib/vulkan.virtio.so" /var/lib/waydroid/overlay/vendor/lib/hw/vulkan.virtio.so
    # declare what the GPU really offers (CTS checks both against the driver):
    # Vulkan 1.3 (Venus on NVIDIA), and GLES 3.1 without the Android Extension Pack, the
    # most ANGLE conforms to (it lacks geometry/tessellation shaders, texture buffers)
    PERM=/var/lib/waydroid/overlay/vendor/etc/permissions
    mkdir -p "$PERM"
    cat >"$PERM/android.hardware.vulkan.version.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<!-- Vulkan 1.3 (0x403000): the API version Venus reports for the NVIDIA GPU -->
<permissions>
    <feature name="android.hardware.vulkan.version" version="4206592" />
</permissions>
EOF
    cat >"$PERM/android.hardware.opengles.aep.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<!-- no Android Extension Pack: ANGLE's GLES 3.1 lacks geometry/tessellation shaders -->
<permissions />
EOF
    chmod 644 "$PERM/android.hardware.vulkan.version.xml" "$PERM/android.hardware.opengles.aep.xml"
    install -D -m644 "$D/android/libgbm_mesa_wrapper.so" "$LIB/libgbm_mesa_wrapper.so"
    install -D -m644 "$D/android/libgbm_mesa_wrapper_intel.so" "$LIB/libgbm_mesa_wrapper_intel.so"
    # 32-bit processes (the camera provider) use the vendor wrapper directly; the patched one
    # can map BLOB buffers (camera JPEG output)
    install -D -m644 "$D/android/lib/libgbm_mesa_wrapper.so" /var/lib/waydroid/overlay/vendor/lib/libgbm_mesa_wrapper.so
    # composer: run it with libnativewindow preloaded so ANGLE's EGL image import works
    # there. The wrapper replaces the binary; the vendor.img one moves next to it as .real.
    BIN=/var/lib/waydroid/overlay/vendor/bin/hw/android.hardware.graphics.composer@2.1-service
    IMG=$(sed -n 's/^images_path = //p' "$CFG")/vendor.img
    mkdir -p "$(dirname "$BIN")"
    rm -f "$BIN.real"
    debugfs -R "dump /bin/hw/android.hardware.graphics.composer@2.1-service $BIN.real" "$IMG" 2>/dev/null
    [ -s "$BIN.real" ] || { echo "cannot read the composer from $IMG" >&2; exit 1; }
    chmod 755 "$BIN.real"
    install -D -m755 "$D/android/composer-preload-wrapper" "$BIN"
    # hwcomposer: a shim module lets SurfaceFlinger compose the layers Hyprland can't
    # import (YV12 video frames) instead of sending them to Wayland, which kills the
    # composer. It loads the vendor.img module, copied next to it as .real.so.
    rm -f "$LIB/hw/hwcomposer.waydroid.real.so"
    debugfs -R "dump /lib64/hw/hwcomposer.waydroid.so $LIB/hw/hwcomposer.waydroid.real.so" "$IMG" 2>/dev/null
    [ -s "$LIB/hw/hwcomposer.waydroid.real.so" ] || { echo "cannot read hwcomposer from $IMG" >&2; exit 1; }
    chmod 644 "$LIB/hw/hwcomposer.waydroid.real.so"
    install -D -m644 "$D/android/hwcomposer.waydroid.so" "$LIB/hw/hwcomposer.waydroid.so"
    # gralloc4 mapper: a shim gives single-plane YV12 buffers real Cr/Cb plane layouts, so
    # CPU writers (camera, software codecs) put chroma where Mesa reads it (green camera
    # preview otherwise). It loads the vendor.img mapper, copied next to it as .real.so.
    # Both ABIs: the camera provider is a 32-bit process.
    MAPPER=android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa
    for abi in lib64:"$D/android" lib:"$D/android/lib"; do
        dir=/var/lib/waydroid/overlay/vendor/${abi%%:*}/hw
        mkdir -p "$dir"
        rm -f "$dir/$MAPPER.real.so"
        debugfs -R "dump /${abi%%:*}/hw/$MAPPER.so $dir/$MAPPER.real.so" "$IMG" 2>/dev/null
        [ -s "$dir/$MAPPER.real.so" ] || { echo "cannot read /${abi%%:*}/hw/$MAPPER.so from $IMG" >&2; exit 1; }
        chmod 644 "$dir/$MAPPER.real.so"
        install -D -m644 "${abi#*:}/$MAPPER.so" "$dir/$MAPPER.so"
    done
    sed -i 's/^mount_overlays = .*/mount_overlays = True/' "$CFG"

    # Vulkan: Venus over the vtest socket (host waydroid-vtest.service)
    set_prop ro.hardware.vulkan virtio
    set_prop mesa.vtest.socket.name /data/vtest/vtest.sock
    # all GLES through ANGLE on Venus: Intel must never render into NVIDIA buffers
    set_prop ro.hardware.egl angle
    # waydroid declares GLES 3.2 by default; ANGLE gives 3.1 (0x30001)
    set_prop ro.opengles.version 196609
    # gralloc: window buffers allocated by the host (shim); the rest via Intel, linear
    set_prop ro.hardware.gralloc minigbm_gbm_mesa
    set_prop gralloc.gbm.device /dev/dri/renderD128
    set_prop gralloc.gbm.linear 1
    set_prop waydroid.gralloc.vtest 1
    set_prop debug.hwui.renderer skiavk
    # cursor through the composer's EGL readback into shm (the wrapper above fixes
    # its crash); a dma-buf cursor is a LINEAR buffer Hyprland/NVIDIA cannot show
    del_prop persist.waydroid.cursor_force_shm
    # leftovers from debugging
    for p in mesa.vn.debug ro.logd.size debug.renderengine.backend debug.mesa.log.level; do
        del_prop "$p"
    done

    clear_shader_caches
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
