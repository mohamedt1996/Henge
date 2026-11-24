/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims (gpu/reims_link.h). The engine's half: vmapple's two GPU MMIO windows are forwarded to
   the app, where reims-vgpu is the device, and the interrupts it raises come back here. Nothing of the
   device's state lives in the engine, so this replaces pvg_lite outright (and VNC shows no GPU frames:
   the app's screen does). */
#include "gpu/reims_link.h"
#include "gpu/ram_fd.h"

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
void hv_pvg_lite_set_mode(uint32_t width, uint32_t height);

typedef struct {
    HvReimsHeader *h;
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    int nslots;
    uint64_t waits, lost;
    uint64_t wait_ns; /* all CPUs' time inside forward(), for the heartbeat */
    /* 0.3.126: the waits apart: answers seen while still awake and those slept for, and of both the ones that were
       for this thread's own posted write (a doorbell the next access has to wait out). Counts and ns. */
    uint64_t awake_n, awake_ns, slept_n, slept_ns, pend_n, pend_ns, pend_slept_n;
    uint64_t post_n, post_ns; /* 0.3.127: the posting itself (the futex wake of the app's thread, and what follows it on this core) */
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

#define HV_REIMS_BRINGUP_SECS 45
static int wait_for_app_secs(int secs) {
    for (int i = 0; !app_alive(); i++) {
        if (i == 0) fprintf(stderr, "hengevm: reims: waiting for the app's GPU to come up\n");
        if (i >= secs * 10) return -1;
        usleep(100000);
    }
    fprintf(stderr, "hengevm: reims: the app's GPU is up\n");
    return 0;
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

/* Posted writes (0.3.59): a register write is handed to the app's slot and the vCPU goes on, as a write to
   a real device's registers is posted on its bus; the next access from the same thread (read or write)
   first waits for it to be done, so one thread's accesses still reach the device in order, and a read
   still sees every earlier write. The tablet's log: the Mac's CPUs stood 1 to 4.7 s of every 30 waiting
   here, mostly on the gfx doorbell write (+0x1020), with single waits up to 11 ms. HV_REIMS_POSTED=0: every
   access waited for, as before. The app marks a write done with IDLE (a read with REPLY). */
static __thread int my_pending;
static __thread uint64_t my_pending_since;

static int posted_writes(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("HV_REIMS_POSTED");
        on = !(v && *v == '0');
    }
    return on;
}

static int spin_fixed; /* HV_REIMS_SPIN_US was given: that limit, not one learnt */

static int64_t spin_limit_ns(void) {
    /* Most answers take microseconds, and a CPU thread that sleeps here pays the scheduler twice (the app's
       thread woken, then this one: on the tablet an access cost a millisecond on average, 0.3.24). So it
       stays awake for at most the first 150 us, by the clock, and sleeps only for an answer that takes
       longer. HV_REIMS_SPIN_US changes it (0: sleep at once) and turns off the learning (spin_now_ns). */
    static int64_t spin_ns = -1;
    if (spin_ns < 0) {
        const char *us = getenv("HV_REIMS_SPIN_US");
        spin_fixed = us && *us;
        spin_ns = (spin_fixed ? atoll(us) : 150) * 1000;
    }
    return spin_ns;
}

/* 0.3.61: how long answers caught awake took, on average (1/8 the newest; ns). The wait stays awake for four
   times that and 10 us more, at least 60 us (0.3.63; 30 let waits of up to 6 s in 30 build up), at most spin_limit_ns: answers of a few microseconds no longer
   keep a core awake 150 us when the app's thread is slow, and a slow spell lengthens it again. */
static uint32_t answer_avg_ns = 20000;

static int64_t spin_now_ns(void) {
    int64_t most = spin_limit_ns();
    if (spin_fixed || most <= 60000) return most;
    int64_t t = 4 * (int64_t)__atomic_load_n(&answer_avg_ns, __ATOMIC_RELAXED) + 10000;
    return t < 60000 ? 60000 : t > most ? most : t;
}

static void answer_took(int64_t ns) {
    if (ns < 0) return;
    if (ns > 1000000) ns = 1000000;
    uint32_t a = __atomic_load_n(&answer_avg_ns, __ATOMIC_RELAXED); /* racy between CPUs: an average, no more */
    __atomic_store_n(&answer_avg_ns, (uint32_t)((int64_t)a + (ns - (int64_t)a) / 8), __ATOMIC_RELAXED);
}

/* Waits until the slot's request is no longer REQUEST (answered: REPLY, or IDLE for a write); 0, or -1
   when the app died with it (the slot is then IDLE and the access counts as lost). */
static int wait_answer_1(HvReimsSlot *s, uint64_t began, int *slept) {
    *slept = 0;
    int64_t spin_ns = spin_now_ns();
    uint64_t from = now_ns();
#if defined(__aarch64__) && !defined(HV_REIMS_NO_WFE)
    /* 0.3.61: awake in the core's WFE, not a yield loop. LDAXR arms the exclusive monitor on the slot's state;
       the app's store to it clears the monitor, which wakes the WFE at once; otherwise the kernel's event
       stream (10 kHz on arm64 Linux) wakes it to look at the clock. A core in WFE is clock-gated: the wait
       costs a fraction of a spinning core's power, and the answer is seen as soon. */
    if (spin_ns > 0)
        for (;;) {
            uint32_t v;
            __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(&s->state) : "memory");
            if (v != HV_REIMS_REQUEST) {
                __asm__ volatile("clrex" ::: "memory");
                answer_took((int64_t)(now_ns() - from));
                return 0;
            }
            if ((int64_t)(now_ns() - began) >= spin_ns) break;
            __asm__ volatile("wfe" ::: "memory");
        }
    __asm__ volatile("clrex" ::: "memory");
#else
    for (unsigned spin = 0;; spin++) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) {
            answer_took((int64_t)(now_ns() - from));
            return 0;
        }
        if ((spin & 63) == 63 && (int64_t)(now_ns() - began) >= spin_ns) break;
    }
