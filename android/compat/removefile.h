#pragma once
/* Linux/Android stand-in for removefile(REMOVEFILE_RECURSIVE), as host/share.c uses it (a WebDAV DELETE of a folder). */
#include <ftw.h>
#include <stdio.h>
#include <unistd.h>

typedef void *removefile_state_t;
#define REMOVEFILE_RECURSIVE (1 << 0)

static inline int hv_remove_one(const char *path, const struct stat *st, int type, struct FTW *f) {
    (void)st, (void)f;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static inline int removefile(const char *path, removefile_state_t state, unsigned flags) {
    (void)state;
    if (!(flags & REMOVEFILE_RECURSIVE)) return remove(path);
    return nftw(path, hv_remove_one, 32, FTW_DEPTH | FTW_PHYS);
}
