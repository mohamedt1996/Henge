#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the renderer for Linux and runs selftest.c on whatever Vulkan driver is installed (lavapipe
# in CI: apt install mesa-vulkan-drivers libvulkan-dev).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$HERE/../../.." && pwd)/build/gpu
mkdir -p "$OUT"
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -D_GNU_SOURCE \
  "$HERE"/guest.c "$HERE"/objects.c "$HERE"/pixels.c "$HERE"/screen_vk.c "$HERE"/shaders.c "$HERE"/replay.c "$HERE"/selftest.c \
  -o "$OUT/selftest" -lvulkan -lpthread -lm
if [[ -z ${VK_ICD_FILENAMES:-} ]]; then
  for icd in /usr/share/vulkan/icd.d/lvp_icd*.json /opt/lavapipe/lvp_icd.json; do
    [[ -f $icd ]] && export VK_ICD_FILENAMES=$icd && break
  done
fi
"$OUT/selftest"
# The engine's side of the link (android/pvg_android.c) against this one.
ROOT=$(cd "$HERE/../../.." && pwd)
cc -std=gnu11 -O1 -g -D_GNU_SOURCE -w -I"$ROOT/android/compat" -I"$ROOT/android" -I"$ROOT/core" -I"$ROOT/host" -I"$ROOT/devices" -I"$HERE" \
  "$HERE"/linktest.c "$ROOT"/android/pvg_android.c "$HERE"/guest.c "$HERE"/objects.c "$HERE"/pixels.c "$HERE"/screen_vk.c \
  "$HERE"/shaders.c "$HERE"/replay.c -o "$OUT/linktest" -lvulkan -lpthread -lm
"$OUT/linktest"
