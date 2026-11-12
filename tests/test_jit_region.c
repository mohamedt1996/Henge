#include "jit_region.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>

/* Both shapes: macOS MAP_JIT, and the iOS split mapping (RX plus an RW alias). */
static int one(const char *what) {
    HvJitRegion region;
    if (hv_jit_region_open(&region, 1 << 20) != 0) {
        printf("%s: could not map the JIT region\n", what);
        return 1;
    }
    uint32_t *code = hv_jit_reserve(&region, 2);
    static const uint32_t body[2] = {0xd2800540, 0xd65f03c0}; /* mov x0, #42; ret */
    hv_jit_write(&region, code, body, sizeof body);
    hv_jit_finish(&region, code, 2);
    long (*fn)(void) = (long (*)(void))code;
    long got = fn();
    printf("%s: jit region returned %ld\n", what, got);
    hv_jit_region_close(&region);
    return got != 42;
}

#ifdef __linux__
/* A reset gives the pages written back to the kernel (hv_jit_reset): the code is gone (zeros), the region
   takes new code at the same addresses and runs it, and a region filled past its end releases no more than
   it has. */
/* Whether MADV_REMOVE takes pages out of a shared mapping here: qemu-user answers success and does nothing,
   so under it only the reuse is checked. */
static int removes(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    int fd = (int)syscall(SYS_memfd_create, "probe", 0u);
    if (fd < 0 || ftruncate(fd, (off_t)pg) != 0) return 0;
    volatile char *p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 0;
    p[0] = 1;
    int ok = madvise((void *)p, pg, MADV_REMOVE) == 0 && p[0] == 0;
    munmap((void *)p, pg);
    return ok;
}

static int reset_releases(void) {
    HvJitRegion region;
    enum { BYTES = 4 << 20, WORDS = BYTES / 4 };
    int strict = removes();
    if (hv_jit_region_open(&region, BYTES) != 0) return printf("reset: could not map the JIT region\n"), 1;
    static const uint32_t ret42[2] = {0xd2800540, 0xd65f03c0}, ret7[2] = {0xd28000e0, 0xd65f03c0}; /* mov x0, #42 / #7; ret */
    int bad = 0;
    for (int round = 0; round < 3; round++) {
        /* Fill it: the same two instructions over the whole region, one block reserved past the end. */
        uint32_t *first = NULL, *last = NULL;
        for (uint32_t *code; (code = hv_jit_reserve(&region, 2)) != NULL; last = code) {
            if (!first) first = code;
            hv_jit_write(&region, code, round & 1 ? ret7 : ret42, sizeof ret42);
        }
        hv_jit_finish(&region, first, WORDS);
        long want = round & 1 ? 7 : 42;
        if (first != region.base || ((long (*)(void))first)() != want || ((long (*)(void))last)() != want) bad = 1;
        uint64_t before = region.released;
        hv_jit_reset(&region);
        if (region.used != 0 || region.released - before != BYTES) bad = 1;
        /* Both mappings read zeros now: the pages were dropped, not kept. */
        const uint32_t *rw = (const uint32_t *)((const uint8_t *)region.base + region.rw_diff);
        for (size_t i = 0; strict && i < WORDS; i += 1021)
            if (region.base[i] || rw[i]) bad = 1;
    }
    /* A partly used region gives back the pages it used, whole pages. */
    uint32_t *code = hv_jit_reserve(&region, 2);
    hv_jit_write(&region, code, ret42, sizeof ret42);
    hv_jit_finish(&region, code, 2);
    uint64_t before = region.released;
    if (((long (*)(void))code)() != 42) bad = 1;
    hv_jit_reset(&region);
    if (region.released - before != (uint64_t)sysconf(_SC_PAGESIZE) || (strict && region.base[0])) bad = 1;
    printf("reset: %s (%llu MB given back%s)\n", bad ? "FAILED" : "pages given back and reused", (unsigned long long)(region.released >> 20),
           strict ? "" : "; this host's MADV_REMOVE does nothing, so only the reuse was checked");
    hv_jit_region_close(&region);
    return bad;
}
#endif

int main(void) {
    int bad = one("MAP_JIT");
    setenv("HV_JIT_SPLIT", "1", 1);
    bad |= one("split");
#ifdef __linux__
    bad |= reset_releases();
#endif
    return bad;
}
