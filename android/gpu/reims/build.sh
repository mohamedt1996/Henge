#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Graphics: reims. Builds reims-vgpu's C library for the tablet (aarch64-linux-android, on its
# Vulkan platform over metal2vulkan) into android/gpu/reims/out/{lib/libreims_vgpu.a,include/reims_vgpu.h},
# which android/gpu/replay/CMakeLists.txt links into libhengegpu.so when it is there.
#
#   android/gpu/reims/build.sh            needs git, rustup, python3 and an NDK
#                                         (ANDROID_NDK_LATEST_HOME, ANDROID_NDK_HOME or ANDROID_NDK_ROOT)
#
# Both projects are fetched at the commits below and ported by android_port.py: Rust's target_os for
# Android is "android", where the projects build for "linux" only.
set -eu

REIMS_REPO=https://github.com/steelbrain/reims-vgpu
REIMS_REV=1fe0acb68b287db36f1e6c7b9f45383940b8a76f
M2V_REPO=https://github.com/steelbrain/metal2vulkan
M2V_REV=219f01365dc4c8eb346816c405b5b17403793764 # the commit reims-vgpu's Cargo.toml names
API=29                                           # the app's minSdk
TARGET=aarch64-linux-android

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${REIMS_WORK:-$HERE/work}
OUT=$HERE/out
NDK=${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "reims: no Android NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }

# One commit, without the parts the library's build never reads (metal2vulkan's validation corpus
# is 1.6 GB; reims-vgpu's suite and QEMU).
fetch() { # repo rev dir sparse-paths...
    repo=$1 rev=$2 dir=$3; shift 3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$rev" ]; then
        git -C "$dir" checkout -q -- . && git -C "$dir" clean -qfd
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$repo"
    git -C "$dir" sparse-checkout set --no-cone "$@"
    git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$rev"
    git -C "$dir" checkout -q FETCH_HEAD
}
mkdir -p "$WORK"
fetch "$REIMS_REPO" "$REIMS_REV" "$WORK/reims-vgpu" '/*' '!/tests/suite/' '!/vendor/' '!/assets/'
fetch "$M2V_REPO" "$M2V_REV" "$WORK/metal2vulkan" '/*' '!/crates/metal2vulkan-validation/' '!/crates/metal2vulkan-conformance/'

python3 "$HERE/android_port.py" "$WORK/reims-vgpu" "$WORK/metal2vulkan"

# The runtime from the ported checkout, not from its repository.
cat >> "$WORK/reims-vgpu/Cargo.toml" <<EOF

# Henge's Android build: metal2vulkan ported by android_port.py.
[patch."$M2V_REPO"]
metal2vulkan = { path = "$WORK/metal2vulkan/crates/metal2vulkan" }
metal2vulkan-runtime = { path = "$WORK/metal2vulkan/crates/metal2vulkan-runtime" }
metal2vulkan-air = { path = "$WORK/metal2vulkan/crates/metal2vulkan-air" }
EOF
# metal2vulkan's workspace lists members this checkout left out: only the three crates stay.
python3 - "$WORK/metal2vulkan/Cargo.toml" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'members\s*=\s*\[[^\]]*\]', 'members = ["crates/metal2vulkan", "crates/metal2vulkan-runtime", "crates/metal2vulkan-air"]', s, count=1)
open(p, 'w').write(s)
EOF

TOOLCHAIN=$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$WORK/reims-vgpu/rust-toolchain.toml")
rustup toolchain install "$TOOLCHAIN" --profile minimal --target "$TARGET" --no-self-update
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export CC_aarch64_linux_android="$CLANG/$TARGET$API-clang"
export AR_aarch64_linux_android="$CLANG/llvm-ar"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$CC_aarch64_linux_android"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$WORK/target}"
# reims-vgpu's guest-memory atomics are LSE instructions (Apple's CPUs always have them); Android's
# default target is plain ARMv8.0. The tablet's cores are ARMv9: ARMv8.2 with LSE and RCpc is safe.
export CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS="-C target-feature=+v8.2a,+lse,+rcpc,+crc,+aes,+sha2"

cd "$WORK/reims-vgpu"
RUSTUP_TOOLCHAIN=$TOOLCHAIN cargo build --release --target "$TARGET" -p reims-vgpu-c --no-default-features --features vulkan

mkdir -p "$OUT/lib" "$OUT/include"
cp "$CARGO_TARGET_DIR/$TARGET/release/libreims_vgpu_c.a" "$OUT/lib/libreims_vgpu.a" # the crate's name; xtask dist renames it the same way
cp crates/reims-vgpu-c/include/reims_vgpu.h "$OUT/include/"
echo "reims: $OUT/lib/libreims_vgpu.a"
