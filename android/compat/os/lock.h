#pragma once
#include <pthread.h>
typedef pthread_mutex_t os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define os_unfair_lock_lock pthread_mutex_lock
#define os_unfair_lock_unlock pthread_mutex_unlock
