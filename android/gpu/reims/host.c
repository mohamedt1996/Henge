/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims, the app's half (../reims_link.h): reims-vgpu is macOS's paravirtual GPU device here,
   in the app's process, on Android's Vulkan driver, and the engine forwards the device's MMIO to it.
   This file is the host reims-vgpu's C API asks for, shaped like QEMU's apple-gfx-mmio.c on the same API:
   guest RAM as one memory-map range over the RAM file, the IOSurface mapper device beside the GPU, the
   two interrupt lines pulsed per raise, one display whose frames are drawn into a frame target and
   handed to the replay's presenter (replay_show_frame) for the screen. */
#include "../reims_link.h"
#include "../replay/replay.h"
#include "reims_vgpu.h"
#include "reims_host.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char path[1024];
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

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static long futex(uint32_t *word, int op, uint32_t val, const struct timespec *t) {
    return syscall(SYS_futex, word, op, val, t, NULL, 0);
}

static void on_log(void *ctx, RvgLogLevel level, const char *target, size_t tl, const char *msg, size_t ml) {
    (void)ctx, (void)level;
    hlog("reims %.*s: %.*s", (int)tl, target, (int)ml, msg);
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

static void on_show(void *ctx, uint8_t show) {
    Host *h = ctx;
    pthread_mutex_lock(&h->lock);
    h->cursor_on = show != 0, h->cursor_changed = 1;
    pthread_cond_signal(&h->wake);
    pthread_mutex_unlock(&h->lock);
}

static void on_move(void *ctx) {
    Host *h = ctx;
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
        if (frame) {
            RvgFrame *f = NULL;
            RvgStatus st = rvg_display_encode_frame(h->disp, h->target, NULL, &f);
            if (st != RVG_OK) { hlog("reims: encode: %s", rvg_status_message(st)); continue; }
            rvg_frame_commit(f);
            rvg_frame_wait(f);
            rvg_frame_release(f);
            size_t stride = (size_t)h->tw * 4;
            st = rvg_frame_target_read_pixels(h->target, h->pixels, stride, stride * h->th);
            if (st != RVG_OK) { hlog("reims: read pixels: %s", rvg_status_message(st)); continue; }
            h->frames++;
        }
        pthread_mutex_lock(&h->lock);
        compose(h);
        if (h->sink) replay_show_frame(h->sink, h->shown, h->tw, h->th); /* under the lock: set_sink waits for it */
        pthread_mutex_unlock(&h->lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the MMIO slots -- */
static void *slot_loop(void *arg) {
    HvReimsSlot *s = arg;
    Host *h = H;
    while (!h->stop) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) {
            struct timespec t = {0, 200 * 1000000};
            futex(&s->state, FUTEX_WAIT, HV_REIMS_IDLE, &t);
            continue;
        }
        uint64_t v = 0;
        if (s->device == HV_REIMS_GFX) {
            if (s->write) rvg_device_mmio_write(h->dev, s->offset, (uint32_t)s->value);
            else v = rvg_device_mmio_read(h->dev, s->offset);
        } else {
            if (s->write) rvg_iosurface_device_mmio_write(h->iosfc, s->offset, s->value);
            else v = rvg_iosurface_device_mmio_read(h->iosfc, s->offset);
        }
        s->value = v;
        h->mmio++;
        __atomic_store_n(&s->state, HV_REIMS_REPLY, __ATOMIC_RELEASE);
        futex(&s->state, FUTEX_WAKE, 1, NULL);
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
        if (now_ns() - last > 10000000000ull && h->ready) {
            uint64_t guest = 0, host = 0;
            if (h->disp) rvg_display_present_counts(h->disp, &guest, &host);
            hlog("reims: %llu MMIO accesses, %llu interrupts; %llu presents, %llu frames shown",
                 (unsigned long long)h->mmio, (unsigned long long)h->h->irqs, (unsigned long long)guest, (unsigned long long)h->frames);
            last = now_ns();
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ bring-up -- */
static int bring_up(Host *h) {
    HvReimsHeader *x = h->h;
    /* The engine may still be handing the RAM file over (owner, SELinux label): retry for 30 s, not once. */
    for (int i = 0; !h->stop; i++) {
        h->ram_fd = open(x->ram_file, O_RDWR | O_CLOEXEC);
        if (h->ram_fd >= 0) break;
        if (i == 0 || i == 300) hlog("reims: guest RAM %s: %s%s", x->ram_file, strerror(errno), i ? "; giving up" : "; retrying");
        if (i == 300) return -1;
        usleep(100000);
    }
    if (h->ram_fd < 0) return -1;
    h->ram_pa = x->ram_pa, h->ram_size = x->ram_size;
    h->ram = mmap(NULL, h->ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, h->ram_fd, 0);
    if (h->ram == MAP_FAILED) { h->ram = NULL; hlog("reims: mapping guest RAM: %s", strerror(errno)); return -1; }

    RvgMemoryRange range = {h->ram_pa, h->ram_size, h->ram};
    RvgMemoryMap map = {&range, 1};
    RvgMemoryRangeFile file = {h->ram_fd, 0, 0};
    RvgHostCallbacks cb = {.struct_size = sizeof cb, .ctx = h, .raise_interrupt = gfx_raise};
    RvgDeviceConfig dc = {
        .struct_size = sizeof dc,
        .display_port_count = 1,
        .use_iosurface_mapper = true, /* vmapple: the IOSurface device maps the guest's IOSurfaces */
        .process_isolation = true,    /* as QEMU's apple-gfx-mmio */
        .memory_map = &map,
        .memory_files = &file,
        .callbacks = &cb,
    };
    RvgStatus st = rvg_device_create(&dc, &h->dev);
    if (st != RVG_OK) { hlog("reims: rvg_device_create: %s", rvg_status_message(st)); return -1; }
    RvgIOSurfaceCallbacks ic = {.struct_size = sizeof ic, .ctx = h, .raise_interrupt = iosfc_raise};
    RvgIOSurfaceDeviceConfig icfg = {.struct_size = sizeof icfg, .memory_map = &map, .callbacks = &ic};
    st = rvg_iosurface_device_create(&icfg, &h->iosfc);
    if (st != RVG_OK) { hlog("reims: rvg_iosurface_device_create: %s", rvg_status_message(st)); return -1; }

    uint32_t w = x->display_w ? x->display_w : 1920, hgt = x->display_h ? x->display_h : 1080;
    RvgDisplayCallbacks disp_cb = {.struct_size = sizeof disp_cb, .ctx = h, .mode_change = on_mode, .new_frame = on_frame,
                                   .cursor_glyph = on_glyph, .cursor_show = on_show, .cursor_move = on_move};
    /* 110 pixels per inch, as reims-vgpu offers a mode with no host screen: a 1x desktop. */
    RvgDisplayConfig disp = {.struct_size = sizeof disp, .serial_number = 1, .port = 0,
                             .width_millimeters = w / (110.0 / 25.4), .height_millimeters = hgt / (110.0 / 25.4),
                             .callbacks = &disp_cb};
    st = rvg_display_create(h->dev, &disp, &h->disp);
    if (st != RVG_OK) { hlog("reims: rvg_display_create: %s", rvg_status_message(st)); return -1; }
    RvgDisplayMode modes[] = {{(uint16_t)w, (uint16_t)hgt, 0, 60.0}, {1920, 1080, 0, 60.0}, {1280, 800, 0, 60.0}};
    rvg_display_set_modes(h->disp, modes, w == 1920 && hgt == 1080 ? 2 : 3);
    h->mode_w = w, h->mode_h = hgt;

    for (int i = 0; i < HV_REIMS_SLOTS; i++)
        if (pthread_create(&h->slot[i], NULL, slot_loop, &x->slot[i]) == 0) h->nslot_threads = i + 1;
    pthread_create(&h->render, NULL, render_loop, h);
    h->ready = 1;
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
            if (x != MAP_FAILED) munmap(x, HV_REIMS_FILE_BYTES);
        } else if (fd >= 0) {
            close(fd);
        }
        usleep(100000);
    }
    return NULL;
}

int reims_host_start(const char *path, const char *log_path) {
    pthread_mutex_lock(&host_lock);
    if (H) { pthread_mutex_unlock(&host_lock); return 0; }
    Host *h = calloc(1, sizeof *h);
    if (!h) { pthread_mutex_unlock(&host_lock); return -1; }
    snprintf(h->path, sizeof h->path, "%s", path);
    if (log_fd < 0 && log_path && *log_path) log_fd = open(log_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    h->ram_fd = -1;
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->wake, NULL);
    rvg_log_set_handler(on_log, NULL, RVG_LOG_INFO);
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
