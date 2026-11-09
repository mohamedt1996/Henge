/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims (gpu/reims_link.h). The engine's half: vmapple's two GPU MMIO windows are forwarded to
   the app, where reims-vgpu is the device, and the interrupts it raises come back here. Nothing of the
   device's state lives in the engine, so this replaces pvg_lite outright (and VNC shows no GPU frames:
   the app's screen does). */
#include "gpu/reims_link.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "gicv3.h"
#include "machine.h"
#include "vmapple.h"

void hv_android_share_with_app(const char *path);
void hv_pvg_lite_mode(uint32_t *width, uint32_t *height);

typedef struct {
    HvReimsHeader *h;
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    int nslots;
    uint64_t waits, lost;
} Reims;

static Reims R;
static __thread int my_slot = -1;

static long futex(uint32_t *word, int op, uint32_t val, const struct timespec *t) {
    return syscall(SYS_futex, word, op, val, t, NULL, 0); /* shared: no FUTEX_PRIVATE_FLAG */
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int app_alive(void) {
    uint64_t beat = __atomic_load_n(&R.h->app_heartbeat, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&R.h->app_ready, __ATOMIC_ACQUIRE) && beat && now_ns() - beat < HV_REIMS_STALE_NS;
}

/* Until the app's device exists the GPU cannot answer, and macOS's driver must not see a dead device:
   the first access waits for it (the app starts the renderer with the VM). */
static int wait_for_app(void) {
    static int announced, given_up;
    /* Once the app has failed to come up, each later access looks only briefly: 60 s per register access
       froze the guest outright (the heartbeat's "+0"). */
    unsigned limit = given_up ? 5 : 600;
    for (unsigned i = 0; !app_alive(); i++) {
        if (!announced++) fprintf(stderr, "hengevm: reims: waiting for the app's GPU (open the Mac's screen)\n");
        if (i > limit) {
            if (!given_up++) fprintf(stderr, "hengevm: reims: the app's GPU never came up (see its \"reims:\" lines above); "
                                            "use Graphics GPU or Vulkan until it does\n");
            return -1;
        }
        usleep(100000);
    }
    if (announced && announced != -1) fprintf(stderr, "hengevm: reims: the app's GPU is there\n"), announced = -1;
    given_up = 0;
    return 0;
}

static uint64_t forward(int device, int write, uint64_t off, uint64_t value, unsigned size) {
    if (my_slot < 0) {
        my_slot = __atomic_fetch_add(&R.nslots, 1, __ATOMIC_ACQ_REL);
        if (my_slot >= HV_REIMS_SLOTS) { fprintf(stderr, "hengevm: reims: more GPU threads than slots\n"); abort(); }
    }
    if (!app_alive() && wait_for_app() != 0) {
        if (R.lost++ % 1000 == 0) fprintf(stderr, "hengevm: reims: the app's GPU is gone; %llu accesses dropped\n", (unsigned long long)R.lost);
        return 0;
    }
    HvReimsSlot *s = &R.h->slot[my_slot];
    s->device = (uint32_t)device, s->write = (uint32_t)write, s->size = size, s->offset = off, s->value = value;
    __atomic_store_n(&s->state, HV_REIMS_REQUEST, __ATOMIC_RELEASE);
    futex(&s->state, FUTEX_WAKE, 1, NULL);
    /* Most answers take microseconds: spin a little before sleeping. */
    for (unsigned spin = 0; spin < 2000; spin++)
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == HV_REIMS_REPLY) goto done;
    for (;;) {
        struct timespec t = {0, 100 * 1000000};
        futex(&s->state, FUTEX_WAIT, HV_REIMS_REQUEST, &t);
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == HV_REIMS_REPLY) break;
        if (!app_alive()) { /* the app died with our request: answer 0 rather than hang the vCPU */
            __atomic_store_n(&s->state, HV_REIMS_IDLE, __ATOMIC_RELEASE);
            R.lost++;
            return 0;
        }
        R.waits++;
    }
done:;
    uint64_t v = s->value;
    __atomic_store_n(&s->state, HV_REIMS_IDLE, __ATOMIC_RELEASE);
    __atomic_fetch_add(&R.h->requests, 1, __ATOMIC_RELAXED);
    return v;
}

