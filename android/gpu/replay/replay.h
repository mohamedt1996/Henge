/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The app side of the GPU link (../link.h): reads the commands macOS's GPU driver sends, carries out
   what it can, and shows the screen through Vulkan.

   Stage one, this code: everything runs on the CPU, straight in guest memory, so the guest and the
   screen always see the same bytes. That covers what needs no shaders: render-pass clears, blits,
   object queries, the display's swaps and the pointer. Draws and compute dispatches are counted and
   skipped; they need the serialized-pipeline reader and Vulkan pipelines built from air2spv's
   SPIR-V. Shader libraries are already translated as they arrive (shaders.c), so the shader cache
   fills while macOS runs. */
#ifndef HV_REPLAY_H
#define HV_REPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include "../link.h"

#ifdef __ANDROID__
#include <android/log.h>
#define rlog(...) __android_log_print(ANDROID_LOG_INFO, "henge-gpu", __VA_ARGS__)
#else
#define rlog(...) (fprintf(stderr, "henge-gpu: " __VA_ARGS__), fputc('\n', stderr))
#endif

#define GPAGE 0x4000ull /* the guest's page */

/* ------------------------------------------------------------------ guest memory (guest.c) -- */
typedef struct {
    HvLinkHeader *link;
    int ram_fd;
    uint8_t *ram;
    uint64_t ram_pa, ram_size;
} Guest;

int guest_open(Guest *g, const char *link_path);
void guest_close(Guest *g);
void *guest_phys(Guest *g, uint64_t pa, uint64_t len);
/* A task's 16 KB page number for a task offset, 0 if unmapped. */
uint32_t guest_task_page(Guest *g, uint32_t task, uint64_t off);
void guest_task_pages(Guest *g, uint32_t task, uint64_t first, uint64_t n, uint32_t *out);
int guest_task_read(Guest *g, uint32_t task, uint64_t off, void *to, uint32_t n);
/* Pages (guest page numbers, 0 = hole) mapped contiguously from the RAM file; unmap with guest_unmap. */
uint8_t *guest_map_pages(Guest *g, const uint32_t *pages, uint64_t n);
void guest_unmap(uint8_t *p, uint64_t n);
/* The surface mapper's 1 KB descriptor of a mapping id. */
const uint8_t *guest_surface_desc(Guest *g, uint32_t mapping);

/* --------------------------------------------------------------------- objects (objects.c) -- */
enum { OBJ_BUFFER = 1, OBJ_TEXTURE = 2, OBJ_FUNCTION = 6, OBJ_SERIALIZED = 7, OBJ_LINEAR_TEXTURE = 8,
       OBJ_LINEAR_TEXTURE2 = 9, OBJ_MAPPED_TEXTURE = 0xb, OBJ_HEAP = 13, OBJ_HEAP_BUFFER = 14, OBJ_BUFFER27 = 19 };

typedef struct Surface {
    uint32_t mapping;
    uint32_t w, h, fourcc;
    uint64_t row, first, alloc, length;
    uint32_t *pages; /* guest page numbers it is on */
    uint8_t *mem;    /* those pages, contiguous */
    struct Surface *next;
} Surface;

/* A texture as the CPU sees it: level m of slice s starts at *mem + level_off[m] + s * slice_stride.
   mem points at the owner's pointer, so a view follows its storage when the guest moves it. */
typedef struct {
    uint16_t format;      /* MTLPixelFormat */
    uint16_t type;        /* MTLTextureType */
    uint32_t w, h, depth, mips, slices, samples;
    uint8_t **mem;
    uint64_t limit;       /* bytes valid from *mem */
    uint64_t level_off[16], level_row[16], level_img[16];
    uint64_t slice_stride;
} TexView;

typedef struct Obj {
    uint32_t task, ref, refs;
    uint32_t entry[3];
    uint32_t type;
    /* memory a buffer, heap or texture has in the task's address space, mapped contiguously */
    uint32_t vpage;
    uint64_t length;
    uint32_t *pages;
    uint8_t *mem;
    int mem_owned;
    uint8_t *buffer_base; /* a buffer inside a heap: where it starts */
    uint8_t *host;        /* a texture with no guest memory: its texels, here */
    int has_tex;
    TexView tex;
    Surface *surface;
    struct Obj *parent;
    uint64_t unique_id;
    /* a function: its stage (0 vertex, 1 fragment, 2 kernel, -1 unknown) and AIR bitcode */
    int fn_stage;
    uint8_t *fn_bc;
    uint32_t fn_len;
    uint64_t fn_hash;
    char fn_name[96];
    /* a serialized object (pipeline, sampler, depth-stencil state): {tag, size, ref, ...} as sent */
    uint32_t ser_tag;
    uint8_t *ser;
    uint32_t ser_len;
    /* a render pipeline: the functions found in its description (draw.c) */
    int scanned;
    uint32_t vs_ref, fs_ref, fn_candidates;
    struct Obj *next; /* hash chain */
} Obj;

