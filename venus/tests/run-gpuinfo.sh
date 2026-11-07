#!/bin/sh
# Print the GPU (Vulkan devices, GLES context) each ABI sees inside Waydroid, and draw with
# the ES 3.2 stages (android-gles32-test.c): x86_64, x86,
# and arm64-v8a / armeabi-v7a through the native bridge. Needs the NDK and adb connected
# (Waydroid window open).
#
# usage: sh venus/tests/run-gpuinfo.sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${NDK:-/opt/android-ndk}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
B=$ROOT/venus/tests/build
mkdir -p "$B"

for t in x86_64:x86_64-linux-android33 x86:i686-linux-android33 \
         arm64-v8a:aarch64-linux-android33 armeabi-v7a:armv7a-linux-androideabi33; do
    abi=${t%%:*}
    "$TC/${t#*:}-clang" -O1 -Wall -o "$B/gpuinfo-$abi" "$ROOT/venus/tests/android-gpuinfo.c" \
        -lvulkan -lEGL -lGLESv2 -ldl
    "$TC/${t#*:}-clang" -O1 -Wall -o "$B/gles32-test-$abi" "$ROOT/venus/tests/android-gles32-test.c" \
        -lEGL -lGLESv3
    adb push -q "$B/gpuinfo-$abi" "$B/gles32-test-$abi" /data/local/tmp/
    adb shell chmod 755 "/data/local/tmp/gpuinfo-$abi" "/data/local/tmp/gles32-test-$abi"
    adb shell "/data/local/tmp/gpuinfo-$abi" 2>&1 || echo "gpuinfo-$abi failed ($?)"
    adb shell "/data/local/tmp/gles32-test-$abi" 2>&1 || echo "gles32-test-$abi failed ($?)"
done
