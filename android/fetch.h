#ifndef HENGE_ANDROID_FETCH_H
#define HENGE_ANDROID_FETCH_H
#include <stdint.h>
#include <stdio.h>

typedef struct { const char *id, *name, *url; } HvRelease;
const HvRelease *hv_release_find(const char *id);
void hv_release_list(FILE *f);
/* AVPBooter from Apple's macOS 14.6.1 image, checked by hash; 0 or -1. Reuses a good file at path. */
int hv_fetch_boot_rom(const char *path);
/* Resumable download to path (via path.part); 0 or -1. */
int hv_download(const char *url, const char *path);
/* disk.img (sparse), auxiliary.bin, machine.json with a new ECID; 0 or -1. */
int hv_new_mac(const char *dir, const char *name, uint64_t ram_mb, uint64_t disk_gb, uint64_t *ecid);
#endif
