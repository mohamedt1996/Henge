/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A draw, end to end: a guest made by hand holds two Metal functions (air2spv's tests vs_quad and fs_tex,
   in .metallib containers), a pipeline whose description names them, a vertex buffer, two constant
   buffers, a 2x2 texture and a sampler; a render pass clears a 64x32 surface to blue and draws a
   textured quad over its middle. The surface in guest memory must then hold the quad, the right way up.

   usage: drawtest AIR2SPV VS.bc FS.bc CACHE_DIR      (selftest.sh runs it) */
#include "replay.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAM_PA 0x70000000ull
#define RAM_SIZE (64ull << 20)
#define PG(n) (RAM_PA + (uint64_t)(n) * GPAGE)
#define PN(n) ((uint32_t)(PG(n) >> 14))

static uint8_t *ram;
static uint8_t *at(uint64_t pa) { return ram + (pa - RAM_PA); }
static uint64_t task_pa(uint64_t off) { return PG(100 + off / GPAGE) + off % GPAGE; }
static void put(uint64_t off, const void *d, size_t n) { for (size_t i = 0; i < n; i++) *at(task_pa(off + i)) = ((const uint8_t *)d)[i]; }

static uint8_t *slurp(const char *p, uint32_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    *n = (uint32_t)sz;
    return d;
}

/* A .metallib with one function: header, function list {count, {size, NAME, TYPE, OFFT, MDSZ, ENDT}}, bitcode. */
static uint32_t metallib(uint8_t *out, const char *name, uint8_t type, const uint8_t *bc, uint32_t bclen) {
    uint8_t *p = out + 88;
    uint32_t count = 1;
    memcpy(p, &count, 4), p += 4;
    uint8_t *group = p;
    p += 4;
    uint16_t l;
#define TAG(t, v, n) (memcpy(p, t, 4), l = (uint16_t)(n), memcpy(p + 4, &l, 2), memcpy(p + 6, v, n), p += 6 + (n))
    TAG("NAME", name, strlen(name) + 1);
    TAG("TYPE", &type, 1);
    uint64_t offt[3] = {0, 0, 0}, size = bclen;
    TAG("OFFT", offt, 24);
    TAG("MDSZ", &size, 8);
    memcpy(p, "ENDT", 4), p += 4;
    uint32_t gs = (uint32_t)(p - group);
    memcpy(group, &gs, 4);
    uint64_t fl = 88, bco = (uint64_t)(p - out), bcs = bclen;
    memset(out, 0, 88);
    memcpy(out, "MTLB", 4);
    memcpy(out + 24, &fl, 8), memcpy(out + 72, &bco, 8), memcpy(out + 80, &bcs, 8);
    memcpy(p, bc, bclen);
    return (uint32_t)(bco + bclen);
}

