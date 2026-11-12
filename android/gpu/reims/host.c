/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims, the app's half (../reims_link.h): reims-vgpu is macOS's paravirtual GPU device here,
   in the app's process, on Android's Vulkan driver, and the engine forwards the device's MMIO to it.
   This file is the host reims-vgpu's C API asks for, shaped like QEMU's apple-gfx-mmio.c on the same API:
   guest RAM as one memory-map range over the RAM file, the IOSurface mapper device beside the GPU, the
   two interrupt lines pulsed per raise, one display whose frames are drawn into a frame target and
   handed to the replay's presenter (replay_show_frame) for the screen. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* Dl_info, dladdr */
#endif
#include "../reims_link.h"
#include "../ram_fd.h"
#include "../replay/replay.h"
#include "reims_vgpu.h"
#include "reims_host.h"
#include "turnip_load.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char path[1024];
    char home[1024];
    char vulkan[1024]; /* the Vulkan library metal2vulkan opens (Turnip), or empty for the system's */
    HvReimsHeader *h;
    int ram_fd;
    uint8_t *ram;
    uint64_t ram_pa, ram_size;
    RvgDevice *dev;
    RvgIOSurfaceDevice *iosfc;
    RvgDisplay *disp;
    volatile int stop, ready;
    pthread_t attach, beat, render, slot[HV_REIMS_SLOTS];
    int nslot_threads;
    int told;
    /* the screen */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int frame_pending, mode_changed, cursor_changed;
    uint32_t mode_w, mode_h;
    RvgFrameTarget *target;
    uint32_t tw, th;
    uint8_t *pixels, *shown; /* RGBA8: the last frame, and it with the pointer drawn over it */
    uint8_t *glyph;
    uint32_t gw, gh;
    int hx, hy, cursor_on;
    Replay *sink;
    uint64_t frames, mmio;
    /* Where a register access's time goes, since the last log line (reg_lock): from the engine's post to
       this side's thread taking it, and inside the library; and the registers that took the most. */
    pthread_mutex_t reg_lock;
    uint64_t reg_n, reg_reach_ns, reg_device_ns;
    struct { uint64_t key, n, ns, max_ns; } reg[48];
    uint64_t t_encode, t_read, t_show, t_frames, t_shows; /* ns and counts since the last log line */
} Host;

/* reims-vgpu calls memfd_create, which bionic has from API 30; the app's minSdk is 29, and the kernel
   has had the syscall since Linux 3.17. */
int memfd_create(const char *name, unsigned int flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}

static Host *H;
static int log_fd = -1;

/* To logcat (henge-gpu) and to the app's own log (henge.log), which the Log tab shows: the engine writes
   there too, so this side's lines sit beside its own, in order. */
#include <stdarg.h>
static void hlog(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    rlog("%s", line);
    if (log_fd >= 0) {
        char out[1100];
        int m = snprintf(out, sizeof out, "henge-gpu: %.*s\n", n, line);
        if (m > 0) (void)!write(log_fd, out, (size_t)(m < (int)sizeof out ? m : (int)sizeof out - 1));
    }
}
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

/* Where bring-up is: a crash or a kill in it left no line at all, only the engine's "waiting" and a fresh run. */
static const char *volatile stage = "idle";
static void set_stage(const char *s) { stage = s; hlog("reims: %s", s); }

/* A native crash in this process takes the whole app (and the engine with it) down with nothing in henge.log:
   write the signal and the stage first (write() only: async-signal-safe), then hand over to debuggerd's handler. */
static struct sigaction old_act[32];

/* Async-signal-safe formatting: hex and a few strings. */
static size_t put_str(char *out, size_t m, size_t cap, const char *c) { while (*c && m < cap - 1) out[m++] = *c++; return m; }
static size_t put_hex(char *out, size_t m, size_t cap, uint64_t v) {
    char d[19]; int n = 0;
    do d[n++] = "0123456789abcdef"[v & 15], v >>= 4; while (v);
    if (m < cap - 3) out[m++] = '0', out[m++] = 'x';
    while (n && m < cap - 1) out[m++] = d[--n];
    return m;
}

/* The frames of the crashing thread: _Unwind_Backtrace walks them (not strictly signal-safe, but the
   process is dying anyway), dladdr names the library and the offset in it, which `ndk-stack`/`addr2line`
   turn into lines. The faulting address too: a NULL or near-NULL one is a missing object. */
#include <dlfcn.h>
#include <unwind.h>
typedef struct { uintptr_t pc[48]; int n; } Frames;
static _Unwind_Reason_Code take_frame(struct _Unwind_Context *ctx, void *arg) {
    Frames *f = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && f->n < (int)(sizeof f->pc / sizeof *f->pc)) f->pc[f->n++] = pc;
    return f->n < (int)(sizeof f->pc / sizeof *f->pc) ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static void write_frames(void);
