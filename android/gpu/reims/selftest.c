/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims, end to end without macOS: this program plays the engine (pvg_reims.c's side of
   gpu.reims, over a RAM file) against the app's half (host.c) on reims-vgpu, on whatever Vulkan driver
   the machine has (CI: lavapipe). It checks that the device comes up on Vulkan, answers register reads
   through the slots, takes writes, and draws a frame of its display into a frame target for the screen.

   selftest DIR   (exit 0 when every step passed) */
#include "../reims_link.h"
#include "reims_host.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RAM_PA 0x70000000ull
#define RAM_SIZE (256ull << 20)

static HvReimsHeader *X;
static volatile int frames;
static volatile uint32_t frame_w, frame_h;
static volatile uint32_t frame_px;

/* The replay's presenter, as host.c hands it frames: the frame's pixels are the presenter's own, which host.c
   reads the frame target back into between begin and end. */
static uint8_t *shown_px;
static uint32_t shown_w, shown_h;
uint8_t *replay_frame_begin(Replay *r, uint32_t w, uint32_t h, int *kept) {
    (void)r;
    int same = shown_px && shown_w == w && shown_h == h;
    if (!same) {
        free(shown_px);
        shown_px = calloc((size_t)w * h, 4);
        shown_w = w, shown_h = h;
    }
    if (kept) *kept = same;
    return shown_px;
}
void replay_present_stats(Replay *r, uint64_t *presents, uint64_t *ns) { (void)r, *presents = 0, *ns = 0; }
void replay_screen_words(Replay *r, char *out, size_t n) { (void)r; snprintf(out, n, "the test's"); }
void replay_frame_end(Replay *r, int changed) {
    (void)r;
    if (!changed) return;
    frame_w = shown_w, frame_h = shown_h, frame_px = *(const uint32_t *)shown_px;
    __atomic_add_fetch(&frames, 1, __ATOMIC_RELEASE);
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* pvg_reims.c's forward(), slot 0. */
static int mmio(int device, int write, uint64_t off, uint64_t *value) {
    HvReimsSlot *s = &X->slot[0];
    s->device = (uint32_t)device, s->write = (uint32_t)write, s->size = 4, s->offset = off, s->value = write ? *value : 0;
    __atomic_store_n(&s->state, HV_REIMS_REQUEST, __ATOMIC_RELEASE);
    syscall(SYS_futex, &s->state, FUTEX_WAKE, 1, NULL, NULL, 0);
    uint64_t until = now_ms() + 5000;
    /* Answered: REPLY for a read, IDLE for a write (writes are posted, 0.3.59). */
    while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == HV_REIMS_REQUEST)
        if (now_ms() > until) return -1;
    if (!write) *value = s->value;
    __atomic_store_n(&s->state, HV_REIMS_IDLE, __ATOMIC_RELEASE);
    return 0;
}

static int fails;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char ram[1024], link[1024];
    snprintf(ram, sizeof ram, "%s/disk.img.ram", dir);
    snprintf(link, sizeof link, "%s/gpu.reims", dir);

    int rfd = open(ram, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (rfd < 0 || ftruncate(rfd, (off_t)RAM_SIZE) != 0) { perror(ram); return 2; }
    close(rfd);
    int fd = open(link, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, HV_REIMS_FILE_BYTES) != 0) { perror(link); return 2; }
    X = mmap(NULL, HV_REIMS_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (X == MAP_FAILED) { perror("mmap"); return 2; }
    X->version = HV_REIMS_VERSION, X->ram_pa = RAM_PA, X->ram_size = RAM_SIZE;
    snprintf(X->ram_file, sizeof X->ram_file, "%s", ram);
    X->display_w = 1920, X->display_h = 1080, X->engine_alive = 1;
    __atomic_store_n(&X->magic, HV_REIMS_MAGIC, __ATOMIC_RELEASE);

    check(reims_host_start(link, NULL, NULL, NULL) == 0, "the app's host starts");
    uint64_t until = now_ms() + 60000;
    while (!__atomic_load_n(&X->app_ready, __ATOMIC_ACQUIRE) && now_ms() < until) usleep(10000);
    check(X->app_ready, "reims-vgpu's device, IOSurface device and display come up on Vulkan");
    if (!X->app_ready) return 1;
    check(X->app_heartbeat != 0, "the app's heartbeat runs");

    /* Registers whose answers the guest's driver reads first (pvg_lite.c's notes): the display count
       at +122c, and the protocol version the device answers at +1034. */
    uint64_t v = 0;
    check(mmio(HV_REIMS_GFX, 0, 0x122c, &v) == 0, "a gfx register read is answered through a slot");
    printf("     gfx +122c (displays) = %#llx\n", (unsigned long long)v);
    check(v == 1, "it reports one display");
    v = 0;
    check(mmio(HV_REIMS_GFX, 0, 0x1034, &v) == 0, "the protocol version register reads");
    printf("     gfx +1034 (version) = %#llx\n", (unsigned long long)v);
    v = 0;
    check(mmio(HV_REIMS_IOSFC, 0, 0x1028, &v) == 0, "an IOSurface-mapper register read is answered");
    printf("     iosfc +1028 = %#llx\n", (unsigned long long)v);
    uint64_t w = 0;
    check(mmio(HV_REIMS_GFX, 1, 0x1000, &w) == 0, "a gfx register write is taken");

    /* A screen attaches: the display's current frame is encoded on the GPU and read back. */
    reims_host_set_sink((Replay *)1);
    until = now_ms() + 30000;
    while (!__atomic_load_n(&frames, __ATOMIC_ACQUIRE) && now_ms() < until) usleep(10000);
    check(frames > 0, "a frame is encoded into a frame target and read back for the screen");
    if (frames) printf("     frame %ux%u, first pixel %08x\n", frame_w, frame_h, frame_px);
    check(frame_w == 1920 && frame_h == 1080, "the frame has the display's mode");
    reims_host_set_sink(NULL);

    reims_host_stop();
    check(!X->app_ready, "the host stops and says so");
    unlink(ram), unlink(link);
    printf("%s\n", fails ? "reims selftest: FAILED" : "reims selftest: passed");
    return fails ? 1 : 0;
}
