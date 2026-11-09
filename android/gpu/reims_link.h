/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims. The paravirtual GPU's device itself runs in the app, on reims-vgpu
   (github.com/steelbrain/reims-vgpu): the host side of Apple's ParavirtualizedGraphics protocol, over
   metal2vulkan's Metal on Vulkan, on the tablet's Adreno. The engine (a static glibc process, which
   cannot load Android's Vulkan driver) keeps only the bus: it forwards each guest access to the GPU's
   two MMIO windows (vmapple's gfx at 0x200000000 and IOSurface mapper) through this file, and raises
   the interrupts the app posts back. Guest RAM is the RAM file both map (HV_RAM_FILE, kept).

     engine (vCPU thread)                         app (libhengegpu.so)
       MMIO read/write ── slot: state 1 ──▶  slot's thread: rvg_device_mmio_* / rvg_iosurface_device_mmio_*
       waits (spin, then futex)  ◀── state 2 ──  value
       irq thread  ◀── irq_bits, irq_seq (futex) ──  raise_interrupt callbacks

   One slot per engine thread that touches the GPU (each vCPU, the device thread), so vCPUs never wait
   for each other here; reims takes its own locks as QEMU's MMIO calls do. Futexes are shared (not
   FUTEX_PRIVATE): both processes map the file MAP_SHARED. */
#ifndef HV_REIMS_LINK_H
#define HV_REIMS_LINK_H

#include <stdint.h>

#define HV_REIMS_MAGIC 0x534d5248u /* "HRMS" */
#define HV_REIMS_VERSION 1
#define HV_REIMS_SLOTS 32
#define HV_REIMS_STALE_NS 3000000000ull
#define HV_REIMS_FILE_BYTES 16384u

enum { HV_REIMS_IDLE = 0, HV_REIMS_REQUEST = 1, HV_REIMS_REPLY = 2 };
enum { HV_REIMS_GFX = 0, HV_REIMS_IOSFC = 1 };
enum { HV_REIMS_IRQ_GFX = 1u, HV_REIMS_IRQ_IOSFC = 2u };

typedef struct {
    uint32_t state;   /* HV_REIMS_*: the futex word */
    uint32_t device;  /* HV_REIMS_GFX or HV_REIMS_IOSFC */
    uint32_t write;   /* 1 write, 0 read */
    uint32_t size;    /* bytes: 4 or 8 */
    uint64_t offset;  /* in the device's window */
    uint64_t value;   /* the write's value, or the read's answer */
    uint64_t pad[4];
} HvReimsSlot; /* 64 bytes: a cache line each */

typedef struct {
    uint32_t magic, version;
    /* Guest RAM: [ram_pa, ram_pa + ram_size) is ram_file from offset 0. */
    uint64_t ram_pa, ram_size;
    char ram_file[512];
    uint32_t display_w, display_h; /* the mode macOS is offered first */
    uint32_t engine_alive;
    uint32_t app_ready;            /* the app's device exists: MMIO is answered */
    uint64_t app_heartbeat;        /* CLOCK_MONOTONIC ns */
    uint32_t app_pid;
    uint32_t irq_seq;              /* futex word: bumped after irq_bits change */
    uint32_t irq_bits;             /* HV_REIMS_IRQ_*, taken by the engine */
    uint32_t pad0;
    uint64_t requests, irqs;       /* counts, for the log */
    uint64_t pad[8];
    HvReimsSlot slot[HV_REIMS_SLOTS];
} HvReimsHeader;

_Static_assert(sizeof(HvReimsSlot) == 64, "slot is a cache line");
_Static_assert(sizeof(HvReimsHeader) <= HV_REIMS_FILE_BYTES, "header fits the file");

#endif