static void write_backtrace(int sig, siginfo_t *si) {
    char out[512];
    size_t m = 0;
    m = put_str(out, m, sizeof out, "henge-gpu: reims: fault address ");
    m = put_hex(out, m, sizeof out, (uint64_t)(uintptr_t)(sig == SIGSEGV || sig == SIGBUS ? si->si_addr : 0));
    m = put_str(out, m, sizeof out, ", code ");
    m = put_hex(out, m, sizeof out, (uint64_t)si->si_code);
    m = put_str(out, m, sizeof out, "\n");
    (void)!write(log_fd, out, m);
    write_frames();
}
static void write_frames(void) {
    char out[512];
    size_t m;
    Frames f = {{0}, 0};
    _Unwind_Backtrace(take_frame, &f);
    for (int i = 0; i < f.n; i++) {
        Dl_info info;
        m = 0;
        m = put_str(out, m, sizeof out, "henge-gpu: reims:   #");
        m = put_hex(out, m, sizeof out, (uint64_t)i);
        m = put_str(out, m, sizeof out, " pc ");
        if (dladdr((void *)f.pc[i], &info) && info.dli_fname) {
            m = put_hex(out, m, sizeof out, (uint64_t)(f.pc[i] - (uintptr_t)info.dli_fbase));
            m = put_str(out, m, sizeof out, " ");
            const char *base = strrchr(info.dli_fname, '/');
            m = put_str(out, m, sizeof out, base ? base + 1 : info.dli_fname);
            if (info.dli_sname) {
                m = put_str(out, m, sizeof out, " (");
                m = put_str(out, m, sizeof out, info.dli_sname);
                m = put_str(out, m, sizeof out, "+");
                m = put_hex(out, m, sizeof out, (uint64_t)(f.pc[i] - (uintptr_t)info.dli_saddr));
                m = put_str(out, m, sizeof out, ")");
            }
        } else {
            m = put_hex(out, m, sizeof out, (uint64_t)f.pc[i]);
            m = put_str(out, m, sizeof out, " ?");
        }
        m = put_str(out, m, sizeof out, "\n");
        (void)!write(log_fd, out, m);
    }
}

static void on_crash(int sig, siginfo_t *si, void *uc) {
    if (log_fd >= 0) {
        char out[256];
        const char *n = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGABRT ? "SIGABRT" : sig == SIGILL ? "SIGILL" : "SIGFPE";
        size_t m = 0;
        const char *parts[] = {"henge-gpu: reims: the app crashed (", n, ") during: ", stage, "\n"};
        for (unsigned i = 0; i < 5; i++)
            for (const char *c = parts[i]; *c && m < sizeof out - 1; c++) out[m++] = *c;
        (void)!write(log_fd, out, m);
        write_backtrace(sig, si);
    }
    struct sigaction *o = &old_act[sig];
    if (o->sa_flags & SA_SIGINFO) { if (o->sa_sigaction) { o->sa_sigaction(sig, si, uc); return; } }
    else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) { o->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_crash_log(void) {
    static int done;
    if (done++) return;
    /* SA_ONSTACK needs a stack: without one a stack overflow's SIGSEGV has nowhere to run the handler. */
    static char alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    int sigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
    struct sigaction a = {0};
    a.sa_sigaction = on_crash;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&a.sa_mask);
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &a, &old_act[sigs[i]]);
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static long futex(uint32_t *word, int op, uint32_t val, const struct timespec *t) {
    return syscall(SYS_futex, word, op, val, t, NULL, 0);
}

/* ---- Where the GPU's threads are when the guest waits on it ----
   The guest's driver reports a wait of 2 s for the GPU ("stamp wait report") when a command buffer takes that
   long; on the tablet they took tens of seconds each, with nothing in the log to say doing what. On such a
   report (at most every 30 s, 10 times a run) every busy thread of this process, and every thread of the GPU
   library's by name, writes its backtrace to the log: a sample of where the time goes. The library's symbols
   are in the CI's "symbols" artifact for the same build, for the offsets dladdr cannot name. */
#include <dirent.h>
#include <sys/prctl.h>
#define SAMPLE_SIG (SIGRTMAX - 3)
static volatile int sample_wanted;
static void on_sample(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si, (void)uc;
    if (log_fd < 0) return;
    int saved = errno;
    char name[32] = "?", out[160];
    prctl(PR_GET_NAME, name, 0, 0, 0);
    size_t m = 0;
    m = put_str(out, m, sizeof out, "henge-gpu: reims: sample: thread ");
    m = put_str(out, m, sizeof out, name);
    m = put_str(out, m, sizeof out, " is at:\n");
    (void)!write(log_fd, out, m);
    write_frames();
    errno = saved;
}
static unsigned long long thread_ticks(const char *tid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/self/task/%s/stat", tid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *p = strrchr(buf, ')'); /* past the name, which may hold spaces */
    unsigned long long ut = 0, st = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
    return ut + st;
}
static void sample_threads(void) {
    static int installed;
    if (!installed++) {
        struct sigaction a = {0};
        a.sa_sigaction = on_sample;
        a.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&a.sa_mask);
        sigaction(SAMPLE_SIG, &a, NULL);
    }
    enum { MAX = 96 };
    static char tids[MAX][12];
    static unsigned long long before[MAX];
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && n < MAX;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(tids[n], sizeof tids[n], "%s", e->d_name);
        before[n] = thread_ticks(tids[n]);
        n++;
    }
    closedir(d);
    usleep(400000);
    long me = syscall(SYS_gettid);
    int sent = 0;
    for (int i = 0; i < n && sent < 14; i++) {
        long tid = atol(tids[i]);
        if (tid == me) continue;
        char path[64], comm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", tids[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { ssize_t k = read(fd, comm, sizeof comm - 1); if (k > 0) comm[k] = 0; close(fd); }
        int busy = thread_ticks(tids[i]) - before[i] >= 4; /* a tenth of the 400 ms on a CPU, at 100 ticks a second */
        int ours = !strncmp(comm, "metal2vulkan", 12) || !strncmp(comm, "m2v", 3) || !strncmp(comm, "reims", 5) ||
                   !strncmp(comm, "com.Metal", 9) || strstr(comm, "fifo") || strstr(comm, "FIFO");
        if (!busy && !ours) continue;
        if (syscall(SYS_tgkill, getpid(), tid, SAMPLE_SIG) == 0) sent++, usleep(40000); /* one at a time: lines stay together */
    }
    hlog("reims: sample: %d of %d threads sampled (the busy ones and the GPU library's)", sent, n);
}

static void on_log(void *ctx, RvgLogLevel level, const char *target, size_t tl, const char *msg, size_t ml) {
    (void)ctx, (void)level;
    hlog("reims %.*s: %.*s", (int)tl, target, (int)ml, msg);
    if (ml >= 17 && !memcmp(msg, "stamp wait report", 17)) sample_wanted = 1;
}

