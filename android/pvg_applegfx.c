/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: applegfx — a second, independent paravirtual-GPU host, as an alternative to reims so the GPU is
 * not bet on one implementation. It is meant to drive libapplegfx-vulkan (MattJackson/libapplegfx-vulkan, MIT):
 * a separate Vulkan-backed implementation of Apple's ParavirtualizedGraphics. vmapple.c tries this before
 * reims; both gate on their own HV_GPU_* variable, so selecting one never starts the other.
 *
 * Built with HAVE_APPLEGFX once the library is linked into the engine (its Android build is a CI step like
 * reims/build.sh). Until then this is the honest fallback: when the person picks Graphics: applegfx the engine
 * says the backend is not in this build and returns 0, so vmapple.c falls through to reims (off here) and then
 * to the built-in pvg_lite, which still shows a desktop. Nothing here changes how reims or the other modes run.
 */
#include <stdio.h>
#include <stdlib.h>

#include "vm.h"

/* The library's own entry points, provided when the engine is built with HAVE_APPLEGFX. Kept to the same
 * shape as pvg_reims.c's so the engine's status line and vmapple.c dispatch treat both the same way. */
#if defined(HAVE_APPLEGFX)
int hengevm_applegfx_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq);
#endif

int hv_pvg_applegfx_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    const char *path = getenv("HV_GPU_APPLEGFX");
    if (!path || !*path)
        return 0; /* a different backend was chosen; leave the device to it */
#if defined(HAVE_APPLEGFX)
    return hengevm_applegfx_create(m, gic, gfx_pa, iosfc_pa, gfx_irq, iosfc_irq);
#else
    (void)m, (void)gic, (void)gfx_pa, (void)iosfc_pa, (void)gfx_irq, (void)iosfc_irq;
    fprintf(stderr, "hengevm: applegfx: this GPU host (libapplegfx-vulkan) is not built into this engine yet; "
                    "using the built-in GPU instead. Pick Graphics: reims for the app's reims-vgpu host.\n");
    return 0;
#endif
}
