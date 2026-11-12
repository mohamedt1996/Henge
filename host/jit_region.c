#include "jit_region.h"

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <mach/mach.h>
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The split shape on a host that can make it itself: plain RW memory, an
   alias, and the original turned R-X. Works for macOS binaries without the
   hardened runtime; on iOS the executable half has to come from a debugger. */
#ifdef __linux__
/* Android/Linux: one memfd mapped twice, RW for the translator and R-X to run.
   Android forbids anonymous W+X pages for apps (execmem), but this shape is allowed. */
static int open_split(HvJitRegion *region, size_t bytes) {
    size_t want = bytes, pg = (size_t)sysconf(_SC_PAGESIZE);
    bytes = (bytes + pg - 1) & ~(pg - 1);
    if (bytes < 4096) bytes = 4096;
    int fd = (int)syscall(SYS_memfd_create, "henge-jit", 0u);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return -1; }
    void *rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    if (rw == MAP_FAILED || rx == MAP_FAILED) {
        if (rw != MAP_FAILED) munmap(rw, bytes);
        if (rx != MAP_FAILED) munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, rw, bytes) != 0) return -1;
    region->capacity = want / sizeof(uint32_t); /* tests ask for regions smaller than a page */
    region->owned = 1;
    return 0;
}
#else
static int open_split(HvJitRegion *region, size_t bytes) {
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur, &max,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, (void *)rw, bytes) != 0) return -1;
    region->owned = 1;
    return 0;
}

#endif

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (getenv("HV_JIT_SPLIT")) return open_split(region, bytes);
#if TARGET_OS_OSX
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
#else
    return open_split(region, bytes); /* works only where nothing enforces code signing (tests on a jailbroken device) */
#endif
}

int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (!rx || !rw || bytes < 4096) return -1;
    region->base = rx;
    region->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    region->capacity = bytes / sizeof(uint32_t);
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->owned && region->base) {
        size_t bytes = region->capacity * sizeof(uint32_t);
        if (region->rw_diff) munmap((uint8_t *)region->base + region->rw_diff, bytes);
        munmap(region->base, bytes);
    }
    memset(region, 0, sizeof *region);
}

void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes) {
    /* One instruction may be rewritten while other CPUs run it (chaining a branch).
       memcpy may copy a word in pieces, and a CPU fetching in between takes half of
       the old branch and half of the new one: a jump to nowhere. */
    if (bytes == 4) {
        uint32_t word;
        memcpy(&word, src, 4);
#if TARGET_OS_OSX
        if (!region->rw_diff) pthread_jit_write_protect_np(0);
#endif
        __atomic_store_n((uint32_t *)((uint8_t *)start + region->rw_diff), word, __ATOMIC_RELEASE);
        return;
    }
    if (region->rw_diff) {
        memcpy((uint8_t *)start + region->rw_diff, src, bytes);
        return;
    }
#if TARGET_OS_OSX
    pthread_jit_write_protect_np(0); /* MAP_JIT: writable on this thread only, until hv_jit_finish */
#endif
    memcpy(start, src, bytes);
}

void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    if (start && words) sys_icache_invalidate(start, words * sizeof(uint32_t));
}

/* Any thread: an even number of words (blocks stay 8-byte aligned), taken atomically. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words) {
    words = (words + 1) & ~(size_t)1;
    size_t at = __atomic_fetch_add(&region->used, words, __ATOMIC_RELAXED);
    size_t cap = region->limit && region->limit < region->capacity ? region->limit : region->capacity;
    if (at + words > cap) return NULL; /* full: the flush resets used */
    return region->base + at;
}

void hv_jit_reset(HvJitRegion *region) {
#ifdef __linux__
    /* Android/Linux: give the code just dropped back to the kernel. The region is a memfd mapped twice
       (open_split), so without this every page ever written stays resident for the rest of the run, the whole
       cache after its first fill, though nothing can run it again: under memory pressure Android compresses that
       dead code into swap, and the next fill has to bring each page back just to overwrite it. Every other CPU
       is parked at the dispatcher when this runs (stop_the_world), so nothing holds an address in it; the pages
       come back as zeros, filled as blocks are written. HV_JIT_KEEP=1 keeps the pages, as before. */
    static int keep = -1;
    if (keep < 0) keep = getenv("HV_JIT_KEEP") != NULL;
    if (!keep && region->owned && region->rw_diff && region->base) {
        size_t used = region->used < region->capacity ? region->used : region->capacity; /* reserve counts past the end when full */
        size_t pg = (size_t)sysconf(_SC_PAGESIZE), bytes = (used * sizeof(uint32_t) + pg - 1) & ~(pg - 1);
        size_t whole = region->capacity * sizeof(uint32_t) & ~(pg - 1);
        if (bytes > whole) bytes = whole;
        /* Through the writable alias: MADV_REMOVE punches the file, which unmaps the pages from both. */
        if (bytes && madvise((uint8_t *)region->base + region->rw_diff, bytes, MADV_REMOVE) == 0) region->released += bytes;
    }
#endif
    region->used = 0;
}
