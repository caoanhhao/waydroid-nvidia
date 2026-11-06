#!/bin/sh
# Cross-build the gralloc vtest shim (libgbm_mesa_wrapper.so) for Waydroid
# (Android 13, x86_64) with the NDK. Output: build-android/libgbm_mesa_wrapper.so
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
NDK=${ANDROID_NDK:-/opt/android-ndk}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang
OUT=$DIR/build-android

mkdir -p "$OUT"
"$CC" -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -shared \
    -fvisibility=hidden -Wl,-soname,libgbm_mesa_wrapper.so -Wl,--no-undefined \
    -I "$DIR" -o "$OUT/libgbm_mesa_wrapper.so" \
    "$DIR/vtest_gbm_shim.c" "$DIR/vtest_alloc_client.c" -llog -ldl

ls -l "$OUT/libgbm_mesa_wrapper.so"
