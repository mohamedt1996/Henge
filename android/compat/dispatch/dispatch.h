#pragma once
/* Linux/Android stand-in for the parts of libdispatch the engine uses, in their function (_f) forms (no blocks:
   the NDK has no blocks runtime). host/vm.c: a serial queue for the pointer, dispatch_after_f for delayed work,
   dispatch_once_f. host/snapshot.c: a group and dispatch_apply_f for packing RAM on every core.

   Semantics kept: a serial queue runs its work in order on one thread; dispatch_sync_f waits for its function;
   dispatch_after_f runs no sooner than asked. Simplified: the global queues are one serial queue (the engine only
   puts short, order-insensitive work there), and group_async_f runs each function on a thread of its own. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../plat_linux.h"

typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ull
#endif
#ifndef NSEC_PER_MSEC
#define NSEC_PER_MSEC 1000000ull
#endif
#ifndef NSEC_PER_USEC
#define NSEC_PER_USEC 1000ull
#endif
#define DISPATCH_QUEUE_SERIAL NULL
#define DISPATCH_QUEUE_CONCURRENT ((void *)1)
#define DISPATCH_APPLY_AUTO NULL

typedef void (*dispatch_function_t)(void *);

typedef struct hv_dq_job {
    struct hv_dq_job *next;
    dispatch_function_t fn;
    void *ctx;
    int *done; /* dispatch_sync_f: set when fn has run */
} hv_dq_job;

typedef struct hv_dq {
    pthread_mutex_t mu;
    pthread_cond_t cv, done_cv;
    hv_dq_job *head, *tail;
    int started;
} *dispatch_queue_t;

static inline uint64_t hv_dq_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * NSEC_PER_SEC + (uint64_t)t.tv_nsec;
}

static inline dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when == DISPATCH_TIME_NOW ? hv_dq_now() : when;
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}

static void *hv_dq_worker(void *arg) {
    dispatch_queue_t q = (dispatch_queue_t)arg;
    pthread_mutex_lock(&q->mu);
    for (;;) {
        while (!q->head) pthread_cond_wait(&q->cv, &q->mu);
        hv_dq_job *j = q->head;
        q->head = j->next;
        if (!q->head) q->tail = NULL;
        pthread_mutex_unlock(&q->mu);
        j->fn(j->ctx);
        pthread_mutex_lock(&q->mu);
        if (j->done) {
            *j->done = 1;
            pthread_cond_broadcast(&q->done_cv);
        } else {
            free(j);
        }
    }
    return NULL;
}

static inline dispatch_queue_t dispatch_queue_create(const char *label, void *attr) {
    (void)label, (void)attr;
    dispatch_queue_t q = (dispatch_queue_t)calloc(1, sizeof *q);
    if (!q) abort();
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    pthread_cond_init(&q->done_cv, NULL);
    return q;
}

static inline void hv_dq_push(dispatch_queue_t q, hv_dq_job *j) {
    pthread_mutex_lock(&q->mu);
    if (!q->started) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, hv_dq_worker, q) != 0) abort();
        pthread_attr_destroy(&a);
        q->started = 1;
    }
    j->next = NULL;
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static inline void dispatch_async_f(dispatch_queue_t q, void *ctx, dispatch_function_t fn) {
    hv_dq_job *j = (hv_dq_job *)calloc(1, sizeof *j);
    if (!j) abort();
    j->fn = fn, j->ctx = ctx;
    hv_dq_push(q, j);
}

static inline void dispatch_sync_f(dispatch_queue_t q, void *ctx, dispatch_function_t fn) {
    int done = 0;
    hv_dq_job j = {NULL, fn, ctx, &done};
    hv_dq_push(q, &j);
    pthread_mutex_lock(&q->mu);
    while (!done) pthread_cond_wait(&q->done_cv, &q->mu);
    pthread_mutex_unlock(&q->mu);
}

static inline dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags) {
    (void)qos, (void)flags;
    static dispatch_queue_t g;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!g) g = dispatch_queue_create("global", NULL);
    pthread_mutex_unlock(&mu);
    return g;
}

typedef struct {
    dispatch_time_t when;
    dispatch_queue_t q;
    void *ctx;
    dispatch_function_t fn;
} hv_dq_after;

