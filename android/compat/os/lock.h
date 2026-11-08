#pragma once
/* os_unfair_lock as a small futex-free spinlock that yields: assignable like Darwin's. */
#include <sched.h>
typedef struct { int v; } os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT ((os_unfair_lock){0})
static inline void os_unfair_lock_lock(os_unfair_lock *l) {
    while (__atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE)) sched_yield();
}
static inline void os_unfair_lock_unlock(os_unfair_lock *l) { __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE); }
