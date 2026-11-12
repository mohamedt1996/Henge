/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: applegfx — a second, independent paravirtual-GPU host, so the GPU is not bet on reims alone. It
 * drives libapplegfx-vulkan (MattJackson/libapplegfx-vulkan, MIT): a clean-room host implementation of Apple's
 * ParavirtualizedGraphics with its own AIR -> SPIR-V translator and a Vulkan backend.
 *
 * Built with HAVE_APPLEGFX once the library is cross-built for the tablet (android/applegfx/build.sh, a CI step
 * like reims/build.sh). vmapple.c tries this before reims; both gate on their own HV_GPU_* variable.
 *
 * ========================= The MMIO <-> PCI transport bridge (android/applegfx/BRIDGE.md) ==================
 * libapplegfx-vulkan was written against Apple's *PCI* paravirtual-GPU transport (QEMU hw/display/apple-gfx-pci.m):
 * one PGDevice behind a single PCI BAR, interrupts delivered as MSI vectors. Henge's vmapple exposes the *MMIO*
 * transport that Apple-silicon macOS actually uses (QEMU hw/display/apple-gfx-mmio.m): the SAME PGDevice register
 * window (`iomem_gfx`) plus a SECOND window and device, the PGIOSurfaceHostDevice (`iomem_iosfc`), and two GIC SPI
 * lines instead of MSI.
 *
 * Evidence (QEMU sources, read directly) and the resulting mapping:
 *   - apple-gfx-pci.m registers exactly ONE BAR: `pci_register_bar(dev, PG_PCI_BAR_MMIO, ..., &s->common.iomem_gfx)`.
 *   - apple-gfx-mmio.m exposes `s->common.iomem_gfx` as sysbus MMIO #0 (the GFX window) AND a separate
 *     `s->iomem_iosfc` (0x10000) as MMIO #1 (the IOSFC window), backed by a distinct PGIOSurfaceHostDevice.
 *   - Both variants route the GFX window through the identical `apple_gfx_ops` -> `[pgdev mmioReadAtOffset:]`.
 *     The register offsets are therefore IDENTICAL across transports. => The GFX window maps 1:1 onto the
 *     library's `lagfx_mmio_read/write(dev, offset)` with NO offset translation. (The library reserves BAR
 *     0x0..0xfff for the PCI MSI-X table and keeps the real registers at 0x1000+; on MMIO that low 4 KiB is
 *     simply never touched because interrupts come over the GIC, so the identity mapping is exact.)
 *   - apple-gfx-pci.m delivers interrupts with `msi_notify(dev, vector)`; the PGDevice's raiseInterrupt block is
 *     the ONLY interrupt source. apple-gfx-mmio.m pulses `s->irq_gfx` from that same PGDevice block, and pulses a
 *     SEPARATE `s->irq_iosfc` only from the PGIOSurfaceHostDevice's own raiseInterrupt. => every interrupt the
 *     library raises (lagfx shell `raise_interrupt`, always MSI vector 0 for stamps/displays) belongs to the GFX
 *     line. The IOSFC line is owned by the IOSurface host device, which the library does not implement.
 *
 * What is wired here:
 *   - GFX window (VMAPPLE_GFX): each guest MMIO access is forwarded verbatim to lagfx_mmio_read/write. 8-byte
 *     accesses are split into the library's native 4-byte register ops.
 *   - Interrupts: the lagfx raise_interrupt callback edges the GFX GIC line (hv_gicv3_pend), waking a 1-CPU guest.
 *   - A ~60 Hz pump thread calls lagfx_timer_tick_vblank (display online/vblank IRQ) and lagfx_display_read_frame,
 *     publishing the BGRA frame through hv_pvg_applegfx_frame() which hv_vm_boot_display() presents (the same
 *     route pvg_lite uses).
 *   - The library's shell callbacks remain backed by the real engine: guest RAM (hv_ram_lookup) for
 *     read/write/map, and the library's own memfd+mmap task primitives.
 *
 * What is NOT resolved (the remaining blocker; see BRIDGE.md):
 *   - The IOSFC window (VMAPPLE_IOSFC / PGIOSurfaceHostDevice) has no counterpart in libapplegfx-vulkan. On
 *     Apple silicon macOS drives IOSurface sharing through THIS device (apple-gfx-mmio sets
 *     usingIOSurfaceMapper=true), not through the FIFO IOSurface opcodes (0x26-0x2a) the library implements for
 *     the PCI transport (usingIOSurfaceMapper=false). Its register protocol is not reverse-engineered in the
 *     library, so the window is answered by a logging stub: the bridge boots and the gfx command ring runs, but
 *     surfaces will not be backed and frames are unlikely until the IOSurface host device is implemented.
 *
 * Because of that, the bridge is engaged but honest: it logs its experimental status and points at Graphics:
 * reims for the known-working GPU. Set HV_GPU_APPLEGFX_PROBE_ONLY=1 to keep the old behaviour (prove the library
 * initialises on-device, then fall back to pvg_lite without touching the MMIO windows).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "machine.h"