#endif
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) return 0;
    answer_took(spin_limit_ns()); /* not answered awake: the next waits stay awake longer */
    *slept = 1;
    for (;;) {
        struct timespec t = {0, 100 * 1000000};
        futex(&s->state, FUTEX_WAIT, HV_REIMS_REQUEST, &t);
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) return 0;
        if (!app_alive()) { /* the app died with our request: answer 0 rather than hang the vCPU */
            __atomic_store_n(&s->state, HV_REIMS_IDLE, __ATOMIC_RELEASE);
            R.lost++;
            return -1;
        }
        R.waits++;
    }
}

static int wait_answer(HvReimsSlot *s, uint64_t began, int pending) {
    uint64_t t0 = now_ns();
    int slept = 0, rc = wait_answer_1(s, began, &slept);
    uint64_t d = now_ns() - t0;
    if (slept) __atomic_fetch_add(&R.slept_n, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&R.slept_ns, d, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&R.awake_n, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&R.awake_ns, d, __ATOMIC_RELAXED);
    if (pending) {
        __atomic_fetch_add(&R.pend_n, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&R.pend_ns, d, __ATOMIC_RELAXED);
        if (slept) __atomic_fetch_add(&R.pend_slept_n, 1, __ATOMIC_RELAXED);
    }
    return rc;
}

/* For the heartbeat: nine counts, as in Reims. */
void hv_pvg_reims_wait_split(uint64_t v[9]) {
    v[0] = R.awake_n, v[1] = R.awake_ns, v[2] = R.slept_n, v[3] = R.slept_ns, v[4] = R.pend_n, v[5] = R.pend_ns, v[6] = R.pend_slept_n;
    v[7] = R.post_n, v[8] = R.post_ns;
}

