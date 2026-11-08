#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Translates every function of a shader capture (hengevm-android --gpu capture) to SPIR-V and
# reports which ones compile, which air2spv refuses, and why.
#   translate_capture.sh SHADER_DIR OUT
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOL=${AIR2SPV:-$ROOT/build/air2spv/air2spv}
[[ $# -eq 2 ]] || { echo "usage: $0 SHADER_DIR OUT"; exit 1; }
SRC=$1; OUT=$2
mkdir -p "$OUT/bc" "$OUT/spv"
python3 "$ROOT/android/tools/metallib_inventory.py" "$SRC" --extract "$OUT/bc" > "$OUT/inventory.txt"
ok=0; bad=0; : > "$OUT/refused.txt"
for bc in "$OUT"/bc/*.bc; do
  n=$(basename "$bc" .bc)
  if "$TOOL" --json "$OUT/spv/$n.json" -o "$OUT/spv/$n.spv" "$bc" 2> "$OUT/spv/$n.log" &&
     { ! command -v spirv-val >/dev/null || spirv-val --target-env vulkan1.2 "$OUT/spv/$n.spv" >> "$OUT/spv/$n.log" 2>&1; }; then
    ok=$((ok + 1))
  else
    bad=$((bad + 1)); printf '%s\t%s\n' "$n" "$(tail -1 "$OUT/spv/$n.log")" >> "$OUT/refused.txt"
  fi
done
echo "translated $ok, refused $bad (see $OUT/refused.txt; inventory in $OUT/inventory.txt)"
