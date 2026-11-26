#!/bin/sh
# Cross-build Mesa's Venus Vulkan driver (vulkan.virtio.so) for Waydroid (Android 13, x86_64)
# with the Android NDK. Output: build-android/src/virtio/vulkan/libvulkan_virtio.so
set -e
ROOT=$(dirname "$(readlink -f "$0")")/..
P=$ROOT/venus/patches

# Mesa 26.2.6 + the Venus/vtest and Android patches (src/mesa, branch venus-vtest)
if [ ! -d "$ROOT/src/mesa" ]; then
  git clone --branch mesa-26.2.6 https://gitlab.freedesktop.org/mesa/mesa.git "$ROOT/src/mesa"
  git -C "$ROOT/src/mesa" switch -c venus-vtest
  git -C "$ROOT/src/mesa" am "$P"/0001-*.patch "$P"/0002-*.patch "$P"/1001-*.patch \
    "$P"/1002-*.patch "$P"/1003-*.patch "$P"/1004-*.patch "$P"/1007-*.patch "$P"/1008-*.patch \
    "$P"/1009-*.patch
fi
cd "$ROOT/src/mesa"

[ -d build-android ] || meson setup build-android \
  --cross-file "$ROOT/venus/android-x86_64.cross" \
  -Dbuildtype=debugoptimized -Dplatforms=android -Dplatform-sdk-version=33 \
  -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
  -Dgallium-drivers= -Dvulkan-drivers=virtio -Dallow-fallback-for=libdrm \
  -Dllvm=disabled -Dzstd=disabled -Dvulkan-layers= -Dtools=

meson compile -C build-android
ls -l build-android/src/virtio/vulkan/libvulkan_virtio.so
