/* The engine's half of gpu.reims (android/pvg_reims.c) against a stand-in for the app's slot thread
   (android/gpu/reims/host.c's slot_loop): posted writes (0.3.59) reach the device in order, a read sees
   every write before it, and a write returns before the device has taken it. */
#include "../android/pvg_reims.c"

#include <assert.h>


static HvReimsHeader hdr;
static volatile int app_stop, app_delay_us;
static uint64_t log_off[4096], log_val[4096];
static volatile uint32_t nlog;
static uint64_t regs[64];

static void *app(void *arg) {
    (void)arg;
    HvReimsSlot *s = &hdr.slot[0];
    while (!app_stop) {
        __atomic_store_n(&hdr.app_heartbeat, now_ns(), __ATOMIC_RELEASE);
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HV_REIMS_REQUEST) { usleep(10); continue; }
        if (app_delay_us) usleep((useconds_t)app_delay_us);
        uint64_t v = 0;
        if (s->write) {
            regs[(s->offset / 4) & 63] = s->value;
            uint32_t i = nlog;
            if (i < 4096) log_off[i] = s->offset, log_val[i] = s->value, nlog = i + 1;
        } else
            v = regs[(s->offset / 4) & 63];
        s->value = v;
        __atomic_store_n(&s->state, s->write ? HV_REIMS_IDLE : HV_REIMS_REPLY, __ATOMIC_RELEASE);
        futex(&s->state, FUTEX_WAKE, 1, NULL);
    }
    return NULL;
}

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void) {
    R.h = &hdr;
    hdr.app_ready = 1;
    hdr.app_heartbeat = now_ns();
    pthread_t t;
    pthread_create(&t, NULL, app, NULL);

    /* Writes posted, then a read: the read sees the last write, and the device took all of them in order. */
    for (uint64_t i = 0; i < 1000; i++) gfx_write(NULL, 0x40, i, 4);
    uint64_t v = gfx_read(NULL, 0x40, 4);
    CHECK(v == 999, "a read after 1000 posted writes: %llu", (unsigned long long)v);
    int ordered = nlog == 1000;
    for (uint32_t i = 0; ordered && i < nlog; i++) ordered = log_val[i] == i;
    CHECK(ordered, "the device took %u writes, in order: %d", nlog, ordered);

    /* A write returns before a slow device has taken it; the next access waits for it. */
    app_delay_us = 20000;
    uint64_t t0 = now_ns();
    gfx_write(NULL, 0x44, 7, 4);
    uint64_t posted = now_ns() - t0;
    v = gfx_read(NULL, 0x44, 4);
    uint64_t both = now_ns() - t0;
    CHECK(posted < 5000000 && both >= 30000000 && v == 7, "a posted write took %.1f ms, it and a read %.1f ms, read %llu",
          posted / 1e6, both / 1e6, (unsigned long long)v);

    app_delay_us = 0;
    printf("%d checks, %d failures\n", checks, failures);
    app_stop = 1;
    pthread_join(t, NULL);
    return failures != 0;
}
