#pragma once
/* Linux/Android: the engine counts in 24 MHz ticks (Apple's CNTVCT rate). */
#include <stdint.h>
#include <time.h>
static inline uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 24000000ull + (uint64_t)ts.tv_nsec * 3 / 125;
}
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = 125; tb->denom = 3; return 0; } /* ticks -> ns */
