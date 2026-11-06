#!/bin/sh
# Build the gralloc4 mapper YV12 shim (NDK, API 33) for both ABIs: the external camera
# provider is 32-bit. Output: build/lib64/ and build/lib/. See venus/notes-camera.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${NDK:-/opt/android-ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin
NAME=android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa

for abi in lib64:x86_64 lib:i686; do
    dir=${abi%%:*}
    CC=$BIN/${abi#*:}-linux-android33-clang
    mkdir -p "$HERE/build/$dir"
    # soname differs from the vendor module it loads
    "$CC" -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden \
        -Wl,-soname,$NAME-yv12-shim.so \
        -o "$HERE/build/$dir/$NAME.so" "$HERE/mapper-yv12-shim.c" -llog -ldl
    "$CC" -O2 -Wall -o "$HERE/build/$dir/yv12-planes-test" "$HERE/yv12-planes-test.c" -landroid
done
ls -l "$HERE/build"/*