static uint64_t gfx_read(void *o, uint64_t off, unsigned size) { (void)o; return forward(HV_REIMS_GFX, 0, off, 0, size); }
static void gfx_write(void *o, uint64_t off, uint64_t v, unsigned size) { (void)o; forward(HV_REIMS_GFX, 1, off, v, size); }
static uint64_t iosfc_read(void *o, uint64_t off, unsigned size) { (void)o; return forward(HV_REIMS_IOSFC, 0, off, 0, size); }
static void iosfc_write(void *o, uint64_t off, uint64_t v, unsigned size) { (void)o; forward(HV_REIMS_IOSFC, 1, off, v, size); }

/* QEMU's apple-gfx-mmio pulses its line for each raise: an edge, hv_gicv3_pend. */
static void *irq_thread(void *arg) {
    (void)arg;
    uint32_t seen = __atomic_load_n(&R.h->irq_seq, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t bits = __atomic_exchange_n(&R.h->irq_bits, 0, __ATOMIC_ACQ_REL);
        if (bits & HV_REIMS_IRQ_GFX) hv_gicv3_pend(R.gic, R.gfx_irq);
        if (bits & HV_REIMS_IRQ_IOSFC) hv_gicv3_pend(R.gic, R.iosfc_irq);
        if (bits) {
            if (R.m->ncpus == 1) hv_machine_wake(R.m);
            __atomic_fetch_add(&R.h->irqs, 1, __ATOMIC_RELAXED);
            continue;
        }
        struct timespec t = {1, 0};
        futex(&R.h->irq_seq, FUTEX_WAIT, seen, &t);
        seen = __atomic_load_n(&R.h->irq_seq, __ATOMIC_ACQUIRE);
    }
    return NULL;
}

/* vmapple.c's hook: 1 if the GPU is the app's (HV_GPU_REIMS names the file), else 0 for pvg_lite. */
int hv_pvg_external_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    const char *path = getenv("HV_GPU_REIMS");
    if (!path || !*path) return 0;
    HvRam *ram = hv_ram_lookup(m, VMAPPLE_MEM);
    const char *file = getenv("HV_RAM_FILE");
    if (!ram || !file || !*file) {
        fprintf(stderr, "hengevm: reims: guest RAM is not a file the app can map; GPU off\n");
        return 0;
    }
    /* The app opens guest RAM the moment it sees the magic below: give it the RAM file first. runner.c shares
       it only after hv_vm_create returns, by which time the app had already hit EACCES and given up. */
    hv_android_share_with_app(file);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) hv_android_share_with_app(path);
    if (fd < 0 || ftruncate(fd, HV_REIMS_FILE_BYTES) != 0) {
        fprintf(stderr, "hengevm: reims: %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return 0;
    }
    HvReimsHeader *h = mmap(NULL, HV_REIMS_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (h == MAP_FAILED) return 0;
    memset(h, 0, sizeof *h);
    h->version = HV_REIMS_VERSION;
    h->ram_pa = ram->pa, h->ram_size = ram->size;
    snprintf(h->ram_file, sizeof h->ram_file, "%s", file);
    hv_pvg_lite_mode(&h->display_w, &h->display_h);
    h->engine_alive = 1;
    __atomic_store_n(&h->magic, HV_REIMS_MAGIC, __ATOMIC_RELEASE);
    R = (Reims){h, m, gic, gfx_irq, iosfc_irq, 0, 0, 0};
    hv_machine_add_mmio(m, gfx_pa, 0x10000, &R, gfx_read, gfx_write, "gfx-reims");
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, &R, iosfc_read, iosfc_write, "iosurface-reims");
    pthread_t t;
    pthread_create(&t, NULL, irq_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "hengevm: GPU: reims-vgpu in the app, through %s\n", path);
    return 1;
}

void hv_pvg_reims_activity(uint64_t *requests, uint64_t *irqs) {
    *requests = R.h ? R.h->requests : 0, *irqs = R.h ? R.h->irqs : 0;
}