static void *hv_dq_after_thread(void *arg) {
    hv_dq_after a = *(hv_dq_after *)arg;
    free(arg);
    for (;;) {
        uint64_t now = hv_dq_now();
        if (now >= a.when) break;
        uint64_t left = a.when - now;
        struct timespec ts = {(time_t)(left / NSEC_PER_SEC), (long)(left % NSEC_PER_SEC)};
        nanosleep(&ts, NULL);
    }
    dispatch_async_f(a.q, a.ctx, a.fn);
    return NULL;
}

static inline void dispatch_after_f(dispatch_time_t when, dispatch_queue_t q, void *ctx, dispatch_function_t fn) {
    if (when == DISPATCH_TIME_FOREVER) return;
    hv_dq_after *a = (hv_dq_after *)malloc(sizeof *a);
    if (!a) abort();
    *a = (hv_dq_after){when, q, ctx, fn};
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, hv_dq_after_thread, a) != 0) {
        free(a);
        dispatch_async_f(q, ctx, fn); /* no thread to wait on: at once rather than never */
    }
    pthread_attr_destroy(&at);
}

typedef long dispatch_once_t; /* zero-initialised, as GCD's */

static inline void dispatch_once_f(dispatch_once_t *pred, void *ctx, dispatch_function_t fn) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == 2) return;
    pthread_mutex_lock(&mu);
    if (__atomic_load_n(pred, __ATOMIC_RELAXED) != 2) {
        fn(ctx);
        __atomic_store_n(pred, 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
}

/* Groups: each function on a thread of its own; dispatch_group_wait joins them. */
typedef struct hv_dq_group {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int pending;
} *dispatch_group_t;

static inline dispatch_group_t dispatch_group_create(void) {
    dispatch_group_t g = (dispatch_group_t)calloc(1, sizeof *g);
    if (!g) abort();
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    return g;
}

typedef struct { dispatch_group_t g; void *ctx; dispatch_function_t fn; } hv_dq_gjob;

static void *hv_dq_group_thread(void *arg) {
    hv_dq_gjob j = *(hv_dq_gjob *)arg;
    free(arg);
    j.fn(j.ctx);
    pthread_mutex_lock(&j.g->mu);
    if (--j.g->pending == 0) pthread_cond_broadcast(&j.g->cv);
    pthread_mutex_unlock(&j.g->mu);
    return NULL;
}

static inline void dispatch_group_async_f(dispatch_group_t g, dispatch_queue_t q, void *ctx, dispatch_function_t fn) {
    (void)q;
    hv_dq_gjob *j = (hv_dq_gjob *)malloc(sizeof *j);
    if (!j) abort();
    *j = (hv_dq_gjob){g, ctx, fn};
    pthread_mutex_lock(&g->mu);
    g->pending++;
    pthread_mutex_unlock(&g->mu);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&t, &at, hv_dq_group_thread, j) != 0) hv_dq_group_thread(j); /* no thread: here */
    pthread_attr_destroy(&at);
}

static inline long dispatch_group_wait(dispatch_group_t g, dispatch_time_t timeout) {
    (void)timeout; /* only DISPATCH_TIME_FOREVER is used */
    pthread_mutex_lock(&g->mu);
    while (g->pending) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
    return 0;
}

static inline void dispatch_release(void *o) { (void)o; }

/* dispatch_apply_f: n iterations over the online cores, each taking the next index. */
typedef struct {
    size_t n;
    _Atomic size_t next;
    void *ctx;
    void (*fn)(void *, size_t);
} hv_dq_apply;

static void *hv_dq_apply_thread(void *arg) {
    hv_dq_apply *a = (hv_dq_apply *)arg;
    for (size_t i; (i = atomic_fetch_add(&a->next, 1)) < a->n;) a->fn(a->ctx, i);
    return NULL;
}

static inline void dispatch_apply_f(size_t n, dispatch_queue_t q, void *ctx, void (*fn)(void *, size_t)) {
    (void)q;
    hv_dq_apply a = {n, 0, ctx, fn};
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int nt = cores < 1 ? 1 : cores > 16 ? 16 : (int)cores;
    if ((size_t)nt > n) nt = (int)n;
    pthread_t t[16];
    int made = 0;
    for (int i = 1; i < nt; i++)
        if (pthread_create(&t[made], NULL, hv_dq_apply_thread, &a) == 0) made++;
    hv_dq_apply_thread(&a);
    for (int i = 0; i < made; i++) pthread_join(t[i], NULL);
}
