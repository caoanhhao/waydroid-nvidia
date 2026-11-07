#!/bin/sh
# Collect the built pieces into release/dist so install.sh needs nothing from src/.
# Rebuild first if needed: venus/build-venus-android.sh, venus/build-virglrenderer-host.sh,
# src/minigbm-vtest/build-android.sh, src/minigbm-wrapper/build.sh,
# venus/hwc-yuv/build.sh, venus/mapper-yv12/build.sh, venus/gpuid/build.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/release/dist
VB=$ROOT/src/virglrenderer/build

rm -rf "$D"
mkdir -p "$D/android" "$D/host/bin" "$D/host/lib"

cp "$ROOT/src/mesa/build-android/src/virtio/vulkan/libvulkan_virtio.so" "$D/android/vulkan.virtio.so"
cp "$ROOT/src/minigbm-vtest/build-android/libgbm_mesa_wrapper.so" "$D/android/libgbm_mesa_wrapper.so"
# 32-bit Venus: x86 processes and armeabi-v7a apps (native bridge) load vendor/lib/hw
mkdir -p "$D/android/lib"
cp "$ROOT/src/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so" "$D/android/lib/vulkan.virtio.so"
cp "$ROOT/src/minigbm-wrapper/out/libgbm_mesa_wrapper.so" "$D/android/libgbm_mesa_wrapper_intel.so"
cp "$ROOT/venus/cursor/build/composer-preload-wrapper" "$D/android/"
cp "$ROOT/venus/hwc-yuv/build/hwcomposer.waydroid.so" "$D/android/"
# gralloc4 mapper shim, both ABIs (the camera provider is 32-bit)
mkdir -p "$D/android/lib"
cp "$ROOT/venus/mapper-yv12/build/lib64/android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so" "$D/android/"
cp "$ROOT/venus/mapper-yv12/build/lib/android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so" "$D/android/lib/"
# 32-bit Intel wrapper: 32-bit processes (camera provider) map BLOB buffers through it
cp "$ROOT/src/minigbm-wrapper/out/lib/libgbm_mesa_wrapper.so" "$D/android/lib/libgbm_mesa_wrapper.so"
# gpuid GLES shim over ANGLE, both ABIs, and the apps it renames the GPU for
mkdir -p "$D/android/egl" "$D/android/lib/egl"
cp "$ROOT"/venus/gpuid/build/lib64/lib*_gpuid.so "$D/android/egl/"
cp "$ROOT"/venus/gpuid/build/lib/lib*_gpuid.so "$D/android/lib/egl/"
cp "$ROOT/venus/gpuid/gpuid.conf" "$D/android/"

cp "$VB/vtest/virgl_test_server" "$VB/server/virgl_render_server" "$D/host/bin/"
# only the runtime soname link and the library (not the .so dev link or meson's .p dir)
cp -P "$VB"/src/libvirglrenderer.so.1 "$VB"/src/libvirglrenderer.so.1.*[0-9] "$D/host/lib/"

{
    echo "built $(date -Iseconds)"
    echo "mesa         $(git -C "$ROOT/src/mesa" describe --always --dirty) ($(git -C "$ROOT/src/mesa" branch --show-current))"
    echo "virglrenderer $(git -C "$ROOT/src/virglrenderer" describe --always --dirty) ($(git -C "$ROOT/src/virglrenderer" branch --show-current))"
    echo "repo         $(git -C "$ROOT" describe --always --dirty)"
} >"$D/VERSION"
(cd "$D" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum >SHA256SUMS)

cat "$D/VERSION"
find "$D" -type f -o -type l | sort
