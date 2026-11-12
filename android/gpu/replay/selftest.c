/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A guest made by hand (one GPU task, a surface, a buffer), commands as macOS sends them, and the
   screen read back from Vulkan: a render-pass clear, a buffer-to-texture blit and a swap must show.
   Runs on Linux with any Vulkan driver (lavapipe in CI):  build.sh test */
#include "replay.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define RAM_PA 0x70000000ull
#define RAM_SIZE (64ull << 20)
#define PG(n) (RAM_PA + (uint64_t)(n) * GPAGE) /* guest physical address of RAM page n */
#define PN(n) ((uint32_t)(PG(n) >> 14))        /* its 16 KB page number */

static uint8_t *ram;
static uint8_t *at(uint64_t pa) { return ram + (pa - RAM_PA); }

/* Task 1's memory: task page v is RAM page 100 + v. */
static uint64_t task_pa(uint64_t off) { return PG(100 + off / GPAGE) + off % GPAGE; }
static void task_put(uint64_t off, const void *d, size_t n) { for (size_t i = 0; i < n; i++) *at(task_pa(off + i)) = ((const uint8_t *)d)[i]; }

int main(void) {
    char path[] = "/tmp/henge-gpu-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, RAM_SIZE)) return 1;
    unlink(path);
    ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    HvLinkHeader *link = calloc(1, sizeof *link);
    Guest g = {link, fd, ram, RAM_PA, RAM_SIZE};

    /* Task 1: root (page 1) = {table page, depth 1}; the table (page 2) maps 64 task pages. */
    link->task_root[1] = PN(1);
    uint32_t root[2] = {PN(2), 1};
    memcpy(at(PG(1)), root, 8);
    for (uint32_t v = 0; v < 64; v++) ((uint32_t *)at(PG(2)))[v] = PN(100 + v);
    link->task_objects[1] = 0x10000; /* object list at task offset 64 KB, 12 bytes a ref */

    /* A 64x32 BGRA surface, mapping 1: its pages are RAM pages 20.. */
    const uint32_t W = 64, H = 32, ROW = 256;
    link->surface_table = PG(3);
    *(uint32_t *)at(PG(3)) = (uint32_t)(PG(4) >> 12) | 1; /* descriptors of mappings 0..15 on page 4 */
    uint8_t *d = at(PG(4) + 1 * 0x400);
    *(uint32_t *)at(PG(5)) = (uint32_t)(PG(20) >> 12) | 1; /* page list: one 16 KB page */
    uint32_t list = (uint32_t)(PG(5) >> 12) | 1, fourcc = 'B' << 24 | 'G' << 16 | 'R' << 8 | 'A', first = 0;
    uint64_t alloc = (uint64_t)ROW * H, w64 = W, h64 = H, row64 = ROW;
    memcpy(d, &list, 4), memcpy(d + 4, &fourcc, 4), memcpy(d + 0x18, &alloc, 8), memcpy(d + 0x20, &w64, 8);
    memcpy(d + 0x28, &h64, 8), memcpy(d + 0x30, &row64, 8), memcpy(d + 0x60, &first, 4);

    /* ref 1: the surface as a texture {mapping, tag, size, ref, v1 description}; ref 2: a buffer of 16 KB */
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t desc[24]; } mt = {1, 0x14, 24, 1, {0}};
    mt.desc[0] = 2, mt.desc[2] = 80; /* 2D, BGRA8Unorm */
    memcpy(mt.desc + 4, &W, 4), memcpy(mt.desc + 8, &H, 4);
    mt.desc[12] = 1, mt.desc[16] = 1, mt.desc[18] = 1, mt.desc[20] = 1;
    task_put(0x20000, &mt, sizeof mt);
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage, flags; } buf = {GPAGE, 10, 0};
    task_put(0x20100, &buf, sizeof buf);
    uint32_t e1[3] = {OBJ_MAPPED_TEXTURE | (uint32_t)sizeof mt << 8, 0x20000, 0}, e2[3] = {OBJ_BUFFER | 16 << 8, 0x20100, 0};
    task_put(0x10000 + 12, e1, 12), task_put(0x10000 + 24, e2, 12);
    for (uint32_t i = 0; i < 16 * 16; i++) { uint32_t green = 0xff00ff00u; task_put(10 * GPAGE + i * 4, &green, 4); } /* the buffer's texels */

    /* The command buffer at task offset 0x30000: a render segment clearing ref 1 to red, then a blit
       segment copying a 16x16 green square from ref 2 to (8, 8) of ref 1. */
    uint8_t cb[4096] = {0};
    uint32_t p = 0;
    uint32_t pass_size = 8 + 0x248;
    uint32_t seg0[2] = {pass_size, 0}, rec0[2] = {26, pass_size};
    memcpy(cb + p, seg0, 8), p += 8;
    memcpy(cb + p, rec0, 8);
    uint8_t *pass = cb + p + 8;
    uint32_t ref1 = 1;
    uint16_t clear = 2, store = 1;
    double red[4] = {1, 0, 0, 1};
    memcpy(pass + 0x4c, &ref1, 4), memcpy(pass + 0x4c + 20, &clear, 2), memcpy(pass + 0x4c + 22, &store, 2), memcpy(pass + 0x4c + 0x1c, red, 32);
    p += pass_size;
    uint32_t blit_size = 8 + 0x58;
    uint32_t seg1[2] = {blit_size, 2}, rec1[2] = {300, blit_size};
    uint32_t p1 = p;
    memcpy(cb + p, seg1, 8), p += 8;
    memcpy(cb + p, rec1, 8);
    uint8_t *bl = cb + p + 8;
    uint32_t src = 2, dst = 1;
    uint64_t soff = 0, srow = 64, simg = 0, sz[3] = {16, 16, 1}, org[3] = {8, 8, 0};
    memcpy(bl, &src, 4), memcpy(bl + 4, &dst, 4), memcpy(bl + 8, &soff, 8), memcpy(bl + 0x10, &srow, 8), memcpy(bl + 0x18, &simg, 8);
    memcpy(bl + 0x20, sz, 24), memcpy(bl + 0x38, org, 24);
    p += blit_size;
    task_put(0x30000, cb, p);

    Screen *screen = screen_create(NULL);
    if (!screen) { fprintf(stderr, "no Vulkan\n"); return 1; }
    Replay *r = replay_for_test(&g, screen);
    /* exec: {op 0x43, 0, stamp} {task 1, 0 resources, 2 buffers} {offset, length} x 2 */
    uint32_t ex[3 + 3 + 8] = {0x43, 0, 7, 1, 0, 2};
    uint64_t b0[2] = {0x30000, p1}, b1[2] = {0x30000 + p1, p - p1};
    memcpy(ex + 6, b0, 16), memcpy(ex + 10, b1, 16);
    replay_command(r, 0, ex, sizeof ex);
    uint32_t swap[3 + 10] = {8, 0, 8, 0, 0, 1, 0};
    replay_command(r, 0, swap, sizeof swap);
    replay_present_now(r);

    uint8_t *px;
    uint32_t sw, shh;
    int fail = 0;
    if (screen_readback(screen, &px, &sw, &shh)) { fprintf(stderr, "no readback\n"); return 1; }
    /* The 64x32 frame is letterboxed into 1280x800: scale 20 horizontally, 25 vertically -> 20. */
    double s = (double)sw / W < (double)shh / H ? (double)sw / W : (double)shh / H;
    uint32_t ox = (uint32_t)((sw - W * s) / 2), oy = (uint32_t)((shh - H * s) / 2);
    struct { uint32_t x, y; uint8_t r, g, b; const char *what; } checks[] = {
        {2, 2, 255, 0, 0, "cleared red"}, {60, 28, 255, 0, 0, "cleared red (far corner)"},
        {16, 16, 0, 255, 0, "green square from the blit"}, {30, 10, 255, 0, 0, "red right of the square"},
    };
    for (unsigned i = 0; i < sizeof checks / sizeof *checks; i++) {
        uint32_t x = ox + (uint32_t)((checks[i].x + 0.5) * s), y = oy + (uint32_t)((checks[i].y + 0.5) * s);
        const uint8_t *q = px + ((size_t)y * sw + x) * 4;
        int ok = abs(q[0] - checks[i].r) < 8 && abs(q[1] - checks[i].g) < 8 && abs(q[2] - checks[i].b) < 8;
        printf("%s %s: screen %u,%u = %u %u %u\n", ok ? "ok  " : "FAIL", checks[i].what, x, y, q[0], q[1], q[2]);
        fail |= !ok;
    }
    const uint8_t *letterbox = px + ((size_t)(oy / 2) * sw + sw / 2) * 4;
    printf("%s black letterbox: %u %u %u\n", oy == 0 || (letterbox[0] | letterbox[1] | letterbox[2]) == 0 ? "ok  " : "FAIL", letterbox[0], letterbox[1], letterbox[2]);
    free(px);
    replay_stop(r);
    screen_destroy(screen);

    /* The same exec through the ring, as the engine sends it (android/pvg_android.c link_send). */
    memset(at(PG(20)), 0, GPAGE);
    char lpath[] = "/tmp/henge-gpu-link-XXXXXX", rpath[] = "/tmp/henge-gpu-ram-XXXXXX";
    int lfd = mkstemp(lpath), rfd = mkstemp(rpath);
    if (lfd < 0 || rfd < 0 || ftruncate(lfd, HV_LINK_FILE_BYTES) || write(rfd, ram, RAM_SIZE) != (ssize_t)RAM_SIZE) return 1;
    HvLinkHeader *h = mmap(NULL, HV_LINK_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, lfd, 0);
    memcpy(h, link, sizeof *link);
    h->version = HV_LINK_VERSION, h->ram_pa = RAM_PA, h->ram_size = RAM_SIZE;
    snprintf(h->ram_file, sizeof h->ram_file, "%s", rpath);
    __atomic_store_n(&h->magic, HV_LINK_MAGIC, __ATOMIC_RELEASE);
    Replay *rr = replay_start(lpath, NULL, NULL, NULL);
    for (int i = 0; i < 400 && !__atomic_load_n(&h->heartbeat, __ATOMIC_ACQUIRE); i++) usleep(10000);
    HvLinkRecord rec = {(uint32_t)((sizeof rec + sizeof ex + 7) & ~7u), HV_REC_COMMAND, 0, sizeof ex, 1};
    memcpy(hv_link_ring(h), &rec, sizeof rec);
    memcpy(hv_link_ring(h) + sizeof rec, ex, sizeof ex);
    __atomic_store_n(&h->head, rec.bytes, __ATOMIC_RELEASE);
    for (int i = 0; i < 400 && __atomic_load_n(&h->done, __ATOMIC_ACQUIRE) < 1; i++) usleep(10000);
    uint8_t *ram2 = mmap(NULL, RAM_SIZE, PROT_READ, MAP_SHARED, rfd, 0);
    const uint8_t *px0 = ram2 + (PG(20) - RAM_PA) + 2 * ROW + 2 * 4, *px1 = ram2 + (PG(20) - RAM_PA) + 16 * ROW + 16 * 4;
    int ring_ok = h->done == 1 && h->tail == rec.bytes && px0[2] == 255 && px0[1] == 0 && px1[1] == 255 && px1[2] == 0;
    printf("%s ring: done %llu, tail %llu, texel (2,2) BGRA %u %u %u, (16,16) %u %u %u\n", ring_ok ? "ok  " : "FAIL",
           (unsigned long long)h->done, (unsigned long long)h->tail, px0[0], px0[1], px0[2], px1[0], px1[1], px1[2]);
    fail |= !ring_ok;
    replay_stop(rr);
    unlink(lpath);

    /* The screen at the frame's own scale (0.3.53): a window of 3200x2136 (the Mi Pad 7's) shows a 1280x800
       frame through a 1280x854 swapchain, the frame's pixels one to one between two black bars, written
       straight into the screen's memory. */
    {
        Screen *sc = screen_create_offscreen(3200, 2136);
        if (!sc) { fprintf(stderr, "no Vulkan\n"); return 1; }
        int fresh = -1;
        uint8_t *fp = screen_frame(sc, 1280, 800, &fresh);
        int ok = fp && fresh == 1;
        for (uint32_t y = 0; ok && y < 800; y++)
            for (uint32_t x = 0; x < 1280; x++) {
                uint8_t *q = fp + ((size_t)y * 1280 + x) * 4;
                q[0] = (uint8_t)x, q[1] = (uint8_t)y, q[2] = (uint8_t)(x ^ y), q[3] = 255;
            }
        ok = ok && screen_present(sc, 1280, 800) == 0;
        uint8_t *rb = NULL;
        uint32_t rw = 0, rh = 0;
        ok = ok && screen_readback(sc, &rb, &rw, &rh) == 0 && rw == 1280 && rh == 854;
        int exact = ok;
        for (uint32_t y = 0; exact && y < 800; y += 7)
            for (uint32_t x = 0; x < 1280; x += 5) {
                const uint8_t *q = rb + ((size_t)(y + 27) * rw + x) * 4;
                if (q[0] != (uint8_t)x || q[1] != (uint8_t)y || q[2] != (uint8_t)(x ^ y)) { exact = 0; break; }
            }
        int bars = ok;
        for (uint32_t x = 0; bars && x < 1280; x += 3) {
            const uint8_t *top = rb + ((size_t)26 * rw + x) * 4, *bottom = rb + ((size_t)827 * rw + x) * 4;
            if (top[0] | top[1] | top[2] | bottom[0] | bottom[1] | bottom[2]) bars = 0;
        }
        printf("%s frame's scale: a 1280x800 frame in a 3200x2136 window is a %ux%u image, its pixels %s, its bars %s\n",
               ok && exact && bars ? "ok  " : "FAIL", rw, rh, exact ? "one to one" : "WRONG", bars ? "black" : "NOT BLACK");
        fail |= !(ok && exact && bars);
        free(rb), rb = NULL;
        /* The same memory again: not fresh, and what was written is still there (a pointer is drawn over it). */
        fresh = -1;
        uint8_t *again = screen_frame(sc, 1280, 800, &fresh);
        ok = again == fp && fresh == 0 && again[4 * 1281 + 0] == 1 && again[4 * 1281 + 1] == 1;
        printf("%s frame's scale: the frame's memory is kept between frames\n", ok ? "ok  " : "FAIL");
        fail |= !ok;
        /* Another mode: the swapchain follows the frame. 1920x1080 in the same window: 1920x1282. */
        fp = screen_frame(sc, 1920, 1080, &fresh);
        ok = fp && fresh == 1;
        if (ok) memset(fp, 0x80, (size_t)1920 * 1080 * 4);
        ok = ok && screen_present(sc, 1920, 1080) == 0 && screen_readback(sc, &rb, &rw, &rh) == 0 && rw == 1920 && rh == 1282;
        ok = ok && rb[((size_t)101 * rw + 5) * 4] == 0x80 && rb[((size_t)100 * rw + 5) * 4] == 0 && rb[((size_t)1180 * rw + 1900) * 4] == 0x80 &&
             rb[((size_t)1181 * rw + 1900) * 4] == 0;
        printf("%s frame's scale: a 1920x1080 frame next is a %ux%u image\n", ok ? "ok  " : "FAIL", rw, rh);
        fail |= !ok;
        free(rb), rb = NULL;
        screen_destroy(sc);

        /* A window smaller than the frame: the window's size and the blit, as before. */
        sc = screen_create_offscreen(1000, 700);
        fp = sc ? screen_frame(sc, 1280, 800, NULL) : NULL;
        ok = fp != NULL;
        for (size_t i = 0; ok && i < (size_t)1280 * 800; i++) fp[i * 4] = 0, fp[i * 4 + 1] = 200, fp[i * 4 + 2] = 0, fp[i * 4 + 3] = 255;
        ok = ok && screen_present(sc, 1280, 800) == 0 && screen_readback(sc, &rb, &rw, &rh) == 0 && rw == 1000 && rh == 700;
        /* scale 0.78125: 1000x625, bars of 37 rows */
        ok = ok && rb[((size_t)350 * rw + 500) * 4 + 1] == 200 && rb[((size_t)10 * rw + 500) * 4 + 1] == 0;
        printf("%s a window smaller than the frame: scaled by the blit into %ux%u\n", ok ? "ok  " : "FAIL", rw, rh);
        fail |= !ok;
        free(rb), rb = NULL;

        /* Graphics: reims's frames through the replay, written in place: the first begin's pixels are new, the
           next still hold the frame, and the presenter shows them from the screen's memory. */
        Replay *er = replay_for_test(&g, sc);
        int kept = -1;
        uint8_t *ep = replay_frame_begin(er, 640, 400, &kept);
        ok = ep && kept == 0;
        if (ep) {
            for (size_t i = 0; i < (size_t)640 * 400; i++) ep[i * 4] = 250, ep[i * 4 + 1] = 0, ep[i * 4 + 2] = 0, ep[i * 4 + 3] = 255;
            replay_frame_end(er, 1);
        }
        replay_present_now(er);
        /* 640x400 fits the 1000x700 window: its own scale again, 640x448, bars of 24 rows */
        ok = ok && screen_readback(sc, &rb, &rw, &rh) == 0 && rw == 640 && rh == 448 && rb[((size_t)200 * rw + 300) * 4] == 250 &&
             rb[((size_t)10 * rw + 300) * 4] == 0;
        free(rb), rb = NULL;
        kept = -1;
        uint8_t *ep2 = replay_frame_begin(er, 640, 400, &kept);
        ok = ok && ep2 == ep && kept == 1 && ep2[0] == 250;
        if (ep2) replay_frame_end(er, 0);
        kept = -1;
        ep2 = replay_frame_begin(er, 800, 500, &kept);
        ok = ok && ep2 && kept == 0;
        if (ep2) replay_frame_end(er, 0);
        printf("%s reims frames are written in the screen's memory: new, then kept, new again at another size\n", ok ? "ok  " : "FAIL");
        fail |= !ok;
        replay_stop(er);
        screen_destroy(sc);

        /* With no screen the replay keeps them itself. */
        er = replay_for_test(&g, NULL);
        kept = -1;
        ep = replay_frame_begin(er, 64, 32, &kept);
        ok = ep && kept == 0;
        if (ep) ep[0] = 77, replay_frame_end(er, 1);
        kept = -1;
        ep2 = replay_frame_begin(er, 64, 32, &kept);
        ok = ok && ep2 && kept == 1 && ep2[0] == 77;
        if (ep2) replay_frame_end(er, 0);
        printf("%s reims frames with no screen stay with the replay\n", ok ? "ok  " : "FAIL");
        fail |= !ok;
        replay_stop(er);
    }
    return fail;
}
