#!/bin/sh
# Build the gpuid GLES shim (gpuid.c) for x86_64 and x86. The shims link against the vendor
# ANGLE libraries only to record them as DT_NEEDED; copies are pulled from the running
# Waydroid with adb (needs the Waydroid window open).
# Output: venus/gpuid/build/{lib64,lib}/lib{EGL,GLESv1_CM,GLESv2}_gpuid.so
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:-/opt/android-ndk}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
B=$HERE/build

for t in lib64:x86_64-linux-android33 lib:i686-linux-android33; do
    lib=${t%%:*}
    mkdir -p "$B/$lib/angle"
    for l in EGL GLESv1_CM GLESv2; do
        [ -s "$B/$lib/angle/lib${l}_angle.so" ] ||
            adb pull -q "/vendor/$lib/egl/lib${l}_angle.so" "$B/$lib/angle/"
    done
    cc="$TC/${t#*:}-clang -shared -fPIC -O2 -Wall -Wl,--no-as-needed -L$B/$lib/angle"
    $cc -o "$B/$lib/libEGL_gpuid.so" -Wl,-soname,libEGL_gpuid.so -l:libEGL_angle.so
    $cc -o "$B/$lib/libGLESv1_CM_gpuid.so" -Wl,-soname,libGLESv1_CM_gpuid.so -l:libGLESv1_CM_angle.so
    $cc -o "$B/$lib/libGLESv2_gpuid.so" -Wl,-soname,libGLESv2_gpuid.so "$HERE/gpuid.c" \
        -l:libGLESv2_angle.so -ldl
done
ls -l "$B"/lib64/*_gpuid.so "$B"/lib/*_gpuid.so
