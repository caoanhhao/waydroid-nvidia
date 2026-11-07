#!/bin/sh
# Cross-build Mesa's Venus Vulkan driver (vulkan.virtio.so) for Waydroid (Android 13) with the
# Android NDK, for both ABIs: 64-bit processes load build-android (x86_64), 32-bit ones (x86, and
# armeabi-v7a apps through the native bridge) build-android-x86.
# Output: build-android{,-x86}/src/virtio/vulkan/libvulkan_virtio.so
set -e
ROOT=$(dirname "$(readlink -f "$0")")/..
P=$ROOT/venus/patches

# Mesa 26.2.6 + the Venus/vtest and Android patches (src/mesa, branch venus-vtest)
if [ ! -d "$ROOT/src/mesa" ]; then
  git clone --branch mesa-26.2.6 https://gitlab.freedesktop.org/mesa/mesa.git "$ROOT/src/mesa"
  git -C "$ROOT/src/mesa" switch -c venus-vtest
  git -C "$ROOT/src/mesa" am "$P"/0001-*.patch "$P"/0002-*.patch "$P"/1001-*.patch \
    "$P"/1002-*.patch "$P"/1003-*.patch "$P"/1004-*.patch "$P"/1007-*.patch "$P"/1008-*.patch \
    "$P"/1009-*.patch "$P"/1010-*.patch "$P"/1011-*.patch
fi
cd "$ROOT/src/mesa"

for abi in x86_64: x86:-x86; do
  b=build-android${abi#*:}
  [ -d $b ] || meson setup $b \
    --cross-file "$ROOT/venus/android-${abi%%:*}.cross" \
    -Dbuildtype=debugoptimized -Dplatforms=android -Dplatform-sdk-version=33 \
    -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
    -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
    -Dgallium-drivers= -Dvulkan-drivers=virtio -Dallow-fallback-for=libdrm \
    -Dllvm=disabled -Dzstd=disabled -Dvulkan-layers= -Dtools=
  meson compile -C $b
  ls -l $b/src/virtio/vulkan/libvulkan_virtio.so
done
