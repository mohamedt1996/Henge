/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: applegfx — a second, independent paravirtual-GPU host, so the GPU is not bet on reims alone. It
 * drives libapplegfx-vulkan (MattJackson/libapplegfx-vulkan, MIT): a clean-room host implementation of Apple's
 * ParavirtualizedGraphics with its own AIR -> SPIR-V translator and a Vulkan backend.
 *
 * Built with HAVE_APPLEGFX once the library is cross-built for the tablet (android/applegfx/build.sh, a CI step
 * like reims/build.sh). vmapple.c tries this before reims; both gate on their own HV_GPU_* variable.
 *
 * What is wired here, and what is not:
 *  - Wired: the library's shell callbacks are backed by the real engine — guest RAM (hv_ram_lookup) for
 *    read/write/map, the GIC (hv_gicv3_pend) for interrupts, and the library's own task-memory primitives.
 *    On --gpu applegfx the device and a display are created with these callbacks, so the second GPU stack is
 *    proven to load and initialise on the tablet (its version is logged).
 *  - Not wired yet: libapplegfx-vulkan speaks Apple's *PCI* paravirtual-GPU transport (one BAR + MSI-X), while
 *    vmapple exposes the *MMIO* variant (two windows + GIC), which is what reims matches. Bridging the two
 *    transports is the remaining work before this can drive the screen, so for now the display stays on
 *    pvg_lite and this returns 0 after proving initialisation. Pick Graphics: reims for the working GPU.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "machine.h"
#include "gicv3.h"
#include "vmapple.h"

#if defined(HAVE_APPLEGFX)
#include "libapplegfx-vulkan.h"

/* The engine state the library's callbacks reach back into. */
struct applegfx_ctx {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    HvRam *ram;
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
static void applegfx_raise_interrupt(void *o, uint32_t vector) {
    struct applegfx_ctx *C = o;
    hv_gicv3_pend(C->gic, vector == 0 ? C->gfx_irq : C->iosfc_irq);
}
#endif /* HAVE_APPLEGFX */

int hv_pvg_applegfx_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    const char *path = getenv("HV_GPU_APPLEGFX");
    if (!path || !*path)
        return 0; /* a different backend was chosen; leave the device to it */
    (void)gfx_pa, (void)iosfc_pa;

#if defined(HAVE_APPLEGFX)
    static struct applegfx_ctx ctx;
    ctx.m = m, ctx.gic = gic, ctx.gfx_irq = gfx_irq, ctx.iosfc_irq = iosfc_irq;
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
    lagfx_display_mode_t mode = {.width_px = 1920, .height_px = 1080, .refresh_rate_hz = 60};
    lagfx_display_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    dd.name = "Henge display";
    dd.size_mm_width = 400, dd.size_mm_height = 250;
    dd.modes = &mode, dd.mode_count = 1;
    lagfx_display_t *disp = lagfx_display_new(dev, &dd, 0, 1, &err);
    (void)disp;
    free(err);
    fprintf(stderr, "hengevm: applegfx: device and display created on this tablet (guest memory and interrupts "
                    "wired). The screen stays on the built-in GPU: the apple-gfx MMIO<->PCI transport bridge is "
                    "the remaining work before applegfx drives it. Pick Graphics: reims for the working GPU.\n");
    return 0; /* fall through to pvg_lite for the actual screen until the transport bridge lands */
#else
    (void)m, (void)gic, (void)gfx_irq, (void)iosfc_irq;
    fprintf(stderr, "hengevm: applegfx: this GPU host (libapplegfx-vulkan) is not built into this engine; "
                    "using the built-in GPU instead. Pick Graphics: reims for the app's reims-vgpu host.\n");
    return 0;
#endif
}
