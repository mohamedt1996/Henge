#pragma once
/* Linux/Android: the engine counts in 24 MHz ticks (Apple's CNTVCT rate).
   Since 0.3.59 that is the CPU's own counter where its rate converts exactly: 24 MHz as it is, 19.2 MHz
   (Qualcomm's, the Mi Pad 7's) times 5/4, which is c + c/4 exactly. Translated code then reads the Mac's
   clock with two instructions (translate_counter) where every read went through the engine's helper
   (150 to 180 thousand a second on the tablet). Any other rate, or HV_CLOCK_COUNTER=1: the system clock,
   as before. One time base for the whole engine either way: the timers' deadlines compare against it. */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
static inline int hv_cnt_mode(void) {
#if defined(__aarch64__)
    static int mode = -1;
    if (mode < 0) {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        int m = f == 24000000 ? 1 : f == 19200000 ? 2 : 0;
        const char *off = getenv("HV_CLOCK_COUNTER");
        if (off && *off == '1') m = 0;
        if (off && *off == '2') m = 2; /* tests: the 19.2 MHz conversion on whatever rate the CPU (or qemu) has */
        __atomic_store_n(&mode, m, __ATOMIC_RELAXED);
    }
    return mode;
#else
    return 0;
#endif
}
static inline uint64_t mach_absolute_time(void) {
#if defined(__aarch64__)
    int mode = hv_cnt_mode();
    if (mode) {
        uint64_t c;
        __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(c)::"memory");
        return mode == 2 ? c + (c >> 2) : c;
    }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 24000000ull + (uint64_t)ts.tv_nsec * 3 / 125;
}
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = 125; tb->denom = 3; return 0; } /* ticks -> ns */