static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t off) {
    uint32_t e[3] = {type | size << 8, (uint32_t)off, (uint32_t)(off >> 32)};
    put(0x10000 + ref * 12ull, e, 12);
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: drawtest AIR2SPV VS.bc FS.bc CACHE_DIR\n"); return 2; }
    uint32_t vsn, fsn;
    uint8_t *vsbc = slurp(argv[2], &vsn), *fsbc = slurp(argv[3], &fsn);
    if (!vsbc || !fsbc) { fprintf(stderr, "no bitcode\n"); return 2; }
    char path[] = "/tmp/henge-draw-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, RAM_SIZE)) return 1;
    unlink(path);
    ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    HvLinkHeader *link = calloc(1, sizeof *link);
    Guest g = {link, fd, ram, RAM_PA, RAM_SIZE};

    /* Task 1: 64 pages (task page v is RAM page 100 + v); object list at 64 KB. */
    link->task_root[1] = PN(1);
    uint32_t root[2] = {PN(2), 1};
    memcpy(at(PG(1)), root, 8);
    for (uint32_t v = 0; v < 64; v++) ((uint32_t *)at(PG(2)))[v] = PN(100 + v);
    link->task_objects[1] = 0x10000;

    /* The render target: a 64x32 BGRA surface, mapping 1, on RAM page 20. */
    const uint32_t W = 64, H = 32, ROW = 256;
    link->surface_table = PG(3);
    *(uint32_t *)at(PG(3)) = (uint32_t)(PG(4) >> 12) | 1;
    uint8_t *d = at(PG(4) + 0x400);
    *(uint32_t *)at(PG(5)) = (uint32_t)(PG(20) >> 12) | 1;
    uint32_t list = (uint32_t)(PG(5) >> 12) | 1, fourcc = 'B' << 24 | 'G' << 16 | 'R' << 8 | 'A', first = 0;
    uint64_t alloc = (uint64_t)ROW * H, w64 = W, h64 = H, row64 = ROW;
    memcpy(d, &list, 4), memcpy(d + 4, &fourcc, 4), memcpy(d + 0x18, &alloc, 8), memcpy(d + 0x20, &w64, 8);
    memcpy(d + 0x28, &h64, 8), memcpy(d + 0x30, &row64, 8), memcpy(d + 0x60, &first, 4);
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t desc[24]; } mt = {1, 0x14, 24, 1, {0}};
    mt.desc[0] = 2, mt.desc[2] = 80;
    memcpy(mt.desc + 4, &W, 4), memcpy(mt.desc + 8, &H, 4);
    mt.desc[12] = 1, mt.desc[16] = 1, mt.desc[18] = 1, mt.desc[20] = 1;
    put(0x20000, &mt, sizeof mt);
    entry(1, OBJ_MAPPED_TEXTURE, sizeof mt, 0x20000);

    /* Buffers {length, page, flags}: 2 positions (task page 32), 3 scale (33), 4 tint (34), 5 texels (35). */
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage, flags; } b = {GPAGE, 0, 0};
    for (uint32_t ref = 2; ref <= 5; ref++) {
        b.vpage = 30 + ref;
        put(0x20100 + ref * 16, &b, sizeof b);
        entry(ref, OBJ_BUFFER, 16, 0x20100 + ref * 16);
    }
    float pos[8] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f}, scale[2] = {1, 1}, tint[4] = {1, 1, 1, 1};
    put(32 * GPAGE, pos, sizeof pos), put(33 * GPAGE, scale, sizeof scale), put(34 * GPAGE, tint, sizeof tint);
    /* 2x2 BGRA texels, rows 16 bytes apart: row 0 red, row 1 green. */
    uint8_t texels[32] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 0, 255, 0, 255};
    put(35 * GPAGE, texels, sizeof texels);
    /* 6: a texture over buffer 5 {tag 9, size, ref, buffer, offset, bytes per row, description} */
    struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t desc[24]; } lt = {9, 0, 6, 5, 0, 16, {0}};
    lt.size = sizeof lt;
    uint32_t two = 2;
    lt.desc[0] = 2, lt.desc[2] = 80;
    memcpy(lt.desc + 4, &two, 4), memcpy(lt.desc + 8, &two, 4);
    lt.desc[12] = 1, lt.desc[16] = 1, lt.desc[18] = 1, lt.desc[20] = 1;
    put(0x20300, &lt, sizeof lt);
    entry(6, OBJ_LINEAR_TEXTURE, sizeof lt, 0x20300);
    /* 7: a sampler {tag 3, size, ref, bits, lod min, lod max}: nearest, clamp to edge, normalized coordinates */
    struct __attribute__((packed)) { uint32_t tag, size, ref; uint64_t bits; float lod[2]; } sm = {3, sizeof sm, 7, 1ull << 31, {0, 0}};
    put(0x20400, &sm, sizeof sm);
    entry(7, 7, sizeof sm, 0x20400);
    /* 8, 9: the functions {offset, length, tag, size} of their .metallib */
    static uint8_t lib[65536];
    uint32_t n = metallib(lib, "vs_quad", 0, vsbc, vsn);
    put(0x40000, lib, n);
    struct __attribute__((packed)) { uint64_t off; uint32_t len, tag, size; } fn = {0x40000, n, 0, 0};
    put(0x20500, &fn, sizeof fn);
    entry(8, OBJ_FUNCTION, sizeof fn, 0x20500);
    n = metallib(lib, "fs_tex", 1, fsbc, fsn);
    put(0x50000, lib, n);
    fn.off = 0x50000, fn.len = n;
    put(0x20600, &fn, sizeof fn);
    entry(9, OBJ_FUNCTION, sizeof fn, 0x20600);
    /* 10: the pipeline {tag 0xe, size, ref, length, description}; the description names functions 8 and 9 */
    uint8_t pipe[64] = {0};
    uint32_t ph[4] = {0xe, sizeof pipe, 10, sizeof pipe - 16}, f8 = 8, f9 = 9;
    memcpy(pipe, ph, 16);
    memcpy(pipe + 24, &f8, 4), memcpy(pipe + 40, &f9, 4);
    put(0x20700, pipe, sizeof pipe);
    entry(10, 7, sizeof pipe, 0x20700);

    /* The commands: one render segment. */
    uint8_t cb[4096] = {0};
    uint32_t p = 8, seg_type = 0;
