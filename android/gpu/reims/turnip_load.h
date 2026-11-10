/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims, Vulkan driver: Turnip (turnip_load.cpp). */
#ifndef HV_TURNIP_LOAD_H
#define HV_TURNIP_LOAD_H
#ifdef __cplusplus
extern "C" {
#endif
/* Opens dir/name (the app's own Vulkan driver) in a child of Android's vendor-driver linker namespace and
   answers its vk_icdGetInstanceProcAddr, or NULL; reims_turnip_error says what happened either way. */
void *reims_turnip_open(const char *dir, const char *name);
const char *reims_turnip_error(void);
#ifdef __cplusplus
}
#endif
#endif