#include "gicv3.h"
#include "vmapple.h"

#if defined(HAVE_APPLEGFX)
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "libapplegfx-vulkan.h"

/* The engine state the library's callbacks reach back into. */
struct applegfx_ctx {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    HvRam *ram;
    int ncpus;

    lagfx_device_t *dev;
    lagfx_display_t *disp;

    /* Published frame (double-buffered BGRA). The pump fills `back`, then under `flock` swaps it to `front`;
       hv_pvg_applegfx_frame() hands back `front`. Width/height track the created/mode-changed display size. */
    pthread_mutex_t flock;
    uint8_t *front, *back;
    size_t bufsize;
    uint32_t width, height;
    atomic_uint_fast32_t cur_w, cur_h; /* last mode the display reported (frame_ready/mode_changed) */
    int have_frame;

    pthread_t pump;
    int pump_running;
};

/* The task-memory callbacks wrap the library's own memfd+mmap primitives. */
static lagfx_task_t *applegfx_create_task(void *o, uint64_t vm_size, void **base_out) {
    (void)o;
    return lagfx_task_create(vm_size, base_out);
}
static void applegfx_destroy_task(void *o, lagfx_task_t *t) { (void)o; lagfx_task_destroy(t); }

/* Map guest-physical ranges into the task by aliasing the engine's guest-RAM mapping. */
static bool applegfx_map_memory(void *o, lagfx_task_t *t, uint64_t voff, const lagfx_physical_range_t *ranges,
                                size_t n, bool ro) {
    struct applegfx_ctx *C = o;
    uint64_t at = voff;
    for (size_t i = 0; i < n; i++) {
        uint64_t gpa = ranges[i].guest_physical_address, len = ranges[i].length;
        if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
        if (!lagfx_task_map_host_memory(t, at, C->ram->host + (gpa - C->ram->pa), len, ro)) return false;
        at += len;
    }
    return true;
}
static bool applegfx_unmap_memory(void *o, lagfx_task_t *t, uint64_t voff, uint64_t len) {
    (void)o;
    return lagfx_task_unmap(t, voff, len);
}
static bool applegfx_read_memory(void *o, uint64_t gpa, uint64_t len, void *dst) {
    struct applegfx_ctx *C = o;
    if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
    memcpy(dst, C->ram->host + (gpa - C->ram->pa), len);
    return true;
}
static bool applegfx_write_memory(void *o, uint64_t gpa, uint64_t len, const void *src) {
    struct applegfx_ctx *C = o;
    if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
    memcpy(C->ram->host + (gpa - C->ram->pa), src, len);
    return true;
}

/* The library (the PGDevice) raises MSI vector 0 for stamp/display completions; on the MMIO transport that is
   the single GFX GIC line (apple-gfx-mmio.m: desc.raiseInterrupt -> qemu_irq_pulse(irq_gfx)). hv_gicv3_pend is an
   edge, matching qemu_irq_pulse. The IOSFC line belongs to the (unimplemented) IOSurface host device. */
static void applegfx_raise_interrupt(void *o, uint32_t vector) {
    struct applegfx_ctx *C = o;
    (void)vector; /* every lagfx-raised interrupt is a PGDevice interrupt -> the GFX line */
    hv_gicv3_pend(C->gic, C->gfx_irq);
    if (C->ncpus == 1) hv_machine_wake(C->m);
}

/* Display callbacks: record the live mode so the pump knows the frame geometry. */
static void applegfx_frame_ready(void *o) { (void)o; }
static void applegfx_mode_changed(void *o, uint32_t w, uint32_t h) {
    struct applegfx_ctx *C = o;
    atomic_store(&C->cur_w, w);
    atomic_store(&C->cur_h, h);
}

/* ---- GFX window: forward to the library's BAR dispatch (identity offset, see header) -------------------- */
static uint64_t applegfx_gfx_read(void *o, uint64_t off, unsigned size) {
    struct applegfx_ctx *C = o;
    uint64_t v = 0;
    /* The library's registers are 4-byte; assemble wider (8-byte) accesses from native 4-byte reads. */
    for (unsigned done = 0; done < (size ? size : 4); done += 4)
        v |= (uint64_t)lagfx_mmio_read(C->dev, off + done) << (done * 8);
    return v;
}
static void applegfx_gfx_write(void *o, uint64_t off, uint64_t val, unsigned size) {
    struct applegfx_ctx *C = o;
    for (unsigned done = 0; done < (size ? size : 4); done += 4)
        lagfx_mmio_write(C->dev, off + done, (uint32_t)(val >> (done * 8)));
}

