#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Graphics: reims, Vulkan driver: Turnip. Builds Henge's own Turnip (Mesa's open Adreno Vulkan driver, for
# Android's KGSL) into OUT/libvulkan_freedreno.so: the same source the bundled prebuilt comes from, with one
# change, kgsl_coherent_import.py: dma-bufs imported I/O-coherent where the app asks, which is what lets
# the GPU use the Mac's memory in place (Zero-copy) without reading stale pages.
#
#   android/gpu/turnip/build.sh OUT        needs git, python3, meson, ninja, mako, pyyaml, glslangValidator,
#                                          bison, flex and an NDK (ANDROID_NDK_LATEST_HOME, ANDROID_NDK_HOME
#                                          or ANDROID_NDK_ROOT)
#
# The recipe is whitebelyash's (freedreno_turnip-CI's turnip_builder.sh, whose release the APK bundles as
# the stock driver), at a pinned commit of the same branch.
set -eu

MESA_REPO=https://github.com/whitebelyash/mesa-unified
MESA_REV=9c7e022677dfa3abb356c2b6732cbd2e25783d01 # turnip/gen8, 2026-08-07 (Mesa 26.3.0-devel)
SDK=34                                            # as the stock build's compiler

OUT=${1:?usage: build.sh OUT}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${TURNIP_WORK:-$HERE/work}
NDK=${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "turnip: no Android NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -x "$BIN/aarch64-linux-android$SDK-clang" ] || { echo "turnip: $BIN has no aarch64-linux-android$SDK-clang" >&2; exit 1; }
for tool in git python3 meson ninja glslangValidator bison flex; do
    command -v "$tool" >/dev/null 2>&1 || { echo "turnip: $tool is missing" >&2; exit 1; }
done

mkdir -p "$WORK" "$OUT"
SRC=$WORK/mesa
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" = "$MESA_REV" ]; then
    git -C "$SRC" checkout -q -- . && git -C "$SRC" clean -qfd -e build-android-aarch64
else
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$MESA_REPO"
    git -C "$SRC" fetch -q --depth 1 origin "$MESA_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

python3 "$HERE/kgsl_coherent_import.py" "$SRC"
# The device's name says which build it is ("Turnip Adreno (TM) 7xx (henge-coherent-imports)").
echo '#define TUGEN8_DRV_VERSION "henge-coherent-imports"' > "$SRC/src/freedreno/vulkan/tu_version.h"

cd "$SRC"
cat > android-aarch64.txt <<CROSS
[binaries]
ar = '$BIN/llvm-ar'
c = ['$BIN/aarch64-linux-android$SDK-clang']
cpp = ['$BIN/aarch64-linux-android$SDK-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = '$BIN/ld.lld'
cpp_ld = '$BIN/ld.lld'
strip = '$BIN/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$BIN/pkg-config', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
CROSS

rm -rf "$WORK/prefix"
meson setup build-android-aarch64 \
    --cross-file android-aarch64.txt \
    --prefix "$WORK/prefix" \
    -Dbuildtype=release \
    -Dstrip=true \
    -Dplatforms=android \
    -Dvideo-codecs= \
    -Dplatform-sdk-version=36 \
    -Dandroid-stub=true \
    -Dgallium-drivers= \
    -Dvulkan-drivers=freedreno \
    -Dvulkan-beta=true \
    -Dfreedreno-kmds=kgsl \
    -Degl=disabled \
    -Dandroid-libbacktrace=disabled \
    --reconfigure
ninja -C build-android-aarch64 install

SO=$WORK/prefix/lib/libvulkan_freedreno.so
[ -s "$SO" ] || { echo "turnip: the build left no libvulkan_freedreno.so" >&2; exit 1; }
# What the app looks for before it lets the GPU use the Mac's pages in place (android/gpu/reims/host.c).
grep -q -a TU_KGSL_IMPORT_COHERENT "$SO" || { echo "turnip: the library does not carry TU_KGSL_IMPORT_COHERENT" >&2; exit 1; }
cp "$SO" "$OUT/libvulkan_freedreno.so"
echo "turnip: $OUT/libvulkan_freedreno.so ($(stat -c %s "$OUT/libvulkan_freedreno.so") bytes, Mesa $(cat VERSION) at $MESA_REV)"
