#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds air2spv (host tool + libair2spv.a) against LLVM 22 and a private Mesa build of NIR.
#
#   android/air2spv/build.sh deps [arm64]   # fetch LLVM 22 and Mesa at air2ir3's pinned revision, build Mesa's NIR
#   android/air2spv/build.sh [arm64]        # build build/air2spv[-arm64]/air2spv and libair2spv.a
#   android/air2spv/build.sh test [arm64]   # compile the test shaders and run spirv-val on the output
#
# arm64 is the tablet's build: a static arm64 Linux program, like the engine, that the app ships as
# libair2spv.so and runs on the device (android/gpu/replay/shaders.c). It is cross-compiled with
# aarch64-linux-gnu-gcc against LLVM's arm64 release; its tests run under qemu-aarch64. The host
# build (LLVM's x86-64 release) is still needed: its llvm-config and llvm-as drive both.
#
# Environment: AIR2SPV_DEPS (default ~/air2spv-deps), LLVM_DIR (default $AIR2SPV_DEPS/llvm),
# MESA_URL (default the freedesktop.org repository; any mirror with the pinned commit works).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEPS=${AIR2SPV_DEPS:-$HOME/air2spv-deps}
LLVM=${LLVM_DIR:-$DEPS/llvm}
M=$DEPS/mesa
ARCH=host
[[ ${2:-${1:-}} == arm64 ]] && ARCH=arm64
if [[ $ARCH == arm64 ]]; then
  B=$DEPS/mesa-build-arm64
  TLLVM=$DEPS/llvm-arm64
  OUT=$ROOT/build/air2spv-arm64
else
  B=$DEPS/mesa-build
  TLLVM=$LLVM
  OUT=$ROOT/build/air2spv
fi
A2I=$ROOT/third_party/air2ir3
MESA_REV=bbeb2dca9feed6ac88d71234a9dc5ae8aeed4275       # air2ir3's pinned Mesa
LLVM_VER=22.1.0

fetch_llvm() { # $1: X64 or ARM64, $2: where. Only what air2spv needs (the release unpacks to 12 GB).
  [[ -d $2/lib ]] && return
  local top="LLVM-$LLVM_VER-Linux-$1"
  curl -fL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$top.tar.xz" |
    tar xJ -C "$DEPS" --wildcards "$top/bin/llvm-config" "$top/bin/llvm-as" "$top/include/llvm/*" "$top/include/llvm-c/*" "$top/lib/libLLVM*.a"
  mv "$DEPS/$top" "$2"
}

deps() {
  mkdir -p "$DEPS"
  [[ -x $LLVM/bin/llvm-config ]] || fetch_llvm "$( [[ $(uname -m) == aarch64 ]] && echo ARM64 || echo X64 )" "$LLVM"
  [[ $ARCH == arm64 ]] && fetch_llvm ARM64 "$TLLVM"
  if [[ ! -d $M/.git ]]; then
    git init -q "$M"
    git -C "$M" remote add origin "${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}"
    git -C "$M" fetch -q --depth 1 origin $MESA_REV
    git -C "$M" checkout -q FETCH_HEAD
  fi
  [[ $(git -C "$M" rev-parse HEAD) == "$MESA_REV" ]] || { echo "$M is not at $MESA_REV"; exit 1; }
  # Only NIR, the util libraries and the freedreno register headers are needed. computerator is
  # skipped: it needs ir3's assembler parser, which nothing here builds.
  sed -i 's/^if system_has_kms_drm$/if system_has_kms_drm and false/' "$M/src/freedreno/meson.build"
  # With no driver configured Mesa builds NIR as a stub; build all of it (air2ir3's Mesa patch 0000 does
  # the same).
  grep -q '^with_nir_headers_only = false' "$M/src/compiler/nir/meson.build" ||
    sed -i '/^  \].contains(true))$/a with_nir_headers_only = false' "$M/src/compiler/nir/meson.build"
  grep -q '^with_nir_headers_only = false' "$M/src/compiler/nir/meson.build" || { echo "could not enable NIR in $M"; exit 1; }
  local opts=(-Dbuildtype=release -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Dopengl=true
    -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled
    -Dtools=freedreno -Dfreedreno-kmds=msm -Dvideo-codecs= --wrap-mode=nodownload)
  if [[ ! -f $B/build.ninja ]]; then
    if [[ $ARCH == arm64 ]]; then
      cat > "$DEPS/aarch64.cross" <<'CROSS'
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'false'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
CROSS
      meson setup "$B" "$M" --cross-file "$DEPS/aarch64.cross" "${opts[@]}" -Dzstd=disabled -Dzlib=disabled \
        -Dshader-cache=disabled -Dexpat=disabled -Dxmlconfig=disabled
    else
      meson setup "$B" "$M" "${opts[@]}" -Dzstd=enabled
    fi
  fi
  ninja -C "$B" src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a \
    src/util/libmesa_util_simd.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a \
    src/freedreno/registers/adreno/a8xx_descriptors.xml.h src/freedreno/registers/adreno/a8xx_enums.xml.h \
    src/freedreno/registers/adreno/a6xx_enums.xml.h src/freedreno/registers/adreno/adreno_common.xml.h
}

