/* Android: the paravirtual GPU's protocol side (pvg_lite.c) runs as on iOS, so the
   guest's GPU driver attaches and its command channels are serviced, but there is no
   renderer behind it yet: these are the Metal replay's entry points with nothing to do. */
#include "pvg_lite.h"

#include <stddef.h>

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) { (void)lite; return NULL; }
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *l, uint32_t ch, uint32_t *cmd, uint32_t len) { (void)m; (void)l; (void)ch; (void)cmd; (void)len; }
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t w, uint32_t h) { (void)m; (void)bgra; (void)w; (void)h; return -1; }
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **px, uint32_t *w, uint32_t *h) { (void)m; (void)px; (void)w; (void)h; return -1; }
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) { (void)m; (void)mapping; }
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) { (void)m; (void)layer; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { (void)m; return 0; }
