/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The engine's side of the link (android/pvg_android.c, link mode) against the app's (replay.c), in
   one process: commands submitted the way pvg_lite.c submits them must reach the replay, and their
   stamps must complete only after it has done them; with the app gone they must complete anyway.
   pvg_lite.c's functions are stood in for. */
#include "replay.h"
#include "pvg_lite.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static _Atomic uint32_t completed[16];
static _Atomic int other_commands;

uint32_t hv_pvg_lite_task_root(HvPvgLite *p, uint32_t t) { (void)p, (void)t; return 0; }
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t t) { (void)p, (void)t; return 0; }
uint64_t hv_pvg_lite_surface_table(HvPvgLite *p) { (void)p; return 0; }
uint64_t hv_pvg_lite_display_pa(HvPvgLite *p) { (void)p; return 0; }
int hv_pvg_lite_ram(HvPvgLite *p, uint64_t *pa, uint64_t *size) { (void)p; *pa = 0x70000000ull, *size = 1 << 20; return 0; }
void hv_pvg_lite_mode(uint32_t *w, uint32_t *h) { *w = 1920, *h = 1080; }
void hv_pvg_lite_command(HvPvgLite *p, const uint32_t *c, uint32_t l) { (void)p, (void)c, (void)l; other_commands++; }
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t ch, uint32_t stamp) { (void)p; completed[ch & 15] = stamp; }
int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t t, uint64_t o, void *to, uint32_t n) { (void)p, (void)t, (void)o, (void)to, (void)n; return -1; }

static int wait_for(uint32_t ch, uint32_t stamp) {
    for (int i = 0; i < 500; i++) { if (completed[ch] == stamp) return 1; usleep(10000); }
    return 0;
}

int main(void) {
    char dir[] = "/tmp/henge-linktest-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char link[256], ram[256];
    snprintf(link, sizeof link, "%s/gpu.link", dir), snprintf(ram, sizeof ram, "%s/disk.img.ram", dir);
    int fd = open(ram, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, 1 << 20)) return 1;
    close(fd);
    setenv("HV_GPU_LINK", link, 1), setenv("HV_RAM_FILE", ram, 1);
    HvPvgMetal *m = hv_pvg_metal_create((HvPvgLite *)1);
    if (!m) { puts("FAIL engine side did not start"); return 1; }
    int fail = 0;

    /* No app yet: a swap completes at once. */
    uint32_t *c = calloc(1, 64);
    c[0] = 8, c[2] = 11;
    hv_pvg_metal_submit(m, NULL, 1, c, 64);
    int ok = wait_for(1, 11);
    printf("%s no renderer: the stamp completes anyway\n", ok ? "ok  " : "FAIL"), fail |= !ok;

    /* The app attaches: commands go through it. */
    Replay *r = replay_start(link, NULL, NULL, NULL);
    for (int i = 0; i < 300; i++) { usleep(10000); if (i > 20) break; }
    for (uint32_t s = 20; s < 220; s++) {
        uint32_t *x = calloc(1, 64);
        x[0] = 8, x[2] = s, x[5] = 3; /* swaps to mapping 3 */
        hv_pvg_metal_submit(m, NULL, 2, x, 64);
    }
    ok = wait_for(2, 219);
    HvLinkHeader *h = NULL;
    fd = open(link, O_RDONLY);
    if (fd >= 0) h = mmap(NULL, HV_LINK_FILE_BYTES, PROT_READ, MAP_SHARED, fd, 0), close(fd);
    ok = ok && h && h->done >= 200 && h->tail == h->head;
    printf("%s with the renderer: 200 swaps went through it (done %llu, head %llu, tail %llu)\n", ok ? "ok  " : "FAIL",
           h ? (unsigned long long)h->done : 0, h ? (unsigned long long)h->head : 0, h ? (unsigned long long)h->tail : 0);
    fail |= !ok;
    ok = access(ram, F_OK) != 0;
    printf("%s the renderer removed the RAM file's name once it had it mapped\n", ok ? "ok  " : "FAIL"), fail |= !ok;

    /* The app goes: after its heartbeat goes stale, commands complete without it. */
    replay_stop(r);
    uint32_t *y = calloc(1, 64);
    y[0] = 8, y[2] = 999;
    hv_pvg_metal_submit(m, NULL, 3, y, 64);
    for (int i = 0; i < 400 && completed[3] != 999; i++) usleep(10000);
    ok = completed[3] == 999;
    printf("%s renderer gone: completes after the heartbeat goes stale\n", ok ? "ok  " : "FAIL"), fail |= !ok;
    return fail;
}