#define REC(tag, ...) do { uint8_t body[] = {__VA_ARGS__}; uint32_t h2[2] = {tag, 8 + (uint32_t)sizeof body}; memcpy(cb + p, h2, 8); memcpy(cb + p + 8, body, sizeof body); p += 8 + sizeof body; } while (0)
    /* 26: the pass, colour 0 = the surface, cleared to blue and stored */
    uint32_t pass_size = 8 + 0x248, rec26[2] = {26, pass_size};
    memcpy(cb + p, rec26, 8);
    uint8_t *pass = cb + p + 8;
    uint32_t ref1 = 1;
    uint16_t clear = 2, store = 1;
    double blue[4] = {0, 0, 1, 1};
    uint64_t rw = W, rh = H;
    memcpy(pass + 0x4c, &ref1, 4), memcpy(pass + 0x4c + 20, &clear, 2), memcpy(pass + 0x4c + 22, &store, 2), memcpy(pass + 0x4c + 0x1c, blue, 32);
    memcpy(pass + 0x238, &rw, 8), memcpy(pass + 0x240, &rh, 8);
    p += pass_size;
    REC(116, 10, 0, 0, 0);                                                   /* pipeline 10 */
    REC(125, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,     /* vertex buffers 0, 1: refs 2, 3 */
        3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    REC(110, 0, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);   /* fragment buffer 0: ref 4 */
    REC(114, 0, 0, 0, 0, 1, 0, 0, 0, 6, 0, 0, 0);                            /* fragment texture 0: ref 6 */
    REC(112, 0, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0);                            /* fragment sampler 0: ref 7 */
    REC(1, 4, 0, 0, 0, 0, 0, 4, 0);                                          /* triangle strip, 4 vertices */
    uint32_t seg[2] = {p - 8, seg_type};
    memcpy(cb, seg, 8);
    put(0x30000, cb, p);

    mkdir(argv[4], 0700);
    Replay *r = replay_for_test(&g, NULL);
    replay_enable_draws(r, argv[1], argv[4]);
    uint32_t ex[3 + 3 + 4] = {0x43, 0, 7, 1, 0, 1};
    uint64_t b0[2] = {0x30000, p};
    memcpy(ex + 6, b0, 16);
    replay_command(r, 0, ex, sizeof ex);

    const uint8_t *px = at(PG(20));
    struct { uint32_t x, y; uint8_t b, g, r; const char *what; } checks[] = {
        {2, 2, 255, 0, 0, "outside the quad: the clear (blue)"},
        {60, 28, 255, 0, 0, "outside the quad, far corner: blue"},
        {32, 10, 0, 255, 0, "top of the quad: texture row 1 (green), so +y is up"},
        {32, 21, 0, 0, 255, "bottom of the quad: texture row 0 (red)"},
        {17, 12, 0, 255, 0, "left edge inside"},
        {14, 12, 255, 0, 0, "left edge outside"},
    };
    int fail = 0;
    if (getenv("DRAWTEST_DUMP"))
        for (uint32_t y = 0; y < H; y += 2) {
            for (uint32_t x = 0; x < W; x += 2) { const uint8_t *q = px + y * ROW + x * 4; putchar(q[2] > 128 ? 'R' : q[1] > 128 ? 'G' : q[0] > 128 ? '.' : ' '); }
            putchar('\n');
        }
    for (unsigned i = 0; i < sizeof checks / sizeof *checks; i++) {
        const uint8_t *q = px + checks[i].y * ROW + checks[i].x * 4;
        int ok = abs(q[0] - checks[i].b) < 8 && abs(q[1] - checks[i].g) < 8 && abs(q[2] - checks[i].r) < 8;
        printf("%s %s: (%u,%u) BGR %u %u %u\n", ok ? "ok  " : "FAIL", checks[i].what, checks[i].x, checks[i].y, q[0], q[1], q[2]);
        fail |= !ok;
    }
    replay_stop(r);
    return fail;
}
