#!/bin/sh
# Build the hwcomposer format shim and the YV12 import test (NDK, API 33).
# See venus/notes-tiktok.md.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${NDK:-/opt/android-ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang
mkdir -p "$HERE/build"

# soname differs from the vendor module it loads (hwcomposer.waydroid.so)
"$CC" -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden \
    -Wl,-soname,hwcomposer.waydroid-format-shim.so \
    -o "$HERE/build/hwcomposer.waydroid.so" "$HERE/hwcomposer-format-shim.c" -llog -ldl
"$CC" -O2 -Wall -o "$HERE/build/android-yv12-import-test" "$HERE/android-yv12-import-test.c" \
    -lEGL -lGLESv2 -landroid -lvulkan -ldl
ls -l "$HERE/build"