build() {
  [[ -f $B/build.ninja && -x $LLVM/bin/llvm-config && -d $TLLVM/lib ]] || { echo "run '$0 deps${ARCH/host/}' first"; exit 1; }
  mkdir -p "$OUT"
  local INC=(-I"$HERE" -I"$A2I/src" -I"$M/src/gallium/drivers/zink"
    -I"$B/src/compiler/nir" -I"$M/src/compiler/nir" -I"$B/src/compiler" -I"$M/src/compiler"
    -I"$B/src/util" -I"$M/src/util" -I"$B/src" -I"$M/src" -I"$M/include" -I"$B/include"
    -I"$M/src/gallium/include" -I"$M/src/gallium/auxiliary" -I"$M/src/mapi" -I"$M/src/mesa"
    -I"$B/src/freedreno/registers/adreno" -I"$B/src/freedreno/registers")
  local DEFS=(-DHAVE_PTHREAD -DUSE_GCC_ATOMIC_BUILTINS -DHAVE___BUILTIN_CLZ -DHAVE___BUILTIN_CLZLL
    -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL -DHAVE___BUILTIN_POPCOUNT
    -DHAVE___BUILTIN_POPCOUNTLL -DHAVE___BUILTIN_UNREACHABLE -DHAVE___BUILTIN_EXPECT
    -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_UNUSED -DHAVE_FUNC_ATTRIBUTE_NORETURN
    -DHAVE_STRUCT_TIMESPEC -DMESA_DEBUG=0 -DNDEBUG -D_GNU_SOURCE
    -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_ENDIAN_H -DAIR2SPV_MESA_RELEASE)
  local CC=clang CXX=clang++ AR=ar STATIC=() SYSX=(-lzstd -lz)
  [[ -x $LLVM/bin/clang ]] && CC="$LLVM/bin/clang" CXX="$LLVM/bin/clang++" AR="$LLVM/bin/llvm-ar"
  # The flags of the host's llvm-config, pointed at the target's LLVM (same release, same libraries).
  local LI LL; LI=$("$LLVM/bin/llvm-config" --includedir) LL=$("$LLVM/bin/llvm-config" --libdir)
  local LLVMF; LLVMF=$("$LLVM/bin/llvm-config" --cxxflags | sed "s/-std=c++[0-9]*//; s/-fno-exceptions//; s/-fno-rtti//; s#$LI#$TLLVM/include#g")
  local LLVML; LLVML=$("$LLVM/bin/llvm-config" --link-static --ldflags --libs core bitreader analysis transformutils passes ipo scalaropts instcombine | sed "s#$LL#$TLLVM/lib#g")
  local SYSL; SYSL=$("$LLVM/bin/llvm-config" --link-static --system-libs | sed 's/-lxml2//; s#[^ ]*libzstd.a##; s/-lz\b//')
  if [[ $ARCH == arm64 ]]; then
    CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar
    STATIC=(-static -static-libstdc++ -static-libgcc) SYSX=("$OUT/nocompress.o")
  fi
  set -x
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$A2I/src/a8xx_sampler.c" -o "$OUT/a8xx_sampler.o"
  [[ $ARCH == arm64 ]] && "$CC" -std=gnu11 -O2 -c "$HERE/nocompress.c" -o "$OUT/nocompress.o"
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$M/src/gallium/drivers/zink/nir_to_spirv/nir_to_spirv.c" -o "$OUT/nir_to_spirv.o"
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$M/src/gallium/drivers/zink/nir_to_spirv/spirv_builder.c" -o "$OUT/spirv_builder.o"
  local W=(-Wno-deprecated-declarations -Wno-unused-function -Wno-unknown-warning-option -Wno-c99-designator)
  [[ $ARCH == arm64 ]] && W=(-Wno-deprecated-declarations -Wno-unused-function -Wno-missing-field-initializers)
  "$CXX" -std=c++17 -O2 -fPIC "${W[@]}" $LLVMF "${INC[@]}" "${DEFS[@]}" -DAIR2SPV_NO_MAIN -c "$HERE/air2spv.cpp" -o "$OUT/air2spv_lib.o"
  "$CXX" -std=c++17 -O2 "${W[@]}" $LLVMF "${INC[@]}" "${DEFS[@]}" -c "$HERE/air2spv.cpp" -o "$OUT/air2spv_main.o"
  rm -f "$OUT/libair2spv.a"
  "$AR" rcs "$OUT/libair2spv.a" "$OUT/air2spv_lib.o" "$OUT/a8xx_sampler.o" "$OUT/nir_to_spirv.o" "$OUT/spirv_builder.o"
  local MESAL=("$B/src/compiler/nir/libnir.a" "$B/src/compiler/libcompiler.a" "$B/src/util/libmesa_util.a"
    "$B/src/util/libmesa_util_simd.a" "$B/src/c11/impl/libmesa_util_c11.a" "$B/src/util/blake3/libblake3.a")
  "$CXX" "${STATIC[@]}" -o "$OUT/air2spv" "$OUT/air2spv_main.o" "$OUT/a8xx_sampler.o" "$OUT/nir_to_spirv.o" "$OUT/spirv_builder.o" \
    "${MESAL[@]}" $LLVML $SYSL "${SYSX[@]}" -lpthread -ldl -lm
  set +x
  # As for the engine: syscalls Android's seccomp filter kills (set_robust_list, rseq, clone3) return ENOSYS.
  [[ $ARCH == arm64 ]] && python3 "$ROOT/android/nosyscall.py" "$OUT/air2spv"
  echo "built $OUT/air2spv and $OUT/libair2spv.a"
}

runtests() {
  local run=()
  [[ $ARCH == arm64 && $(uname -m) != aarch64 ]] && run=(qemu-aarch64)
  RUN="${run[*]}" "$HERE/tests/run.sh" "$LLVM" "$OUT/air2spv"
}

case "${1:-}" in
  deps) deps ;;
  test) runtests ;;
  ""|arm64) build ;;
  *) echo "usage: $0 [deps|test] [arm64]"; exit 1 ;;
esac
