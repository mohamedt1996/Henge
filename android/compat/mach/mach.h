#pragma once
/* Linux/Android stand-ins for the few Mach calls the engine makes. */
#include <mach/mach_time.h>
#include <stdint.h>
#include <sys/resource.h>
typedef int kern_return_t;
typedef unsigned int mach_port_t;
typedef unsigned int mach_msg_type_number_t;
typedef int *thread_info_t;
#define KERN_SUCCESS 0
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct {
    time_value_t user_time, system_time;
    int cpu_usage, policy, run_state, flags, suspend_count, sleep_time;
} thread_basic_info_data_t;
#define TH_STATE_RUNNING 1
static inline mach_port_t mach_thread_self(void) { return 0; }
static inline mach_port_t mach_task_self(void) { return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return 0; }
static inline kern_return_t thread_info(mach_port_t t, int f, thread_info_t out, mach_msg_type_number_t *n) {
    (void)t; (void)f; (void)n;
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru)) return -1;
    thread_basic_info_data_t *ti = (thread_basic_info_data_t *)out;
    ti->user_time.seconds = ru.ru_utime.tv_sec; ti->user_time.microseconds = ru.ru_utime.tv_usec;
    ti->system_time.seconds = ru.ru_stime.tv_sec; ti->system_time.microseconds = ru.ru_stime.tv_usec;
    ti->run_state = TH_STATE_RUNNING; /* not known here (the engine's thread ports are 0 on Android) */
    return 0;
}