/* ------------------------------------------------------------------ interrupts -- */
static void raise_bits(Host *h, uint32_t bits) {
    __atomic_fetch_or(&h->h->irq_bits, bits, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&h->h->irq_seq, 1, __ATOMIC_ACQ_REL);
    futex(&h->h->irq_seq, FUTEX_WAKE, 1, NULL);
}
static void gfx_raise(void *ctx, uint32_t vector) { (void)vector; raise_bits(ctx, HV_REIMS_IRQ_GFX); }
static void iosfc_raise(void *ctx, uint32_t vector) { (void)vector; raise_bits(ctx, HV_REIMS_IRQ_IOSFC); }

/* ------------------------------------------------------------------- the display -- */
static void on_mode(void *ctx, uint16_t w, uint16_t hgt, uint32_t format) {
    Host *h = ctx;
    (void)format;
    pthread_mutex_lock(&h->lock);
    h->mode_w = w, h->mode_h = hgt, h->mode_changed = 1, h->frame_pending = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
    hlog("reims: mode %ux%u", w, hgt);
}

static void on_frame(void *ctx) {
    Host *h = ctx;
    pthread_mutex_lock(&h->lock);
    h->frame_pending = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
}

static void on_glyph(void *ctx, const RvgCursorGlyph *g) {
    Host *h = ctx;
    if (!g->width || !g->height || g->width > 512 || g->height > 512 || g->rgba_length < (size_t)g->width * g->height * 4) return;
    uint8_t *copy = malloc((size_t)g->width * g->height * 4);
    if (!copy) return;
    memcpy(copy, g->rgba, (size_t)g->width * g->height * 4);
    pthread_mutex_lock(&h->lock);
    free(h->glyph);
    h->glyph = copy, h->gw = g->width, h->gh = g->height, h->hx = g->hot_spot.x, h->hy = g->hot_spot.y;
    h->cursor_changed = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
}

static void publish_cursor(Host *h);

static unsigned long long cache_files, cache_bytes;
static int cache_count(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)path, (void)ftw;
    if (type == FTW_F) cache_files++, cache_bytes += (unsigned long long)st->st_size;
    return 0;
}
static void on_show(void *ctx, uint8_t show) {
    Host *h = ctx;
    publish_cursor(h);
    pthread_mutex_lock(&h->lock);
    h->cursor_on = show != 0, h->cursor_changed = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
}

/* The guest's cursor into the shared file, for the engine's pointer correction (reims_link.h). */
static void publish_cursor(Host *h) {
    if (!h->disp || !h->h) return;
    RvgDisplayCoord at = rvg_display_cursor_position(h->disp);
    HvReimsHeader *x = h->h;
    uint32_t seq = __atomic_load_n(&x->cursor_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&x->cursor_seq, seq | 1u, __ATOMIC_RELEASE);
    x->cursor_x = at.x, x->cursor_y = at.y, x->cursor_valid = 1;
    __atomic_store_n(&x->cursor_seq, (seq | 1u) + 1u, __ATOMIC_RELEASE);
}

static void on_move(void *ctx) {
    Host *h = ctx;
    publish_cursor(h);
    pthread_mutex_lock(&h->lock);
    h->cursor_changed = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
}

/* The pointer over the frame: premultiplied RGBA over RGBA. Lock held. */
static void compose(Host *h) {
    size_t bytes = (size_t)h->tw * h->th * 4;
    memcpy(h->shown, h->pixels, bytes);
    if (!h->cursor_on || !h->glyph || !h->disp) return;
    RvgDisplayCoord at = rvg_display_cursor_position(h->disp);
    int x0 = (int)at.x - h->hx, y0 = (int)at.y - h->hy;
    for (uint32_t cy = 0; cy < h->gh; cy++) {
        int y = y0 + (int)cy;
        if (y < 0 || y >= (int)h->th) continue;
        const uint8_t *s = h->glyph + (size_t)cy * h->gw * 4;
        for (uint32_t cx = 0; cx < h->gw; cx++, s += 4) {
            int x = x0 + (int)cx;
            if (x < 0 || x >= (int)h->tw) continue;
            uint8_t *d = h->shown + ((size_t)y * h->tw + (size_t)x) * 4;
            uint32_t keep = 255u - s[3];
            for (int k = 0; k < 3; k++) d[k] = (uint8_t)(s[k] + (d[k] * keep + 127) / 255);
        }
    }
}

static int make_target(Host *h, uint32_t w, uint32_t hgt) {
    if (h->target && h->tw == w && h->th == hgt) return 0;
    if (h->target) rvg_frame_target_release(h->target), h->target = NULL;
    RvgFrameTargetConfig c = {sizeof c, w, hgt, RVG_PIXEL_FORMAT_RGBA8_UNORM};
    RvgStatus st = rvg_frame_target_create(h->dev, &c, &h->target);
    if (st != RVG_OK) { hlog("reims: frame target %ux%u: %s", w, hgt, rvg_status_message(st)); return -1; }
    free(h->pixels), free(h->shown);
    h->pixels = calloc((size_t)w * hgt, 4), h->shown = calloc((size_t)w * hgt, 4);
    h->tw = w, h->th = hgt;
    return h->pixels && h->shown ? 0 : -1;
}

