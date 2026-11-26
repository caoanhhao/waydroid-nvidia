#!/bin/sh
# Build the patched libgbm_mesa_wrapper.so (gralloc.gbm.linear support, 1D R8 import) with the
# Android NDK, for both ABIs: 32-bit processes (the camera provider) map buffers through
# /vendor/lib. Links against the vendor image's own libgbm_mesa.so; libc++ is linked statically.
# Output: src/minigbm-wrapper/out/libgbm_mesa_wrapper.so (x86_64),
#         src/minigbm-wrapper/out/lib/libgbm_mesa_wrapper.so (x86)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:-/opt/android-ndk}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
VENDOR=${VENDOR:-/var/lib/waydroid/rootfs/vendor}
OUT=$HERE/out

# upstream gbm_mesa_wrapper (waydroid minigbm lineage-18.1) + venus/patches/gralloc-*
if [ ! -d "$HERE/minigbm" ]; then
	git clone --branch lineage-18.1 https://github.com/waydroid/android_external_minigbm "$HERE/minigbm"
	git -C "$HERE/minigbm" switch -c gralloc-nvidia e5a7769a4892cd1b6c61c56a8bedfc9b93584704
	git -C "$HERE/minigbm" am "$HERE"/../../venus/patches/gralloc-*.patch
fi

for abi in x86_64:lib64:. i686:lib:lib; do
	triple=${abi%%:*}
	rest=${abi#*:}
	vendor_lib=$VENDOR/${rest%%:*}
	out=$OUT/${rest#*:}
	mkdir -p "$out/stubs"
	# Link-time copy of the vendor libgbm_mesa.so (only its dynamic symbols are used).
	cp "$vendor_lib/libgbm_mesa.so" "$out/stubs/libgbm_mesa.so"

	"$BIN/$triple-linux-android33-clang++" -shared -fPIC -O2 -std=c++17 -Wall -Wno-c99-designator -Wno-missing-field-initializers \
		-fno-exceptions -fvisibility=default \
		-I"$HERE/include" -I"$HERE/minigbm" \
		"$HERE/minigbm/gbm_mesa_driver/gbm_mesa_wrapper.cpp" \
		-Wl,-soname,libgbm_mesa_wrapper.so -Wl,--exclude-libs,ALL -Wl,--version-script,"$HERE/exports.map" -Wl,--no-undefined -Wl,-z,defs \
		-L"$out/stubs" -lgbm_mesa -llog -static-libstdc++ \
		-o "$out/libgbm_mesa_wrapper.so"
	"$BIN/llvm-strip" --strip-unneeded "$out/libgbm_mesa_wrapper.so"
	echo "built $out/libgbm_mesa_wrapper.so"
done
