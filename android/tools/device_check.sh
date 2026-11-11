#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# What this tablet offers Henge, read over adb (root used where it helps):
#   android/tools/device_check.sh
set -u
sh() { adb shell "$@" 2>/dev/null | tr -d '\r'; }
su_sh() { adb shell su -c "\"$*\"" 2>/dev/null | tr -d '\r'; }
echo "device:      $(sh getprop ro.product.model) ($(sh getprop ro.product.device)), Android $(sh getprop ro.build.version.release)"
echo "chip:        $(sh getprop ro.soc.model) / $(sh getprop ro.board.platform)"
echo "kernel:      $(sh uname -r)"
echo "root:        $(su_sh id -u | grep -q '^0$' && echo yes || echo no)"
for d in /dev/kvm /dev/gunyah /dev/gzvm; do
  printf '%-12s %s\n' "$d:" "$(su_sh ls -l $d || sh ls -l $d || echo absent)"
done
echo "Vulkan:      $(sh getprop ro.hardware.vulkan) driver, $(sh dumpsys SurfaceFlinger | grep -m1 -i 'vulkan' )"
echo "page size:   $(sh getconf PAGE_SIZE)"
echo "huge pages:  $(su_sh cat /sys/kernel/mm/transparent_hugepage/enabled || echo unknown)"
echo "SELinux:     $(sh getenforce)"
echo "big cores:   $(for c in $(sh ls /sys/devices/system/cpu/ | grep -E '^cpu[0-9]+$'); do printf '%s=%s ' $c $(sh cat /sys/devices/system/cpu/$c/cpufreq/cpuinfo_max_freq); done)"
# /dev/udmabuf: dma-bufs of memfd pages, which reims-vgpu's views and a no-copy import of the Mac's memory need.
echo "udmabuf:     $(su_sh ls -lZ /dev/udmabuf || echo absent)"
echo "  config:    $(su_sh 'zcat /proc/config.gz | grep -i UDMABUF' | tr '\n' ' ')"
echo "  module:    $(su_sh "find /vendor_dlkm /system_dlkm /vendor/lib/modules /system/lib/modules /odm/lib/modules -name '*udmabuf*'" | head -3 | tr '\n' ' ')"
echo "  loaded:    $(su_sh ls -d /sys/module/udmabuf /sys/class/misc/udmabuf | tr '\n' ' ')"