/* ---- IOSFC window: PGIOSurfaceHostDevice — not implemented by the library (see header/BRIDGE.md) -------- *
 * TODO(applegfx-iosfc): implement the PGIOSurfaceHostDevice MMIO protocol. macOS on Apple silicon maps guest
 * IOSurface memory through this window (iosfc mapMemory(phys,len,ro,&va)); without it the renderer has no
 * surface backing. The register layout is not reverse-engineered in libapplegfx-vulkan, so for now we answer
 * reads with 0 and swallow writes, logging the first accesses so a capture can begin the RE. */
static uint64_t applegfx_iosfc_read(void *o, uint64_t off, unsigned size) {
    struct applegfx_ctx *C = o;
    (void)C; (void)size;
    static unsigned seen;
    if (seen < 16) {
        fprintf(stderr, "hengevm: applegfx: IOSFC read off=0x%llx (unimplemented PGIOSurfaceHostDevice; returning 0)\n",
                (unsigned long long)off);
        seen++;
    }
    return 0;
}
static void applegfx_iosfc_write(void *o, uint64_t off, uint64_t val, unsigned size) {
    struct applegfx_ctx *C = o;
    (void)C; (void)size;
    static unsigned seen;
    if (seen < 16) {
        fprintf(stderr, "hengevm: applegfx: IOSFC write off=0x%llx val=0x%llx (unimplemented PGIOSurfaceHostDevice)\n",
                (unsigned long long)off, (unsigned long long)val);
        seen++;
    }
}

/* ---- ~60 Hz pump: vblank/online IRQ + frame read-out -------------------------------------------------- */
static void *applegfx_pump(void *arg) {
    struct applegfx_ctx *C = arg;
    const struct timespec interval = {0, 16666667}; /* ~60 Hz */
    for (;;) {
        /* Display online / vblank: fires the online IRQ (vector 0 -> GFX line) the moment the guest enables a
           display, which the deadlock fix in the library depends on. */
        lagfx_timer_tick_vblank(C->dev, C, applegfx_write_memory, applegfx_read_memory);

        /* Pull the latest rendered frame, if any, into the back buffer and publish it. */
        size_t stride = 0;
        bool newf = false;
        lagfx_status_t st = lagfx_display_read_frame(C->disp, C->back, C->bufsize, &stride, &newf);
        if (st == LAGFX_OK && newf) {
            pthread_mutex_lock(&C->flock);
            uint8_t *tmp = C->front;
            C->front = C->back;
            C->back = tmp;
            C->have_frame = 1;
            pthread_mutex_unlock(&C->flock);
        }

        nanosleep(&interval, NULL);
    }
    return NULL;
}

/* The newest applegfx frame (BGRA, width*4 bytes per row), or -1 if none yet. Mirrors hv_pvg_lite_frame(). */
int hv_pvg_applegfx_frame(const uint8_t **pixels, uint32_t *width, uint32_t *height);
static struct applegfx_ctx *g_ctx; /* published once the bridge is live */

int hv_pvg_applegfx_frame(const uint8_t **pixels, uint32_t *width, uint32_t *height) {
    struct applegfx_ctx *C = g_ctx;
    if (!C) return -1;
    int rc = -1;
    pthread_mutex_lock(&C->flock);
    if (C->have_frame) {
        *pixels = C->front;
        *width = C->width;
        *height = C->height;
        rc = 0;
    }
    pthread_mutex_unlock(&C->flock);
    return rc;
}
#endif /* HAVE_APPLEGFX */

int hv_pvg_applegfx_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    const char *path = getenv("HV_GPU_APPLEGFX");
    if (!path || !*path)
        return 0; /* a different backend was chosen; leave the device to it */