static uint64_t forward(int device, int write, uint64_t off, uint64_t value, unsigned size) {
    if (my_slot < 0) {
        my_slot = __atomic_fetch_add(&R.nslots, 1, __ATOMIC_ACQ_REL);
        if (my_slot >= HV_REIMS_SLOTS) { fprintf(stderr, "hengevm: reims: more GPU threads than slots\n"); abort(); }
    }
    HvReimsSlot *s = &R.h->slot[my_slot];
    if (my_pending) { /* this thread's posted write: done before anything else of its goes */
        uint64_t w0 = now_ns();
        /* 0.3.126: awake from now, not from when the write was posted. A doorbell (gfx +0x1020) takes the app 45 us on
           average and the Mac reads the status right behind it; counted from the post, the awake part was over
           before this wait began, so it slept, and a sleep costs both sides' wake-ups: the logs' 175 to 200 us an
           access where the app's own share is 50. HV_REIMS_PENDING_FROM_POST=1: as before. */
        static int from_post = -1;
        if (from_post < 0) { const char *v = getenv("HV_REIMS_PENDING_FROM_POST"); from_post = v && *v == '1'; }
        wait_answer(s, from_post ? my_pending_since : w0, 1);
        __atomic_fetch_add(&R.wait_ns, now_ns() - w0, __ATOMIC_RELAXED);
        my_pending = 0;
    }
    if (!app_alive() && wait_for_app() != 0) {
        if (R.lost++ % 1000 == 0) fprintf(stderr, "hengevm: reims: the app's GPU is gone; %llu accesses dropped\n", (unsigned long long)R.lost);
        return 0;
    }
    uint64_t began = now_ns();
    s->device = (uint32_t)device, s->write = (uint32_t)write, s->size = size, s->offset = off, s->value = value;
    s->sent_ns = began;
    __atomic_store_n(&s->state, HV_REIMS_REQUEST, __ATOMIC_RELEASE);
    futex(&s->state, FUTEX_WAKE, 1, NULL);
    /* 0.3.126's split: the waits themselves were 2 s of the 8 s counted per 30 s; the rest lies between `began` and the
       wait, which is this call (the woken thread may be put on this core, before this one goes on). Timed. */
    __atomic_fetch_add(&R.post_n, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&R.post_ns, now_ns() - began, __ATOMIC_RELAXED);
    if (write && posted_writes()) {
        my_pending = 1, my_pending_since = began;
        __atomic_fetch_add(&R.h->requests, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (wait_answer(s, began, 0) != 0) return 0;
    __atomic_fetch_add(&R.wait_ns, now_ns() - began, __ATOMIC_RELAXED);
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
    /* A new file, never the last run's truncated in place: the app may already have the old one mapped, and
       truncating under it zeroed app_ready (or SIGBUSed it), leaving the guest waiting on a GPU that never came. */
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) hv_android_share_with_app(path);
    if (fd < 0 || ftruncate(fd, HV_REIMS_FILE_BYTES) != 0) {
        fprintf(stderr, "hengevm: reims: %s: %s; using GPU instead\n", path, strerror(errno));
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
    {   /* The Mac's RAM in memory (HV_RAM_MEMFD): its name, /proc/<engine>/fd/<n>, is one Android lets no app
           open, so the app is handed the descriptor over a socket and the header names the socket ('@...').
           Where the socket cannot be made the /proc name stays, and the app's note sends the next start back
           to the RAM file, as before. */
        const char *number = getenv("HV_RAM_FD");
        char name[64];
        if (number && *number && !strncmp(file, "/proc/", 6)) {
            if (hv_ram_fd_serve(atoi(number), name, sizeof name) == 0) {
                snprintf(h->ram_file, sizeof h->ram_file, "%s", name);
                fprintf(stderr, "hengevm: reims: the Mac's RAM is handed to the app as a descriptor (%s)\n", name);
            } else {
                fprintf(stderr, "hengevm: reims: no socket to hand the Mac's RAM over (%s); the app opens %s\n", strerror(errno), file);
            }
        }
    }
    hv_pvg_lite_mode(&h->display_w, &h->display_h);
    {   /* HV_REIMS_MODE=WxH (the app's "Mac screen" setting). Without the host-memory import every surface is a
           copy compared on the CPU at each commit, so fewer pixels is the first performance lever: 1280x800
           by default, a 1920x1080 only when asked. */
        const char *mode = getenv("HV_REIMS_MODE");
        unsigned w = 0, hgt = 0;
        if (mode && sscanf(mode, "%ux%u", &w, &hgt) == 2 && w >= 640 && hgt >= 480 && w <= 7680 && hgt <= 4320)
            h->display_w = w, h->display_h = hgt;
        else if (h->display_w == 1920 && h->display_h == 1080)
            h->display_w = 1280, h->display_h = 800;
        fprintf(stderr, "hengevm: reims: the Mac's screen is %ux%u%s\n", h->display_w, h->display_h,
                mode ? "" : " (HV_REIMS_MODE=WxH to change)");
        /* The engine's own display size too: the boot framebuffer and the VNC framebuffer the app maps its
           touches through are then the Mac's screen, one coordinate space. */
        hv_pvg_lite_set_mode(h->display_w, h->display_h);
    }
    h->engine_alive = 1;
    __atomic_store_n(&h->magic, HV_REIMS_MAGIC, __ATOMIC_RELEASE);
    /* The guest has not run yet: wait here for the app's device, and if it does not come, boot with pvg_lite
       rather than hand macOS a GPU that never answers (a frozen boot). */
    R.h = h;
    if (wait_for_app_secs(HV_REIMS_BRINGUP_SECS) != 0) {
        fprintf(stderr, "hengevm: reims: the app's GPU did not come up in %d s (its \"reims:\" lines say why); "
                        "booting with Graphics GPU instead\n", HV_REIMS_BRINGUP_SECS);
        __atomic_store_n(&h->magic, 0, __ATOMIC_RELEASE);
        h->engine_alive = 0;
        munmap(h, HV_REIMS_FILE_BYTES);
        unlink(path);
        R.h = NULL;
        return 0;
    }
    R = (Reims){h, m, gic, gfx_irq, iosfc_irq, 0, 0, 0};
    /* Self-locked, as the GIC: each access waits for the app, and under the engine's device lock that wait
       stalled every other device for the other vCPU (a timer or UART access queued behind a GPU register).
       reims-vgpu takes its own locks, as QEMU's MMIO calls into it do. */
    hv_machine_add_mmio(m, gfx_pa, 0x10000, &R, gfx_read, gfx_write, "gfx-reims");
    m->mmio[m->nmmio - 1].self_locked = 1;
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, &R, iosfc_read, iosfc_write, "iosurface-reims");
    m->mmio[m->nmmio - 1].self_locked = 1;
    pthread_t t;
    pthread_create(&t, NULL, irq_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "hengevm: GPU: reims-vgpu in the app, through %s\n", path);
    return 1;
}

/* host/vm.c's hook (weak there): the guest's cursor as the app last placed it, for the pointer correction
   the VNC server does (vnc.c's pointer_event), which read pvg_lite's display page before. */
int hv_pvg_external_pointer(int *x, int *y) {
    HvReimsHeader *h = R.h;
    if (!h) return -1;
    for (int i = 0; i < 8; i++) {
        uint32_t s0 = __atomic_load_n(&h->cursor_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        uint32_t cx = h->cursor_x, cy = h->cursor_y, valid = h->cursor_valid;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&h->cursor_seq, __ATOMIC_ACQUIRE) != s0) continue;
        if (!valid) return -1;
        *x = (int)cx, *y = (int)cy;
        return 0;
    }
    return -1;
}