/* Frames are encoded only while a screen is attached; the guest's presents complete either way. */
static void *render_loop(void *arg) {
    Host *h = arg;
    while (!h->stop) {
        pthread_mutex_lock(&h->lock);
        while (!h->stop && !h->frame_pending && !h->cursor_changed) pthread_cond_wait(&h->wake, &h->lock);
        int frame = h->frame_pending, mode = h->mode_changed;
        uint32_t w = h->mode_w, hgt = h->mode_h;
        h->frame_pending = h->cursor_changed = h->mode_changed = 0;
        Replay *sink = h->sink;
        pthread_mutex_unlock(&h->lock);
        if (h->stop || !sink || !h->disp || !w || !hgt) continue;
        if ((mode || !h->target) && make_target(h, w, hgt) != 0) continue;
        uint64_t t0 = now_ns(), t1 = t0, t2 = t0;
        if (frame) {
            RvgFrame *f = NULL;
            RvgStatus st = rvg_display_encode_frame(h->disp, h->target, NULL, &f);
            if (st != RVG_OK) { hlog("reims: encode: %s", rvg_status_message(st)); continue; }
            rvg_frame_commit(f);
            rvg_frame_wait(f);
            rvg_frame_release(f);
            t1 = now_ns();
            size_t stride = (size_t)h->tw * 4;
            st = rvg_frame_target_read_pixels(h->target, h->pixels, stride, stride * h->th);
            if (st != RVG_OK) { hlog("reims: read pixels: %s", rvg_status_message(st)); continue; }
            h->frames++;
            t2 = now_ns();
        }
        pthread_mutex_lock(&h->lock);
        compose(h);
        if (h->sink) replay_show_frame(h->sink, h->shown, h->tw, h->th); /* under the lock: set_sink waits for it */
        /* Where a frame's time goes, for the log (beat_loop): drawing it to the target and waiting for the
           GPU, reading it back, then the pointer and the tablet's screen. */
        if (frame) h->t_encode += t1 - t0, h->t_read += t2 - t1, h->t_frames++;
        h->t_show += now_ns() - t2, h->t_shows++;
        pthread_mutex_unlock(&h->lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the MMIO slots -- */
#if defined(__aarch64__)
#define cpu_relax() __asm__ volatile("yield")
#else
#define cpu_relax() ((void)0)
#endif

/* One register access's times into the log's counts: `reach` from the engine's post to this side taking it,
   `device` inside the library; per register (device, direction, offset) how many and how long. */
static void reg_note(Host *h, const HvReimsSlot *s, uint64_t reach, uint64_t device) {
    uint64_t key = (uint64_t)s->device << 62 | (uint64_t)(s->write & 1) << 61 | (s->offset & ((1ull << 61) - 1));
    key |= 1ull << 60; /* never 0: an empty entry */
    pthread_mutex_lock(&h->reg_lock);
    h->reg_n++, h->reg_reach_ns += reach, h->reg_device_ns += device;
    int n = (int)(sizeof h->reg / sizeof h->reg[0]), at = -1;
    for (int i = 0; i < n; i++) {
        if (h->reg[i].key == key) { at = i; break; }
        if (!h->reg[i].key && at < 0) at = i;
    }
    if (at >= 0) {
        h->reg[at].key = key, h->reg[at].n++, h->reg[at].ns += device;
        if (device > h->reg[at].max_ns) h->reg[at].max_ns = device;
    }
    pthread_mutex_unlock(&h->reg_lock);
}

/* The log's line of them every 10 s (beat_loop), and the counts start again. */
static void reg_report(Host *h) {
    pthread_mutex_lock(&h->reg_lock);
    uint64_t count = h->reg_n, reach = h->reg_reach_ns, device = h->reg_device_ns;
    int n = (int)(sizeof h->reg / sizeof h->reg[0]);
    char top[400] = "";
    size_t m = 0;
    for (int k = 0; k < 4; k++) { /* the four registers that took the most time */
        int best = -1;
        for (int i = 0; i < n; i++)
            if (h->reg[i].key && (best < 0 || h->reg[i].ns > h->reg[best].ns)) best = i;
        if (best < 0 || !h->reg[best].ns) break;
        uint64_t key = h->reg[best].key;
        m += (size_t)snprintf(top + m, sizeof top - m, "%s%s %s +%#llx %llux %.0f ms (longest %.1f)", k ? ", " : "",
                              key >> 62 ? "iosurface" : "gfx", key >> 61 & 1 ? "write" : "read",
                              (unsigned long long)(key & ((1ull << 60) - 1)), (unsigned long long)h->reg[best].n,
                              h->reg[best].ns / 1e6, h->reg[best].max_ns / 1e6);
        h->reg[best].key = 0;
        if (m >= sizeof top) break;
    }
    memset(h->reg, 0, sizeof h->reg);
    h->reg_n = h->reg_reach_ns = h->reg_device_ns = 0;
    pthread_mutex_unlock(&h->reg_lock);
    if (count)
        hlog("reims: registers: %llu accesses; %.0f ms to reach the app (%.0f us each), %.0f ms in the device (%.0f us each); most: %s",
             (unsigned long long)count, reach / 1e6, reach / 1e3 / count, device / 1e6, device / 1e3 / count, top[0] ? top : "none");
}

static void *slot_loop(void *arg) {
    HvReimsSlot *s = arg;
    Host *h = H;
    /* Ahead of the app's ordinary threads: a CPU of the Mac's stands still until this one answers. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -10);
    uint64_t idle_since = now_ns();
    while (!h->stop) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) {
            /* Accesses come in runs: stay awake for 150 us after one before sleeping, so the next is taken
               without the scheduler (the engine's side waits as long for the answer, pvg_reims.c). */
            if (now_ns() - idle_since < 150000) {
                cpu_relax();
                continue;
            }
            struct timespec t = {0, 200 * 1000000};
            futex(&s->state, FUTEX_WAIT, HV_REIMS_IDLE, &t);
            idle_since = now_ns() - 150000; /* woken: look once, then sleep again unless there is work */
            continue;
        }
        uint64_t taken = now_ns(), sent = s->sent_ns;
        HvReimsSlot asked = *s; /* the engine may post its next request the moment this one is answered */
        uint64_t v = 0;
        if (s->device == HV_REIMS_GFX) {
            if (s->write) rvg_device_mmio_write(h->dev, s->offset, (uint32_t)s->value);
            else v = rvg_device_mmio_read(h->dev, s->offset);
        } else {
            /* The mapper's setup registers, a handful of writes per boot: their values are what the first
               death past device creation turned on (a ring size the library would not map). */
            if (s->write && (s->offset == 0x1000 || s->offset == 0x1008 || s->offset == 0x1010 || s->offset == 0x1030))
                hlog("reims: iosurface mapper: %s = %#llx (%u-byte write)",
                     s->offset == 0x1000 ? "ring base" : s->offset == 0x1008 ? "ring size (entries)" :
                     s->offset == 0x1010 ? "descriptor table" : "descriptor enable",
                     (unsigned long long)s->value, s->size);
            if (s->write) rvg_iosurface_device_mmio_write(h->iosfc, s->offset, s->value);
            else v = rvg_iosurface_device_mmio_read(h->iosfc, s->offset);
        }
        s->value = v;
        h->mmio++;
        __atomic_store_n(&s->state, HV_REIMS_REPLY, __ATOMIC_RELEASE);
        futex(&s->state, FUTEX_WAKE, 1, NULL);
        idle_since = now_ns();
        reg_note(h, &asked, sent && taken > sent ? taken - sent : 0, idle_since - taken);
    }
    return NULL;
}

static void *beat_loop(void *arg) {
    Host *h = arg;
    uint64_t last = now_ns();
    while (!h->stop) {
        if (h->h) {
            __atomic_store_n(&h->h->app_heartbeat, now_ns(), __ATOMIC_RELEASE);
            if (h->ready && !h->stop) __atomic_store_n(&h->h->app_ready, 1, __ATOMIC_RELEASE); /* survives a header reset */
        }
        usleep(250000);
        static uint64_t sampled_at;
        static int samples;
        if (sample_wanted && h->ready && samples < 10 && now_ns() - sampled_at > 30000000000ull) {
            sampled_at = now_ns(), samples++;
            sample_threads();
        }
        sample_wanted = 0;
        if (now_ns() - last > 10000000000ull && h->ready) {
            uint64_t guest = 0, host = 0;
            if (h->disp) rvg_display_present_counts(h->disp, &guest, &host);
            pthread_mutex_lock(&h->lock);
            uint64_t n = h->t_frames, s = h->t_shows, te = h->t_encode, tr = h->t_read, ts = h->t_show;
            h->t_frames = h->t_shows = h->t_encode = h->t_read = h->t_show = 0;
            pthread_mutex_unlock(&h->lock);
            double secs = (now_ns() - last) / 1e9;
            hlog("reims: %llu MMIO accesses, %llu interrupts; %llu presents, %llu frames shown; "
                 "%.1f frames/s, each %.1f ms to draw, %.1f ms to read back, %.1f ms to show",
                 (unsigned long long)h->mmio, (unsigned long long)h->h->irqs, (unsigned long long)guest, (unsigned long long)h->frames,
                 n / secs, n ? te / 1e6 / n : 0.0, n ? tr / 1e6 / n : 0.0, s ? ts / 1e6 / s : 0.0);
            last = now_ns();
            reg_report(h);
        }
    }
    return NULL;
}

/* The Mac's RAM, mapped where its first byte is on a 2 MiB boundary. reims-vgpu runs each of the Mac's GPU
   tasks but task 0 apart ("device isolates every task but task 0"), and refuses, without a word, to make such
   a task unless the memory map's host address is whole pages of the GUEST's size, 16 KiB (tasks/remote.rs,
   whole_pages). The tablet's pages are 4 KiB, so a plain mmap lands on a 16 KiB boundary only when its
   neighbour happens to: it always had until 0.3.26, where every task failed ("command 56 faulted: task 1 not
   created", then "no task" for each of its commands) and nothing was drawn. So: reserve the size and 2 MiB
   more, map the file over the aligned part of it, and give back the rest. */
static uint8_t *map_ram_aligned(int fd, uint64_t size) {
    const uint64_t align = 2ull << 20;
    uint8_t *reserved = mmap(NULL, size + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED) return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t *at = (uint8_t *)(((uintptr_t)reserved + align - 1) & ~(uintptr_t)(align - 1));
    uint8_t *ram = mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (ram == MAP_FAILED) {
        int e = errno;
        munmap(reserved, size + align);
        errno = e;
        return MAP_FAILED;
    }
    if (at > reserved) munmap(reserved, (size_t)(at - reserved));
    uint8_t *end = at + size, *reserved_end = reserved + size + align;
    if (reserved_end > end) munmap(end, (size_t)(reserved_end - end));
    /* Settings > Huge pages: the engine asks for the Mac's RAM in 2 MB pages; a page first touched from this
       side (the GPU reading a surface the Mac has not written yet) is then huge too, and this mapping, on its
       2 MB boundary, maps the huge ones as such. Nothing where the kernel's shared-memory huge pages are off. */
    madvise(ram, size, MADV_HUGEPAGE);
    return ram;
}

/* Whether the file at `path` holds the bytes of `needle` anywhere (a driver's own strings). */
static int file_holds(const char *path, const char *needle) {
    int fd = open(path, O_RDONLY | O_CLOEXEC), found = 0;
    struct stat st;
    size_t n = strlen(needle);
    if (fd < 0) return 0;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size >= n && n) {
        const char *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            for (const char *at = p, *end = p + st.st_size - n; at <= end && !found; at++) {
                at = memchr(at, needle[0], (size_t)(end - at) + 1);
                if (!at) break;
                found = !memcmp(at, needle, n);
            }
            munmap((void *)p, (size_t)st.st_size);
        }
    }
    close(fd);
    return found;
}