#if defined(HAVE_APPLEGFX)
    static struct applegfx_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.m = m, ctx.gic = gic, ctx.gfx_irq = gfx_irq, ctx.iosfc_irq = iosfc_irq;
    ctx.ncpus = m->ncpus;
    ctx.ram = hv_ram_lookup(m, VMAPPLE_MEM);
    if (!ctx.ram || !ctx.ram->host) {
        fprintf(stderr, "hengevm: applegfx: guest RAM is not host-mapped; using the built-in GPU instead\n");
        return 0;
    }

    fprintf(stderr, "hengevm: applegfx: libapplegfx-vulkan %u.%u.%u (%s)\n", lagfx_version_major(),
            lagfx_version_minor(), lagfx_version_patch(), lagfx_build_info());

    lagfx_device_descriptor_t desc;
    memset(&desc, 0, sizeof desc);
    desc.shell.opaque = &ctx;
    desc.shell.create_task = applegfx_create_task;
    desc.shell.destroy_task = applegfx_destroy_task;
    desc.shell.map_memory = applegfx_map_memory;
    desc.shell.unmap_memory = applegfx_unmap_memory;
    desc.shell.read_memory = applegfx_read_memory;
    desc.shell.write_memory = applegfx_write_memory;
    desc.shell.raise_interrupt = applegfx_raise_interrupt;

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&desc, &err);
    if (!dev) {
        fprintf(stderr, "hengevm: applegfx: device init failed: %s; using the built-in GPU instead\n",
                err ? err : "(no detail)");
        free(err);
        return 0;
    }
    ctx.dev = dev;

    lagfx_display_mode_t mode = {.width_px = 1920, .height_px = 1080, .refresh_rate_hz = 60};
    lagfx_display_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    dd.name = "Henge display";
    dd.size_mm_width = 400, dd.size_mm_height = 250;
    dd.modes = &mode, dd.mode_count = 1;
    dd.callbacks.opaque = &ctx;
    dd.callbacks.frame_ready = applegfx_frame_ready;
    dd.callbacks.mode_changed = applegfx_mode_changed;
    lagfx_display_t *disp = lagfx_display_new(dev, &dd, 0, 1, &err);
    if (!disp) {
        fprintf(stderr, "hengevm: applegfx: display init failed: %s; using the built-in GPU instead\n",
                err ? err : "(no detail)");
        free(err);
        lagfx_device_free(dev);
        return 0;
    }
    ctx.disp = disp;
    free(err);

    /* Probe-only escape hatch: prove the library loads/initialises on the tablet, then fall back to pvg_lite
       without claiming the MMIO windows (the old, safe behaviour). */
    if (getenv("HV_GPU_APPLEGFX_PROBE_ONLY")) {
        fprintf(stderr, "hengevm: applegfx: PROBE_ONLY set — device and display created, not bridging the MMIO "
                        "windows. Falling back to the built-in GPU.\n");
        return 0;
    }

    /* Frame buffers for the advertised mode (BGRA, tightly packed). Mode changes beyond this are a TODO:
       the single advertised mode means the guest should select it. */
    ctx.width = mode.width_px, ctx.height = mode.height_px;
    atomic_store(&ctx.cur_w, ctx.width);
    atomic_store(&ctx.cur_h, ctx.height);
    ctx.bufsize = (size_t)ctx.width * ctx.height * 4u;
    ctx.front = malloc(ctx.bufsize);
    ctx.back = malloc(ctx.bufsize);
    if (!ctx.front || !ctx.back) {
        fprintf(stderr, "hengevm: applegfx: out of memory for the frame buffers; using the built-in GPU instead\n");
        free(ctx.front), free(ctx.back);
        lagfx_display_free(disp), lagfx_device_free(dev);
        return 0;
    }
    memset(ctx.front, 0, ctx.bufsize);
    memset(ctx.back, 0, ctx.bufsize);
    pthread_mutex_init(&ctx.flock, NULL);

    /* Claim both MMIO windows. The GFX window forwards to the library; the IOSFC window is the stubbed
       PGIOSurfaceHostDevice. Self-locked like reims: each access may block in the library, which must not stall
       the engine's device lock for the other vCPU. */
    hv_machine_add_mmio(m, gfx_pa, 0x10000, &ctx, applegfx_gfx_read, applegfx_gfx_write, "gfx-applegfx");
    m->mmio[m->nmmio - 1].self_locked = 1;
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, &ctx, applegfx_iosfc_read, applegfx_iosfc_write, "iosurface-applegfx");
    m->mmio[m->nmmio - 1].self_locked = 1;

    g_ctx = &ctx;
    if (pthread_create(&ctx.pump, NULL, applegfx_pump, &ctx) == 0) {
        pthread_detach(ctx.pump);
        ctx.pump_running = 1;
    } else {
        fprintf(stderr, "hengevm: applegfx: could not start the vblank/frame pump\n");
    }

    fprintf(stderr, "hengevm: applegfx: EXPERIMENTAL MMIO<->PCI bridge engaged on this tablet. The GFX window and "
                    "interrupts are wired to libapplegfx-vulkan; the IOSFC (IOSurface host device) window is a stub, "
                    "so frames are unlikely until it is implemented (see android/applegfx/BRIDGE.md). Pick Graphics: "
                    "reims for the known-working GPU, or set HV_GPU_APPLEGFX_PROBE_ONLY=1 for the safe fallback.\n");
    return 1; /* the bridge owns the GPU windows now */
#else
    (void)m, (void)gic, (void)gfx_pa, (void)iosfc_pa, (void)gfx_irq, (void)iosfc_irq;
    fprintf(stderr, "hengevm: applegfx: this GPU host (libapplegfx-vulkan) is not built into this engine; "
                    "using the built-in GPU instead. Pick Graphics: reims for the app's reims-vgpu host.\n");
    return 0;
#endif
}