uint64_t hv_pvg_reims_wait_ns(void) { return __atomic_load_n(&R.wait_ns, __ATOMIC_RELAXED); }
/* The Mac's screen with Graphics: reims (0: reims is the GPU and the size is given; -1: not). The VNC framebuffer
   is the boot framebuffer then, of another size (640 wide in the logs), so places on the Mac's screen are
   reckoned by this (android/vnc.c). */
int hv_pvg_reims_screen(int *w, int *h) {
    if (!R.h || !R.h->engine_alive || !R.h->display_w || !R.h->display_h) return -1;
    *w = (int)R.h->display_w, *h = (int)R.h->display_h;
    /* 0.3.125: the mode macOS set, where the app has told it: macOS may keep another resolution than the one offered
       first (chosen in its Displays settings), and a touch reckoned by the offered one landed off by their ratio,
       further off the further from the screen's top left. */
    uint32_t mw = __atomic_load_n(&R.h->mode_w, __ATOMIC_ACQUIRE), mh = __atomic_load_n(&R.h->mode_h, __ATOMIC_ACQUIRE);
    if (mw >= 320 && mh >= 200) {
        static uint32_t said_w, said_h;
        if ((mw != said_w || mh != said_h) && (mw != R.h->display_w || mh != R.h->display_h))
            fprintf(stderr, "hengevm: pointer: the Mac's screen is %ux%u now (offered %ux%u): places are reckoned by it\n", mw, mh,
                    R.h->display_w, R.h->display_h);
        said_w = mw, said_h = mh;
        *w = (int)mw, *h = (int)mh;
    }
    return 0;
}

/* The awake part of a wait now (us), as learnt (spin_now_ns). */
int hv_pvg_reims_spin_us(void) { return (int)(spin_now_ns() / 1000); }

void hv_pvg_reims_activity(uint64_t *requests, uint64_t *irqs) {
    *requests = R.h ? R.h->requests : 0, *irqs = R.h ? R.h->irqs : 0;
}
