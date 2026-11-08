/* Android bring-up: no paravirtual GPU yet. The Metal replay (pvg_lite.c +
   pvg_metal.m) is replaced by these no-ops until a Vulkan backend exists, so
   the machine boots headless with the serial console only. */
#include "pvg_lite.h"

#include <stddef.h>

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    (void)m; (void)gic; (void)gfx_pa; (void)iosfc_pa; (void)gfx_irq; (void)iosfc_irq;
    return NULL;
}
void hv_pvg_lite_poll(HvPvgLite *p) { (void)p; }
int hv_pvg_lite_frame(HvPvgLite *p, const uint8_t **px, uint32_t *w, uint32_t *h) { (void)p; (void)px; (void)w; (void)h; return 0; }
int hv_pvg_lite_snapshot(HvPvgLite *p, uint8_t *bgra, uint32_t w, uint32_t h) { (void)p; (void)bgra; (void)w; (void)h; return 0; }
uint8_t *hv_pvg_lite_display_page(HvPvgLite *p) { (void)p; return NULL; }
void hv_pvg_lite_set_layer(HvPvgLite *p, void *layer) { (void)p; (void)layer; }
uint64_t hv_pvg_lite_gpu_frames(HvPvgLite *p) { (void)p; return 0; }
void hv_pvg_lite_barrier_stats(HvPvgLite *p, uint64_t out[4]) { (void)p; out[0] = out[1] = out[2] = out[3] = 0; }
void hv_pvg_lite_set_mode(uint32_t w, uint32_t h) { (void)w; (void)h; }
void hv_pvg_lite_set_size_mm(uint32_t w, uint32_t h) { (void)w; (void)h; }
