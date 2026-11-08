/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The link between the engine (a static glibc process) and the app's renderer (bionic, in the app's
   process, where Android's Vulkan driver can be loaded). One file, mapped shared by both:

     engine: pvg_lite.c's commands, as the guest sent them  ──ring──▶  app: replay (android/gpu/replay)
     engine ◀── done: the sequence number the app has finished ──── app

   The guest's memory is a second file (HV_RAM_FILE, kept while linked), mapped by both; commands refer
   to it by guest physical address and by the GPU tasks' page tables, whose roots the engine publishes
   here. The engine completes a command's stamp only once the app says it is done, or at once while no
   app is attached (heartbeat older than HV_LINK_STALE_NS), so macOS keeps running without a screen. */
#ifndef HV_GPU_LINK_H
#define HV_GPU_LINK_H

#include <stdint.h>

#define HV_LINK_MAGIC 0x4b4e4c48u /* "HLNK" */
#define HV_LINK_VERSION 1
#define HV_LINK_TASKS 512
#define HV_LINK_RING_BYTES (16u << 20)
#define HV_LINK_STALE_NS 2000000000ull

/* A record in the ring: header, then the command as the guest wrote it (cmd[0] op | skip << 16,
   cmd[1], cmd[2] stamp, then barriers and arguments). Records are 8-byte aligned; one that would not
   fit before the end of the ring is preceded by a wrap record (kind HV_REC_WRAP) filling the rest. */
enum { HV_REC_COMMAND = 1, HV_REC_WRAP = 2 };
typedef struct {
    uint32_t bytes;   /* the whole record, header included, rounded up to 8 */
    uint32_t kind;
    uint32_t channel;
    uint32_t length;  /* of the command */
    uint64_t seq;     /* 1, 2, 3, ... */
} HvLinkRecord;

typedef struct {
    uint32_t magic, version;
    uint32_t header_bytes, ring_bytes;
    /* Guest RAM: [ram_pa, ram_pa + ram_size) is ram_file from offset 0. */
    uint64_t ram_pa, ram_size;
    char ram_file[512];
    /* The engine's state the replay needs: task roots (16 KB page numbers), object list offsets, and
       the surface mapper's table (guest physical). Written before each record that may use them. */
    uint32_t task_root[HV_LINK_TASKS];
    uint64_t task_objects[HV_LINK_TASKS];
    uint64_t surface_table;
    uint64_t display_page; /* guest physical: the display's shared page (pointer position at +0xe00) */
    uint32_t display_w, display_h;
    /* Ring positions, in bytes written / consumed since the start (monotonic). */
    uint64_t head;      /* engine: written */
    uint64_t tail;      /* app: consumed */
    uint64_t done;      /* app: highest seq finished */
    uint64_t heartbeat; /* app: CLOCK_MONOTONIC ns, every loop */
    uint32_t engine_alive, app_pid;
    uint64_t pad[8];
} HvLinkHeader;

#define HV_LINK_HEADER_BYTES 16384u /* the ring starts here */
#define HV_LINK_FILE_BYTES (HV_LINK_HEADER_BYTES + HV_LINK_RING_BYTES)

static inline uint8_t *hv_link_ring(HvLinkHeader *h) { return (uint8_t *)h + HV_LINK_HEADER_BYTES; }

#endif
