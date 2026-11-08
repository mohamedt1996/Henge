#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Assembles each test .ll (with head.inc) to AIR bitcode, compiles it with air2spv and validates the
# SPIR-V for Vulkan 1.2 (spirv-val). usage: [RUN=qemu-aarch64] run.sh LLVM_DIR AIR2SPV
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LLVM=$1; TOOL=$2
OUT=$(dirname "$TOOL")/tests; mkdir -p "$OUT"
fail=0
for t in "$HERE"/*.ll; do
  n=$(basename "$t" .ll)
  cat "$HERE/head.inc" "$t" > "$OUT/$n.ll"
  if ! "$LLVM/bin/llvm-as" "$OUT/$n.ll" -o "$OUT/$n.bc"; then echo "FAIL $n (llvm-as)"; fail=1; continue; fi
  if ! ${RUN:-} "$TOOL" --json "$OUT/$n.json" -o "$OUT/$n.spv" "$OUT/$n.bc" 2> "$OUT/$n.log"; then echo "FAIL $n: $(tail -1 "$OUT/$n.log")"; fail=1; continue; fi
  if ! spirv-val --target-env vulkan1.2 "$OUT/$n.spv" > "$OUT/$n.val" 2>&1; then echo "FAIL $n (spirv-val): $(head -3 "$OUT/$n.val")"; fail=1; continue; fi
  echo "ok   $n ($(stat -c %s "$OUT/$n.spv") bytes)"
done
exit $fail