/* The Mac's memory in place (Settings > Zero-copy, a kernel with /dev/udmabuf): whether the Vulkan driver at
   `driver` may be given dma-bufs of the Mac's own pages to draw from and into, instead of copies of them.
   metal2vulkan never flushes or invalidates around such an import (it takes it for coherent with the CPU's
   caches, as on the desktop GPUs it was written against), and the Mac's CPUs write those pages through cached
   mappings. Turnip's stock import of a dma-buf on KGSL is not I/O-coherent, so the GPU would read main memory
   while what the Mac just drew sits in a CPU cache: stale blocks on the screen. Henge's own build of Turnip
   (android/gpu/turnip) imports them I/O-coherent where TU_KGSL_IMPORT_COHERENT asks and refuses an import the
   kernel does not keep so; it carries the variable's name, which is how it is told from a stock build. So:
   in place on that driver alone (M2V_DMABUF_INPLACE=1, which m2v_no_host_import.py reads), copies on any
   other. Two files in the home change it (Settings, "Mac's memory in place"): in-place-off (always copies) and
   in-place-force (in place on a Turnip that is not known to be coherent: to see what it does, not to use). */
static void in_place_policy(Host *h, const char *driver) {
    unsetenv("M2V_DMABUF_INPLACE"), unsetenv("TU_KGSL_IMPORT_COHERENT");
    if (!driver || !*driver) return; /* the system's driver imports no dma-bufs: copies, nothing to decide */
    char off[1100] = "", force[1100] = "";
    if (h->home[0]) snprintf(off, sizeof off, "%s/in-place-off", h->home), snprintf(force, sizeof force, "%s/in-place-force", h->home);
    int coherent = file_holds(driver, "TU_KGSL_IMPORT_COHERENT");
    if (off[0] && access(off, F_OK) == 0) {
        hlog("reims: the Mac's memory in place: off (Settings); the GPU works on copies");
    } else if (coherent) {
        setenv("TU_KGSL_IMPORT_COHERENT", "require", 1);
        setenv("M2V_DMABUF_INPLACE", "1", 1);
        hlog("reims: the Mac's memory in place: this Turnip imports dma-bufs I/O-coherent (Henge's build), so the GPU "
             "uses the Mac's pages where /dev/udmabuf gives them (Settings > Zero-copy) and copies the rest; "
             "its \"turnip: dma-buf imports:\" line says what the kernel kept");
    } else if (force[0] && access(force, F_OK) == 0) {
        setenv("M2V_DMABUF_INPLACE", "1", 1);
        hlog("reims: the Mac's memory in place: FORCED (Settings) on a driver not known to import coherently with the "
             "CPU's caches: stale or torn pixels are expected");
    } else {
        hlog("reims: the Mac's memory in place: not on this driver (a stock Turnip's dma-buf imports are not coherent "
             "with the CPU's caches); the GPU works on copies");
    }
}

