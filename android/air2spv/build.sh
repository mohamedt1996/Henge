#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds air2spv (host tool + libair2spv.a) against LLVM 22 and a private Mesa build of NIR.
#
#   android/air2spv/build.sh deps   # fetch LLVM 22 and Mesa at air2ir3's pinned revision, build Mesa's NIR
#   android/air2spv/build.sh        # build build/air2spv/air2spv and libair2spv.a
#   android/air2spv/build.sh test   # compile the test shaders and run spirv-val on the output
#
# Environment: AIR2SPV_DEPS (default ~/air2spv-deps), LLVM_DIR (default $AIR2SPV_DEPS/llvm),
# MESA_URL (default the freedesktop.org repository; any mirror with the pinned commit works).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEPS=${AIR2SPV_DEPS:-$HOME/air2spv-deps}
LLVM=${LLVM_DIR:-$DEPS/llvm}
M=$DEPS/mesa
B=$DEPS/mesa-build
OUT=$ROOT/build/air2spv
A2I=$ROOT/third_party/air2ir3
MESA_REV=bbeb2dca9feed6ac88d71234a9dc5ae8aeed4275       # air2ir3's pinned Mesa
LLVM_VER=22.1.0

deps() {
  mkdir -p "$DEPS"
  if [[ ! -x $LLVM/bin/llvm-config ]]; then
    local arch; arch=$(uname -m); [[ $arch == aarch64 ]] && arch=ARM64 || arch=X64
    curl -fL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-$arch.tar.xz" | tar xJ -C "$DEPS"
    mv "$DEPS/LLVM-$LLVM_VER-Linux-$arch" "$LLVM"
  fi
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
  if [[ ! -f $B/build.ninja ]]; then
    meson setup "$B" "$M" -Dbuildtype=release -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Dopengl=true \
      -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
      -Dtools=freedreno -Dfreedreno-kmds=msm -Dvideo-codecs= -Dzstd=enabled --wrap-mode=nodownload
  fi
  ninja -C "$B" src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a \
    src/util/libmesa_util_simd.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a \
    src/freedreno/registers/adreno/a8xx_descriptors.xml.h src/freedreno/registers/adreno/a8xx_enums.xml.h \
    src/freedreno/registers/adreno/a6xx_enums.xml.h src/freedreno/registers/adreno/adreno_common.xml.h
}

build() {
  [[ -f $B/build.ninja && -x $LLVM/bin/llvm-config ]] || { echo "run '$0 deps' first"; exit 1; }
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
  local CC="$LLVM/bin/clang" CXX="$LLVM/bin/clang++"
  local LLVMF; LLVMF=$("$LLVM/bin/llvm-config" --cxxflags | sed 's/-std=c++[0-9]*//; s/-fno-exceptions//; s/-fno-rtti//')
  local LLVML; LLVML=$("$LLVM/bin/llvm-config" --link-static --ldflags --libs core bitreader analysis transformutils passes ipo scalaropts instcombine)
  local SYSL; SYSL=$("$LLVM/bin/llvm-config" --link-static --system-libs | sed 's/-lxml2//')
  set -x
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$A2I/src/a8xx_sampler.c" -o "$OUT/a8xx_sampler.o"
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$M/src/gallium/drivers/zink/nir_to_spirv/nir_to_spirv.c" -o "$OUT/nir_to_spirv.o"
  "$CC" -std=gnu11 -O2 -fPIC "${INC[@]}" "${DEFS[@]}" -c "$M/src/gallium/drivers/zink/nir_to_spirv/spirv_builder.c" -o "$OUT/spirv_builder.o"
  "$CXX" -std=c++17 -O2 -fPIC -Wno-deprecated-declarations -Wno-c99-designator -Wno-unused-function $LLVMF "${INC[@]}" "${DEFS[@]}" \
    -DAIR2SPV_NO_MAIN -c "$HERE/air2spv.cpp" -o "$OUT/air2spv_lib.o"
  "$CXX" -std=c++17 -O2 -Wno-deprecated-declarations -Wno-c99-designator -Wno-unused-function $LLVMF "${INC[@]}" "${DEFS[@]}" \
    -c "$HERE/air2spv.cpp" -o "$OUT/air2spv_main.o"
  rm -f "$OUT/libair2spv.a"
  "$LLVM/bin/llvm-ar" rcs "$OUT/libair2spv.a" "$OUT/air2spv_lib.o" "$OUT/a8xx_sampler.o" "$OUT/nir_to_spirv.o" "$OUT/spirv_builder.o"
  local MESAL=("$B/src/compiler/nir/libnir.a" "$B/src/compiler/libcompiler.a" "$B/src/util/libmesa_util.a"
    "$B/src/util/libmesa_util_simd.a" "$B/src/c11/impl/libmesa_util_c11.a" "$B/src/util/blake3/libblake3.a")
  "$CXX" -o "$OUT/air2spv" "$OUT/air2spv_main.o" "$OUT/a8xx_sampler.o" "$OUT/nir_to_spirv.o" "$OUT/spirv_builder.o" \
    "${MESAL[@]}" $LLVML $SYSL -lzstd -lz -lpthread -ldl -lm
  set +x
  echo "built $OUT/air2spv and $OUT/libair2spv.a"
}

runtests() {
  "$HERE/tests/run.sh" "$LLVM" "$OUT/air2spv"
}

case "${1:-}" in
  deps) deps ;;
  test) runtests ;;
  "") build ;;
  *) echo "usage: $0 [deps|test]"; exit 1 ;;
esac
