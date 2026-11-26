#!/bin/sh
# Build the wwm_vkhook Vulkan HAL shim for Waydroid (x86_64 + x86). The shim is installed
# as /vendor/lib{64,}/hw/vulkan.virtio.so; it dlopens the real Venus driver kept side-by-side
# as vulkan.virtio_real.so. See deploy.sh.
# Output: venus/wwm-vkhook/build/{lib64,lib}/vulkan.virtio.so
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:-/opt/android-ndk}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
B=$HERE/build

for t in lib64:x86_64-linux-android33 lib:i686-linux-android33; do
    lib=${t%%:*}
    tri=${t#*:}
    mkdir -p "$B/$lib"
    "$TC/$tri-clang" -shared -fPIC -O2 -Wall -fvisibility=hidden \
        -Wl,-soname,vulkan.virtio.so \
        -o "$B/$lib/vulkan.virtio.so" "$HERE/wwm_vkhook.c" -ldl -llog
done
ls -l "$B"/lib64/vulkan.virtio.so "$B"/lib/vulkan.virtio.so