/* ------------------------------------------------------------------ bring-up -- */
static int bring_up(Host *h) {
    HvReimsHeader *x = h->h;
    set_stage("found the engine's file; opening guest RAM");
    /* The engine may still be handing the RAM file over (owner, SELinux label): retry for 30 s, not once. */
    for (int i = 0; !h->stop; i++) {
        /* '@...': the engine's in-memory RAM (HV_RAM_MEMFD), handed over as a descriptor (../ram_fd.h). */
        h->ram_fd = x->ram_file[0] == '@' ? hv_ram_fd_take(x->ram_file) : open(x->ram_file, O_RDWR | O_CLOEXEC);
        if (h->ram_fd >= 0) {
            if (x->ram_file[0] == '@') hlog("reims: the Mac's RAM is in memory: the engine handed it over (%s)", x->ram_file);
            break;
        }
        if (i == 0 || i == 300) hlog("reims: guest RAM %s: %s%s", x->ram_file, strerror(errno), i ? "; giving up" : "; retrying");
        /* The engine's in-memory RAM (HV_RAM_MEMFD) comes as a descriptor over the engine's socket ('@...'), or
           is opened through /proc/<engine>/fd where the engine had no socket: where Android refuses either,
           leave a note the app reads at the next start, which then asks for the RAM file as before. */
        if (i == 30 && (x->ram_file[0] == '@' || !strncmp(x->ram_file, "/proc/", 6)) && h->home[0]) {
            char note[1100];
            snprintf(note, sizeof note, "%s/ram-memfd-failed", h->home);
            int nf = open(note, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (nf >= 0) close(nf);
            hlog("reims: the engine's in-memory RAM cannot be opened from the app; the next start uses the RAM file");
            return -1;
        }
        if (i == 300) return -1;
        usleep(100000);
    }
    if (h->ram_fd < 0) return -1;
    h->ram_pa = x->ram_pa, h->ram_size = x->ram_size;
    set_stage("mapping guest RAM");
    h->ram = map_ram_aligned(h->ram_fd, h->ram_size);
    if (h->ram == MAP_FAILED) { h->ram = NULL; hlog("reims: mapping guest RAM: %s", strerror(errno)); return -1; }
    hlog("reims: guest RAM mapped at %p (%s the guest's 16 KiB pages)", (void *)h->ram,
         (uintptr_t)h->ram & 0x3fff ? "NOT on" : "on");

    RvgMemoryRange range = {h->ram_pa, h->ram_size, h->ram};
    RvgMemoryMap map = {&range, 1};
    RvgMemoryRangeFile file = {h->ram_fd, 0, 0};
    RvgHostCallbacks cb = {.struct_size = sizeof cb, .ctx = h, .raise_interrupt = gfx_raise};
    RvgDeviceConfig dc = {
        .struct_size = sizeof dc,
        .display_port_count = 1,
        .use_iosurface_mapper = true, /* vmapple: the IOSurface device maps the guest's IOSurfaces */
        .process_isolation = true,    /* as QEMU's apple-gfx-mmio */
        /* Argument buffers are offered only on a device that can detach backing; on this Vulkan device the
           path has never run, and the plain path has: off, as a conservative first bring-up. */
        .disable_argument_buffers = true,
        .memory_map = &map,
        .memory_files = &file,
        .callbacks = &cb,
    };
    /* metal2vulkan keeps its translated shaders on disk and refuses to make a device without a directory
       for them: $HOME/.cache/reims-vgpu-vulkan, and an Android app's process has no HOME at all
       ("the Vulkan runtime refused it: no shader cache directory: no home directory", a reims-vgpu fatal
       that took the app down at the first start on the tablet). The app passes a folder of its own that
       outlives the process, so the second boot finds the first's pipelines (macOS-uke's lesson: its
       persistent cache is what made WindowServer usable). Set before the first device: reims-vgpu reads
       HOME once, when it builds its device options. */
    if (h->home[0]) {
        mkdir(h->home, 0700);
        setenv("HOME", h->home, 1);
        setenv("XDG_CACHE_HOME", h->home, 0);
        /* What the last boots left: a cache that is found again is why the second boot is quicker than the first. */
        cache_files = 0, cache_bytes = 0;
        nftw(h->home, cache_count, 16, FTW_PHYS);
        hlog("reims: shader cache under %s: %llu files, %.1f MB from earlier runs%s", h->home, cache_files, cache_bytes / 1048576.0,
             cache_files ? "" : " (none yet: this boot translates every shader, the next finds them here)");
    } else if (!getenv("HOME")) {
        hlog("reims: no HOME and no cache folder given: metal2vulkan will refuse the device");
    }
    /* Settings, GPU memory checks: Everything. The app leaves a file in the home; metal2vulkan then compares
       every copied host buffer at each commit, as upstream does, not only those the commands use
       (m2v_copy_in_used.py): slower, and the answer to a window that shows stale pixels. */
    if (h->home[0]) {
        char all[1100];
        snprintf(all, sizeof all, "%s/copy-in-all", h->home);
        if (access(all, F_OK) == 0) {
            setenv("M2V_COPY_IN_ALL", "1", 1);
            hlog("reims: GPU memory checks: everything, at each commit (Settings)");
        } else {
            unsetenv("M2V_COPY_IN_ALL");
        }
    }
    /* The Vulkan driver: the system's through the loader, or the app's own (Turnip) opened directly as
       the ICD (metal2vulkan reads M2V_VULKAN_LIBRARY, Henge's port). */
    unsetenv("M2V_VULKAN_LIBRARY"), unsetenv("M2V_VULKAN_GIPA");
    in_place_policy(h, NULL);
    if (h->vulkan[0]) {
        /* The app's own driver needs platform libraries an app's namespace is refused: opened as Android
           opens a vendor driver (turnip_load.cpp), and metal2vulkan takes its entry point. */
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", h->vulkan);
        char *slash = strrchr(dir, '/');
        const char *name = slash ? slash + 1 : dir;
        if (slash) *slash = 0;
        void *gipa = reims_turnip_open(slash ? dir : ".", name);
        if (gipa) {
            char hex[32];
            snprintf(hex, sizeof hex, "%#llx", (unsigned long long)(uintptr_t)gipa);
            setenv("M2V_VULKAN_GIPA", hex, 1);
            hlog("reims: Vulkan driver: %s (%s)", h->vulkan, reims_turnip_error());
            in_place_policy(h, h->vulkan);
        } else {
            hlog("reims: Vulkan driver: %s did not open (%s); the system's instead", h->vulkan, reims_turnip_error());
            h->vulkan[0] = 0;
        }
    }
    if (!h->vulkan[0]) hlog("reims: Vulkan driver: the system's (libvulkan.so)");
    set_stage("rvg_device_create (Vulkan device, guest memory map)");
    RvgStatus st = rvg_device_create(&dc, &h->dev);
    if (st != RVG_OK && h->vulkan[0]) {
        /* The app's own driver did not give a device (it did not open, or metal2vulkan refused it): the
           system's, so a bad Turnip build costs the exactness, not the run. */
        hlog("reims: rvg_device_create on %s: %s; trying the system's Vulkan driver", h->vulkan, rvg_status_message(st));
        unsetenv("M2V_VULKAN_LIBRARY"), unsetenv("M2V_VULKAN_GIPA");
        in_place_policy(h, NULL);
        h->vulkan[0] = 0;
        set_stage("rvg_device_create (the system's Vulkan driver, after the app's own failed)");
        st = rvg_device_create(&dc, &h->dev);
    }
    if (st != RVG_OK) { hlog("reims: rvg_device_create: %s", rvg_status_message(st)); return -1; }
    RvgIOSurfaceCallbacks ic = {.struct_size = sizeof ic, .ctx = h, .raise_interrupt = iosfc_raise};
    RvgIOSurfaceDeviceConfig icfg = {.struct_size = sizeof icfg, .memory_map = &map, .callbacks = &ic};
    set_stage("rvg_iosurface_device_create");
    st = rvg_iosurface_device_create(&icfg, &h->iosfc);
    if (st != RVG_OK) { hlog("reims: rvg_iosurface_device_create: %s", rvg_status_message(st)); return -1; }

    uint32_t w = x->display_w ? x->display_w : 1920, hgt = x->display_h ? x->display_h : 1080;
    RvgDisplayCallbacks disp_cb = {.struct_size = sizeof disp_cb, .ctx = h, .mode_change = on_mode, .new_frame = on_frame,
                                   .cursor_glyph = on_glyph, .cursor_show = on_show, .cursor_move = on_move};
    /* 110 pixels per inch, as reims-vgpu offers a mode with no host screen: a 1x desktop. */
    RvgDisplayConfig disp = {.struct_size = sizeof disp, .serial_number = 1, .port = 0,
                             .width_millimeters = w / (110.0 / 25.4), .height_millimeters = hgt / (110.0 / 25.4),
                             .callbacks = &disp_cb};
    set_stage("rvg_display_create");
    st = rvg_display_create(h->dev, &disp, &h->disp);
    if (st != RVG_OK) { hlog("reims: rvg_display_create: %s", rvg_status_message(st)); return -1; }
    /* The engine's mode first (macOS takes the first), then the others as alternatives. */
    RvgDisplayMode modes[] = {{(uint16_t)w, (uint16_t)hgt, 0, 60.0}, {1280, 800, 0, 60.0}, {1920, 1080, 0, 60.0},
                              {1024, 768, 0, 60.0}, {1600, 1000, 0, 60.0}};
    size_t nmodes = 1;
    for (size_t i = 1; i < sizeof modes / sizeof *modes; i++)
        if (modes[i].width != w || modes[i].height != hgt) modes[nmodes++] = modes[i];
    rvg_display_set_modes(h->disp, modes, nmodes);
    h->mode_w = w, h->mode_h = hgt;

    for (int i = 0; i < HV_REIMS_SLOTS; i++)
        if (pthread_create(&h->slot[i], NULL, slot_loop, &x->slot[i]) == 0) h->nslot_threads = i + 1;
    pthread_create(&h->render, NULL, render_loop, h);
    h->ready = 1;
    stage = "running";
    x->app_pid = (uint32_t)getpid();
    __atomic_store_n(&x->app_ready, 1, __ATOMIC_RELEASE);
    hlog("reims: the GPU is up: guest RAM %llu MB, display %ux%u", (unsigned long long)(h->ram_size >> 20), w, hgt);
    return 0;
}

/* Waits for the engine's file, then brings the device up. */
static void *attach_loop(void *arg) {
    Host *h = arg;
    while (!h->stop) {
        int fd = open(h->path, O_RDWR | O_CLOEXEC);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= (off_t)HV_REIMS_FILE_BYTES) {
            HvReimsHeader *x = mmap(NULL, HV_REIMS_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (x != MAP_FAILED && __atomic_load_n(&x->magic, __ATOMIC_ACQUIRE) == HV_REIMS_MAGIC && x->version == HV_REIMS_VERSION &&
                __atomic_load_n(&x->engine_alive, __ATOMIC_ACQUIRE) && x->ram_file[0]) {
                h->h = x;
                pthread_create(&h->beat, NULL, beat_loop, h);
                if (bring_up(h) != 0) hlog("reims: no GPU this run");
                return NULL;
            }
            if (x != MAP_FAILED) {
                if (!h->told++) hlog("reims: the engine's file is there but not ready (magic %08x, version %u, engine_alive %u, ram_file %s)",
                                     x->magic, x->version, x->engine_alive, x->ram_file[0] ? "set" : "empty");
                munmap(x, HV_REIMS_FILE_BYTES);
            }
        } else if (fd >= 0) {
            close(fd);
        } else if (errno != ENOENT && !h->told++) {
            hlog("reims: %s: %s", h->path, strerror(errno));
        }
        usleep(100000);
    }
    return NULL;
}

int reims_host_start(const char *path, const char *log_path, const char *home, const char *vulkan_lib) {
    pthread_mutex_lock(&host_lock);
    if (H) { pthread_mutex_unlock(&host_lock); return 0; }
    Host *h = calloc(1, sizeof *h);
    if (!h) { pthread_mutex_unlock(&host_lock); return -1; }
    snprintf(h->path, sizeof h->path, "%s", path);
    if (home && *home) snprintf(h->home, sizeof h->home, "%s", home);
    if (vulkan_lib && *vulkan_lib) snprintf(h->vulkan, sizeof h->vulkan, "%s", vulkan_lib);
    if (log_fd < 0 && log_path && *log_path) log_fd = open(log_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    /* reims-vgpu reports a panic on stderr and aborts; an app's stderr is /dev/null, so the reason was lost. */
    if (log_fd >= 0) dup2(log_fd, 2);
    h->ram_fd = -1;
    pthread_mutex_init(&h->lock, NULL);
    pthread_mutex_init(&h->reg_lock, NULL);
    pthread_cond_init(&h->wake, NULL);
    rvg_log_set_handler(on_log, NULL, RVG_LOG_INFO);
    install_crash_log();
    stage = "waiting for the engine's file";
    H = h;
    pthread_create(&h->attach, NULL, attach_loop, h);
    pthread_mutex_unlock(&host_lock);
    hlog("reims: waiting for the engine at %s", path);
    return 0;
}

void reims_host_stop(void) {
    pthread_mutex_lock(&host_lock);
    Host *h = H;
    H = NULL;
    pthread_mutex_unlock(&host_lock);
    if (!h) return;
    h->stop = 1;
    pthread_join(h->attach, NULL);
    pthread_mutex_lock(&h->lock);
    pthread_cond_broadcast(&h->wake);
    pthread_mutex_unlock(&h->lock);
    if (h->ready) pthread_join(h->render, NULL);
    for (int i = 0; i < h->nslot_threads; i++) {
        futex(&h->h->slot[i].state, FUTEX_WAKE, 1, NULL);
        pthread_join(h->slot[i], NULL);
    }
    if (h->h) {
        __atomic_store_n(&h->h->app_ready, 0, __ATOMIC_RELEASE);
        pthread_join(h->beat, NULL);
    }
    if (h->target) rvg_frame_target_release(h->target);
    if (h->disp) rvg_display_release(h->disp);
    if (h->iosfc) rvg_iosurface_device_release(h->iosfc);
    if (h->dev) rvg_device_release(h->dev);
    if (h->ram) munmap(h->ram, h->ram_size);
    if (h->ram_fd >= 0) close(h->ram_fd);
    if (h->h) munmap(h->h, HV_REIMS_FILE_BYTES);
    free(h->pixels), free(h->shown), free(h->glyph);
    free(h);
}

int reims_host_running(void) { return H != NULL; }

void reims_host_set_sink(Replay *r) {
    pthread_mutex_lock(&host_lock);
    Host *h = H;
    if (h) {
        pthread_mutex_lock(&h->lock);
        h->sink = r;
        h->frame_pending = 1; /* draw the current frame for the new screen */
        pthread_cond_signal(&h->wake);
        pthread_mutex_unlock(&h->lock);
    }
    pthread_mutex_unlock(&host_lock);
}
