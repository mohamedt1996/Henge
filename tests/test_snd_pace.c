/* SPDX-License-Identifier: GPL-2.0-or-later */
/* devices/snd_pace.h against two guests, in simulated time (1 ms steps, the device thread's; the player pulls 10 ms
   every 10 ms): one that sends a buffer every period by its own timer, late by a random jitter, and one that sends
   the next buffer some turnaround after the last came back. Each must play without silence once started, get its
   buffers back one period apart, and start again with the whole lead after a pause.

     cc -O2 -Idevices -o test_snd_pace tests/test_snd_pace.c && ./test_snd_pace */
#include <stdio.h>
#include <stdlib.h>

#include "snd_pace.h"

#define RATE (48000u * 8u)           /* bytes a second: 48 kHz, 2 channels, float */
#define PERIOD (4096u * 8u)          /* a transmit buffer */
#define MS 1000000ull

static unsigned rnd_state = 12345;
static unsigned rnd(unsigned n) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 16) % n; }

typedef struct { int fails; } T;

/* timer: 1 = sends by its own timer, 0 = sends `turnaround` ms after a buffer comes back. */
static void run(const char *name, int timer, unsigned lead_ms, unsigned jitter_ms, T *t) {
    HvSndPace p = {0};
    uint32_t lead = RATE / 1000 * lead_ms;
    uint64_t written = 0, consumed = 0, done = 0; /* done: buffers returned */
    uint64_t now = 1000 * MS, sent = 0;
    uint64_t next_send = now + 3 * MS, nominal = next_send, last_done_ns = 0;
    uint64_t silence = 0, played = 0, worst_gap = 0, least_gap = ~0ull, returns = 0;
    int pause_done = 0, started = 0;
    uint64_t silence_after_start = 0, first_play_ns = 0, resume_first_data = 0, resume_first_play = 0;
    hv_snd_pace_start(&p, written, now, RATE, lead);
    for (int ms = 0; ms < 60000; ms++, now += MS) {
        int paused = ms >= 30000 && ms < 32000; /* two seconds without sound */
        if (ms == 32000) pause_done = 1, next_send = now + 3 * MS, nominal = next_send;
        /* the guest */
        if (!paused && now >= next_send && (timer || sent == done)) {
            if (pause_done == 1) resume_first_data = now, pause_done = 2;
            written += PERIOD, sent++;
            if (timer) { /* the next one period on by the guest's timer, sent up to `jitter_ms` late */
                nominal += (uint64_t)PERIOD * 1000000000ull / RATE;
                next_send = nominal + (jitter_ms ? rnd(jitter_ms) * MS : 0);
            } else {
                next_send = ~0ull;
            }
        }
        /* the device thread */
        uint64_t pos = hv_snd_pace_advance(&p, written, now);
        while ((done + 1) * (uint64_t)PERIOD <= pos && done < sent) {
            done++;
            if (last_done_ns && !paused && !(ms >= 32000 && ms < 32200)) {
                uint64_t gap = now - last_done_ns;
                if (ms > 500) { if (gap > worst_gap) worst_gap = gap; if (gap < least_gap) least_gap = gap; returns++; }
            }
            last_done_ns = now;
            if (!timer) next_send = now + (1 + rnd(jitter_ms ? jitter_ms : 1)) * MS; /* the guest's turnaround */
        }
        /* the player, every 10 ms */
        if (ms % 10 == 0) {
            uint32_t len = RATE / 100;
            hv_snd_pace_advance(&p, written, now);
            uint64_t to = hv_snd_pace_play_to(&p, written);
            uint64_t n = to > consumed ? to - consumed : 0;
            if (n > len) n = len;
            consumed += n, played += n;
            if (n && !first_play_ns) first_play_ns = now, started = 1;
            if (n && pause_done == 2 && !resume_first_play) resume_first_play = now;
            if (n < len && !paused && started && ms > 200 && !(ms >= 30000 && ms < 32200)) silence_after_start += len - n;
            silence += len - n;
        }
    }
    double period_ms = (double)PERIOD * 1000 / RATE;
    printf("%-34s lead %2u ms jitter %2u ms: silence while playing %5.1f ms, buffers back every %.1f to %.1f ms (period %.1f), "
           "first sound %llu ms after the first buffer, after the pause %llu ms; late %llu times\n",
           name, lead_ms, jitter_ms, (double)silence_after_start * 1000 / RATE, (double)least_gap / 1e6, (double)worst_gap / 1e6,
           period_ms, (unsigned long long)((first_play_ns - 1003 * MS) / MS),
           (unsigned long long)((resume_first_play - resume_first_data) / MS), (unsigned long long)p.holds);
    (void)silence, (void)played, (void)returns;
    if (lead_ms && jitter_ms < lead_ms) {
        if (silence_after_start) t->fails++, printf("  FAIL: silence with a jitter under the lead\n");
        if (worst_gap > (uint64_t)((period_ms + (timer ? 2 : jitter_ms + 2)) * 1e6)) t->fails++, printf("  FAIL: a buffer came back late\n");
        if ((first_play_ns - 1003 * MS) / MS + 11 < lead_ms) t->fails++, printf("  FAIL: started without the lead\n");
        if ((resume_first_play - resume_first_data) / MS + 11 < lead_ms) t->fails++, printf("  FAIL: resumed without the lead\n");
    }
}

int main(void) {
    T t = {0};
    run("timer-driven guest", 1, 50, 0, &t);
    run("timer-driven guest", 1, 50, 30, &t);
    run("timer-driven guest (no lead)", 1, 0, 30, &t);
    run("guest that sends at each return", 0, 50, 5, &t);
    run("guest that sends at each return", 0, 50, 30, &t);
    run("guest that sends at each return (no lead)", 0, 0, 10, &t);
    printf(t.fails ? "FAILED: %d\n" : "ok\n", t.fails);
    return t.fails != 0;
}
