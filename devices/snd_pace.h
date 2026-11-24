#ifndef HENGEVM_SND_PACE_H
#define HENGEVM_SND_PACE_H

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The sound device's clock (0.3.117): when a transmit buffer goes back to the guest, and how far the player may
   have got, both from one position that runs at the stream's rate by the host's clock.

   Up to 0.3.116 a buffer went back once the player had taken its last byte, and the player took bytes as they came.
   macOS's driver sends each buffer as the device reports the one before done, so the ring was empty at every buffer's
   end until the next arrived: in 0.3.116's run the player found it empty 377 times in 30 s (3.3 s of silence, one
   gap per 85 ms buffer), every gap made the device late by the guest's reckoning, and coreaudiod re-anchored its
   clock and reported an IO overload about once a second (Chrome's cuts; what Safari's GPU process dies logging).

   Here a position `v` (bytes of the stream, as `written` and `consumed` count them) advances at the stream's rate
   with the host's clock. A buffer goes back when `v` passes its end, at the pace a sound card would play it. The
   player plays `lead` behind `v`: what the guest sends late, by less than the lead, is still in time, and nothing
   of that shows in when its buffers come back. `v` may run at most `lead` ahead of what the guest has sent
   (beyond that the guest is late for good: the player has silence, the buffers come back late). After a quarter
   second there (no sound at all), `v` falls back to what was sent, so the next sound starts with the whole lead.

   No dependencies: tests/test_snd_pace.c runs it against a guest that sends by its own timer and one that sends at
   each buffer's return. Callers serialize (the device's lock). */

#include <stdint.h>

typedef struct HvSndPace {
    double v;                /* the virtual position, bytes */
    uint64_t last_ns;        /* when `v` was last advanced */
    uint64_t held_since_ns;  /* since when `v` has been held at written + lead; 0: it is not */
    uint64_t idle_written;   /* idle: `v` waits at this `written` until the guest sends more */
    int idle;
    uint32_t lead;           /* bytes the player stays behind `v` */
    uint32_t rate;           /* bytes a second */
    /* what happened, for the log: counted here, read and cleared by the backend */
    uint64_t holds;          /* times the guest's sound was later than the lead */
    uint64_t held_ns;        /* for how long in all */
    uint64_t held_max_ns;    /* the longest */
} HvSndPace;

#define HV_SND_PACE_IDLE_NS 250000000ull

/* A stream starts idle: `v` runs from the first sound the guest sends, not from the start request. */
static inline void hv_snd_pace_start(HvSndPace *p, uint64_t written, uint64_t now_ns, uint32_t rate, uint32_t lead) {
    p->v = (double)written;
    p->last_ns = now_ns;
    p->held_since_ns = 0;
    p->idle = 1;
    p->idle_written = written;
    p->rate = rate;
    p->lead = lead;
}

/* Advances `v` to `now_ns`; returns the position buffers have been played to, for the guest: one whose end is at or
   below it goes back. */
static inline uint64_t hv_snd_pace_advance(HvSndPace *p, uint64_t written, uint64_t now_ns) {
    if (p->idle) {
        if (written == p->idle_written) {
            p->last_ns = now_ns;
            return p->v > 0 ? (uint64_t)p->v : 0;
        }
        p->idle = 0; /* sound again: `v` runs from here, the player the lead behind it */
        p->last_ns = now_ns;
    }
    if (now_ns > p->last_ns) {
        uint64_t dt = now_ns - p->last_ns;
        if (dt > 1000000000ull) dt = 1000000000ull; /* a stopped process: not a second of sound at once */
        p->v += (double)dt * (double)p->rate / 1e9;
        p->last_ns = now_ns;
    }
    double limit = (double)written + (double)p->lead;
    if (p->v >= limit) {
        p->v = limit;
        if (!p->held_since_ns) {
            p->held_since_ns = now_ns ? now_ns : 1;
            p->holds++;
        } else if (now_ns - p->held_since_ns >= HV_SND_PACE_IDLE_NS) {
            /* No sound for a quarter second: not lateness, the stream is idle. `v` falls back to what was sent, so
               the next sound starts with the whole lead. */
            if (p->holds) p->holds--;
            p->held_since_ns = 0;
            p->v = (double)written;
            p->idle = 1;
            p->idle_written = written;
        }
    } else if (p->held_since_ns) {
        uint64_t held = now_ns - p->held_since_ns;
        p->held_ns += held;
        if (held > p->held_max_ns) p->held_max_ns = held;
        p->held_since_ns = 0;
    }
    return p->v > 0 ? (uint64_t)p->v : 0;
}

/* After hv_snd_pace_advance: the position the player may have played to (never past what was sent). */
static inline uint64_t hv_snd_pace_play_to(const HvSndPace *p, uint64_t written) {
    double t = p->v - (double)p->lead;
    if (t <= 0) return 0;
    uint64_t u = (uint64_t)t;
    return u > written ? written : u;
}

/* Whether the guest is late right now (the player's silence is the guest's, not the lead's at a start). */
static inline int hv_snd_pace_held(const HvSndPace *p) { return p->held_since_ns != 0; }

#endif
