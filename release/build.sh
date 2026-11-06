#!/bin/sh
# Collect the built pieces into release/dist so install.sh needs nothing from src/.
# Rebuild first if needed: venus/build-venus-android.sh, venus/build-virglrenderer-host.sh,
# src/minigbm-vtest/build-android.sh, src/minigbm-wrapper/build.sh,
# venus/hwc-yuv/build.sh, venus/mapper-yv12/build.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/release/dist
VB=$ROOT/src/virglrenderer/build

rm -rf "$D"
mkdir -p "$D/android" "$D/host/bin" "$D/host/lib"

cp "$ROOT/src/mesa/build-android/src/virtio/vulkan/libvulkan_virtio.so" "$D/android/vulkan.virtio.so"
cp "$ROOT/src/minigbm-vtest/build-android/libgbm_mesa_wrapper.so" "$D/android/libgbm_mesa_wrapper.so"
cp "$ROOT/src/minigbm-wrapper/out/libgbm_mesa_wrapper.so" "$D/android/libgbm_mesa_wrapper_intel.so"
cp "$ROOT/venus/cursor/build/composer-preload-wrapper" "$D/android/"
cp "$ROOT/venus/hwc-yuv/build/hwcomposer.waydroid.so" "$D/android/"
# gralloc4 mapper shim, both ABIs (the camera provider is 32-bit)
mkdir -p "$D/android/lib"
cp "$ROOT/venus/mapper-yv12/build/lib64/android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so" "$D/android/"
cp "$ROOT/venus/mapper-yv12/build/lib/android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so" "$D/android/lib/"

cp "$VB/vtest/virgl_test_server" "$VB/server/virgl_render_server" "$D/host/bin/"
# only the runtime soname link and the library (not the .so dev link or meson's .p dir)
cp -P "$VB"/src/libvirglrenderer.so.1 "$VB"/src/libvirglrenderer.so.1.*[0-9] "$D/host/lib/"

{
    echo "built $(date -Iseconds)"
    echo "mesa         $(git -C "$ROOT/src/mesa" describe --always --dirty) ($(git -C "$ROOT/src/mesa" branch --show-current))"
    echo "virglrenderer $(git -C "$ROOT/src/virglrenderer" describe --always --dirty) ($(git -C "$ROOT/src/virglrenderer" branch --show-current))"
    echo "minigbm-vtest $(git -C "$ROOT/src/minigbm-vtest" describe --always --dirty 2>/dev/null || echo '?')"
} >"$D/VERSION"
(cd "$D" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum >SHA256SUMS)

cat "$D/VERSION"
find "$D" -type f -o -type l | sort
