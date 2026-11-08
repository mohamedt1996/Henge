#pragma once
/* Linux/Android stand-ins for Darwin-only libc extensions used by the engine. */
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) abs.tv_sec++, abs.tv_nsec -= 1000000000L;
    return pthread_cond_timedwait(cv, mu, &abs);
}

typedef int qos_class_t;
#define QOS_CLASS_UTILITY 0x11
#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED 0x19
static inline int pthread_set_qos_class_self_np(int c, int p) { (void)c; (void)p; return 0; }
static inline const char *mach_error_string(int kr) { return strerror(kr); }

/* Darwin's compare-and-wait on a 32-bit word, as a Linux futex. */
#include <linux/futex.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#define HV_ULF_WAKE_ALL_ 0x100u
static inline int __ulock_wait(uint32_t op, void *addr, uint64_t value, uint32_t timeout_us) {
    (void)op;
    struct timespec ts = {(time_t)(timeout_us / 1000000), (long)(timeout_us % 1000000) * 1000};
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, (uint32_t)value, timeout_us ? &ts : NULL, NULL, 0);
}
static inline int __ulock_wake(uint32_t op, void *addr, uint64_t wake_value) {
    (void)wake_value;
    return (int)syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, (op & HV_ULF_WAKE_ALL_) ? 0x7fffffff : 1, NULL, NULL, 0);
}
#define QOS_CLASS_DEFAULT 0x15