typedef struct Objects {
    Guest *g;
    Obj *table[4096];
    Surface *surfaces;
    uint64_t next_id;
    void (*on_function)(void *ctx, uint32_t task, const uint8_t *lib, uint32_t len);
    void *ctx;
} Objects;

Obj *obj_get(Objects *os, uint32_t task, uint32_t ref);
void obj_forget(Objects *os, uint32_t task, uint32_t ref);
void obj_forget_task(Objects *os, uint32_t task);
/* Re-reads a buffer's or texture's pages if the guest moved them. */
void obj_refresh(Objects *os, Obj *o);
Surface *surface_get(Objects *os, uint32_t mapping);
/* The type of a ref's object (OBJ_*), from the task's object list, without making the object. */
uint32_t obj_type(Objects *os, uint32_t task, uint32_t ref);
/* A buffer's bytes (a heap buffer's, inside its heap). */
uint8_t *obj_bytes(Obj *o, uint64_t *len);

/* ----------------------------------------------------------------- pixels (pixels.c) -- */
/* Bytes per texel of a MTLPixelFormat, 0 if this code cannot address it per texel. */
unsigned px_bytes(uint32_t mtl_format);
/* Encodes an RGBA clear colour in the format: returns the byte count, 0 if not supported. */
unsigned px_encode(uint32_t mtl_format, const double rgba[4], uint8_t out[16]);
/* One row of the scanout to RGBA8 (R first), for the screen. */
void px_row_to_rgba8(uint32_t fourcc, const uint8_t *src, uint8_t *dst, uint32_t w);

/* ------------------------------------------------------------ screen (screen_vk.c) -- */
typedef struct Screen Screen;
Screen *screen_create(void *native_window); /* NULL window: off-screen (tests) */
void screen_destroy(Screen *s);
/* A frame, RGBA8, w x h, tightly packed. Copies it; presents now. Returns 0 if shown. */
int screen_show(Screen *s, const uint8_t *rgba, uint32_t w, uint32_t h);
/* Tests: the last frame as the GPU drew it (RGBA8, swapchain-sized). */
int screen_readback(Screen *s, uint8_t **rgba, uint32_t *w, uint32_t *h);

/* --------------------------------------------------------------- shaders (shaders.c) -- */
typedef struct Shaders Shaders;
Shaders *shaders_create(const char *air2spv, const char *cache_dir);
void shaders_destroy(Shaders *s);
/* A .metallib the guest uploaded: each function translated once, in the background. */
void shaders_library(Shaders *s, const uint8_t *lib, uint32_t len);
void shaders_stats(Shaders *s, uint64_t out[3]); /* translated, refused, pending */
/* The first function of a .metallib: its bitcode (pointing into lib), stage (0 vertex, 1 fragment, 2 kernel,
   -1 other) and name. Returns 0 if there is one. */
int metallib_first(const uint8_t *lib, uint32_t n, const uint8_t **bc, uint32_t *len, int *stage, char name[96]);
uint64_t shaders_hash(const void *d, size_t n);
/* A function's SPIR-V and layout (air2spv's --layout text), translating it now if it is not in the cache.
   Returns 0 and malloc'd copies, or -1 if air2spv refused it or is not there. */
int shaders_get(Shaders *s, const uint8_t *bc, uint32_t len, uint8_t **spv, size_t *spv_len, char **layout, uint8_t **cdata, size_t *clen);

/* ------------------------------------------------------------------ draws (draw.c) -- */
/* Render passes with draws run on the GPU: the attachments and everything the draws read are copied in
   from guest memory, and what the pass stores is copied back, so guest memory stays the truth. */
typedef struct Gpu Gpu;
Gpu *gpu_create(Objects *os, Shaders *shaders);
void gpu_destroy(Gpu *g);
/* A render encoder's records: 26 begins a pass, the rest set state and draw. Returns 1 if the record was taken. */
void gpu_pass_begin(Gpu *g, uint32_t task, const uint8_t *rec, uint32_t size);
int gpu_record(Gpu *g, uint32_t tag, const uint8_t *rec, uint32_t size);
/* Ends the open pass: runs it if it drew, else returns 0 so the caller clears on the CPU. */
int gpu_pass_end(Gpu *g);
void gpu_stats(Gpu *g, uint64_t out[4]); /* passes on the GPU, draws done, draws skipped, pipelines */

/* ---------------------------------------------------------------- the replay (replay.c) -- */
typedef struct Replay Replay;
Replay *replay_start(const char *link_path, void *native_window, const char *air2spv, const char *cache_dir);
void replay_set_window(Replay *r, void *native_window);
void replay_stop(Replay *r);
/* One command, as the ring carries it; returns when it is done. Exposed for tests. */
void replay_command(Replay *r, uint32_t channel, const uint32_t *cmd, uint32_t len);
/* Tests: build a replay over an open guest without the ring thread. */
Replay *replay_for_test(Guest *g, Screen *screen);
void replay_present_now(Replay *r);
void replay_enable_draws(Replay *r, const char *air2spv, const char *cache_dir);

#endif
