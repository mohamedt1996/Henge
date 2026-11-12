#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-build libapplegfx-vulkan (MIT) as a static arm64 library for the tablet, like reims/build.sh. Best
# effort: on any failure the engine is built without it and Graphics: applegfx logs that it is not present.
#
#   android/applegfx/build.sh OUTDIR     # writes OUTDIR/libapplegfx-vulkan.a and OUTDIR/include/
#
# The engine is a static aarch64-linux-gnu program, so the library is built with the same toolchain and Vulkan
# off (the library makes Vulkan optional): a render-stubbed static lib that links into the engine. Driving the
# tablet's own Vulkan (Turnip) is the transport-bridge work noted in android/pvg_applegfx.c.
set -euo pipefail
OUT=${1:?usage: build.sh OUTDIR}
REPO=${APPLEGFX_URL:-https://github.com/MattJackson/libapplegfx-vulkan}
REV=${APPLEGFX_REV:-7574eac}                 # pin; bump deliberately
DEPS=${APPLEGFX_DEPS:-$HOME/applegfx-deps}
SRC=$DEPS/src
mkdir -p "$OUT" "$DEPS"
command -v meson >/dev/null || pip install --user -q meson ninja
if [ ! -d "$SRC/.git" ]; then
  git init -q "$SRC"
  git -C "$SRC" remote add origin "$REPO"
fi
git -C "$SRC" fetch -q --depth 1 origin "$REV"
git -C "$SRC" checkout -q FETCH_HEAD
# Henge-local patches on top of the pinned upstream rev. These add the
# lagfx_iosurface_register/unregister entry points the MMIO-transport bridge
# needs (upstream only accepts IOSurfaces over the in-band PCI command ring).
# Idempotent: skip a patch that is already applied (re-run / warm $DEPS cache).
PATCHDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/patches"
if [ -d "$PATCHDIR" ]; then
  for p in "$PATCHDIR"/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$SRC" apply --check -p1 "$p" 2>/dev/null; then
      git -C "$SRC" apply -p1 "$p"
      echo "applegfx: applied patch $(basename "$p")"
    elif git -C "$SRC" apply --reverse --check -p1 "$p" 2>/dev/null; then
      echo "applegfx: patch $(basename "$p") already applied, skipping"
    else
      echo "applegfx: WARNING patch $(basename "$p") does not apply to $REV — skipping" >&2
    fi
  done
fi
# A Meson cross file for the engine's toolchain. Vulkan is left off (dependency('vulkan', required:false)).
CROSS=$DEPS/aarch64.cross
cat > "$CROSS" <<CROSSEOF
[binaries]
c = 'aarch64-linux-gnu-gcc'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSSEOF
rm -rf "$SRC/bd-arm64"
meson setup "$SRC/bd-arm64" "$SRC" --cross-file "$CROSS" -Dbuildtype=release -Ddefault_library=static
meson compile -C "$SRC/bd-arm64"
cp "$SRC/bd-arm64"/libapplegfx-vulkan.a "$OUT/" 2>/dev/null || cp "$SRC/bd-arm64"/*applegfx*vulkan*.a "$OUT/libapplegfx-vulkan.a"
mkdir -p "$OUT/include"
cp -r "$SRC/include/." "$OUT/include/"
echo "applegfx: built $(du -h "$OUT/libapplegfx-vulkan.a" | cut -f1) static lib for arm64"
