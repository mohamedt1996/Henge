/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Mac's sound on Android. The engine is a static program of its own, with none of Android's audio
   libraries, so it plays nothing itself: it serves the guest's PCM on 127.0.0.1 (HV_AUDIO_PORT, 5901),
   and the app (VmService's sound thread) plays what it reads through an AudioTrack.

   One thread pulls the virtio-snd ring at the stream's own rate by the clock, 10 ms at a time, whether
   or not a listener is there: that pull is what paces the guest's audio clock (virtio_snd.h), so the
   Mac runs the same with the app's sound on, off, or gone.

   The stream, little-endian: messages of an 8-byte header { 'H', 'S', type, 0, u32 length } and a body.
     type 1  the format, sent first and whenever it changes: u32 channels, u32 format (HV_VSND_FMT_*), u32 Hz
     type 2  PCM in that format
   A listener that falls behind is dropped (it reconnects), never waited for. */
#include "audio_mac.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static struct {
    HvVirtioSnd *d;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int running;
    unsigned channels, format, rate;
    int client;       /* the listener's socket, or -1 */
    int client_knows; /* it has been sent the format in force */
    uint64_t sent, dropped;
} A = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .client = -1};

static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k <= 0) return -1; /* gone, or behind for longer than the send timeout */
        b += k, n -= (size_t)k;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static int send_message(int fd, int type, const void *body, uint32_t len) {
    uint8_t h[8] = {'H', 'S', (uint8_t)type, 0};
    put32(h + 4, len);
    return send_all(fd, h, 8) || send_all(fd, body, len) ? -1 : 0;
}

/* Under A.mu. */
static void drop_client(const char *why) {
    if (A.client < 0) return;
    close(A.client);
    A.client = -1;
    fprintf(stderr, "hengevm: sound: the app's player left (%s)\n", why);
}

static void *pump_thread(void *unused) {
    (void)unused;
    static uint8_t buf[48000 / 100 * 8 * 4 * 2]; /* 10 ms of anything this device offers, with room */
    struct timespec next = {0};
    int was_running = 0;
    for (;;) {
        pthread_mutex_lock(&A.mu);
        while (!A.running) was_running = 0, pthread_cond_wait(&A.cv, &A.mu);
        unsigned bytes_per = A.format == HV_VSND_FMT_S16 ? 2 : 4;
        uint32_t len = A.rate / 100 * A.channels * bytes_per;
        if (len > sizeof buf) len = sizeof buf;
        hv_virtio_snd_pull(A.d, buf, len); /* what is not there yet is silence */
        if (A.client >= 0) {
            if (!A.client_knows) {
                uint8_t f[12];
                put32(f, A.channels), put32(f + 4, A.format), put32(f + 8, A.rate);
                if (send_message(A.client, 1, f, sizeof f)) drop_client("format");
                else A.client_knows = 1;
            }
            if (A.client >= 0) {
                if (send_message(A.client, 2, buf, len)) A.dropped++, drop_client("it fell behind");
                else A.sent++;
            }
        }
        /* 0.3.115, 0.3.117: every 30 s of playing, what the Mac's side of the sound did: its buffers, how often its
           sound was late (later than the lead, with the device's clock) and what silence that cost, a full ring, a
           late pump, sound dropped to catch up. */
        {
            static uint64_t passes, cuts0, short0, full0, late0, tx0, drop0;
            static uint64_t late; /* pulls that started 30 ms or more behind their time */
            if (A.d && ++passes % 3000 == 0) {
                HvVirtioSnd *d = A.d;
                uint64_t cuts = d->underruns - cuts0, sh = d->short_bytes - short0, full = d->ring_full - full0;
                unsigned fb = A.channels * bytes_per ? A.channels * bytes_per : 1, rate = A.rate ? A.rate : 48000;
                pthread_mutex_lock(&d->pace_mu);
                uint64_t holds = d->pace.holds, held_ms = d->pace.held_ns / 1000000, held_max = d->pace.held_max_ns / 1000000;
                d->pace.holds = 0, d->pace.held_ns = 0, d->pace.held_max_ns = 0;
                pthread_mutex_unlock(&d->pace_mu);
                fprintf(stderr, "hengevm: sound: last 30 s: %llu buffers from the Mac of %llu to %llu frames; its sound was late "
                                "%llu time(s) (%llu ms in all, %llu at most), %llu pull(s) came up short, %llu ms of silence played "
                                "for it; the ring was full %llu time(s); the pump ran 30 ms or more late %llu time(s); %llu ms dropped "
                                "to catch up\n",
                        (unsigned long long)(d->tx_count - tx0), (unsigned long long)(d->tx_bytes_min / fb),
                        (unsigned long long)(d->tx_bytes_max / fb), (unsigned long long)(d->pace_on ? holds : cuts),
                        (unsigned long long)held_ms, (unsigned long long)held_max, (unsigned long long)cuts,
                        (unsigned long long)(sh / fb * 1000 / rate), (unsigned long long)full, (unsigned long long)(late - late0),
                        (unsigned long long)((d->dropped_bytes - drop0) / fb * 1000 / rate));
                cuts0 = d->underruns, short0 = d->short_bytes, full0 = d->ring_full, late0 = late, tx0 = d->tx_count;
                drop0 = d->dropped_bytes, d->tx_bytes_min = 0, d->tx_bytes_max = 0;
            }
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            if (was_running && (int64_t)(t.tv_sec - next.tv_sec) * 1000000000 + (t.tv_nsec - next.tv_nsec) > 30000000) late++;
        }
        pthread_mutex_unlock(&A.mu);
        /* The next 10 ms, by the clock; after a pause or a stall, from now. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!was_running) next = now, was_running = 1;
        next.tv_nsec += 10000000;
        if (next.tv_nsec >= 1000000000) next.tv_nsec -= 1000000000, next.tv_sec++;
        int64_t behind = (int64_t)(now.tv_sec - next.tv_sec) * 1000000000 + (now.tv_nsec - next.tv_nsec);
        if (behind > 200000000) next = now; /* more than 200 ms late: do not burst to catch up */
        else clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

static void *accept_thread(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { usleep(100000); continue; }
        int one = 1, size = 1 << 16;
        struct timeval tv = {0, 100000}; /* a send that waits this long means the player has stopped reading */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        pthread_mutex_lock(&A.mu);
        drop_client("another took its place");
        A.client = fd, A.client_knows = 0;
        pthread_mutex_unlock(&A.mu);
        fprintf(stderr, "hengevm: sound: the app's player is listening\n");
    }
    return NULL;
}

static int start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    (void)opaque;
    pthread_mutex_lock(&A.mu);
    if (channels != A.channels || format != A.format || rate_hz != A.rate) {
        A.channels = channels, A.format = format, A.rate = rate_hz, A.client_knows = 0;
        fprintf(stderr, "hengevm: sound: the Mac plays %u channels, format %u, %u Hz\n", channels, format, rate_hz);
    }
    A.running = 1;
    pthread_cond_signal(&A.cv);
    pthread_mutex_unlock(&A.mu);
    return 0;
}

static void stop(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&A.mu); /* the pump pulls under it: none is under way once this returns */
    A.running = 0;
    pthread_mutex_unlock(&A.mu);
}

/* 0.3.117: the device's own clock (devices/snd_pace.h): buffers go back to the Mac at the stream's rate by this
   clock and the pump plays HV_SND_LEAD_MS (50) behind it. HV_SND_LEAD_MS=0: as up to 0.3.116. */
static void pace_setup(HvVirtioSnd *d) {
    const char *e = getenv("HV_SND_LEAD_MS");
    int ms = e && *e ? atoi(e) : 50;
    if (ms < 0) ms = 0;
    if (ms > 200) ms = 200;
    d->pace_lead_ms = (uint32_t)ms;
    d->pace_on = ms > 0;
    static int said;
    if (!said++)
        fprintf(stderr, ms ? "hengevm: sound: the device keeps its own clock; the player runs %d ms behind it\n"
                           : "hengevm: sound: buffers go back to the Mac as they are played out (HV_SND_LEAD_MS=%d)\n", ms);
}

static int listening; /* the port and the threads stay from the first attach on: the app keeps its connection */

void hv_audio_attach(HvVirtioSnd *d) {
    if (listening) { /* again, after hv_audio_detach (a Mac paused and run on): the same port and app */
        pthread_mutex_lock(&A.mu);
        A.d = d;
        pthread_mutex_unlock(&A.mu);
        d->backend = (HvVirtioSndBackend){start, stop, d};
        pace_setup(d);
        return;
    }
    const char *p = getenv("HV_AUDIO_PORT");
    int port = p && *p ? atoi(p) : 5901;
    if (port <= 0) return; /* HV_AUDIO_PORT=0: no sound; the device drains by the clock */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pthread_t t;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) {
        fprintf(stderr, "hengevm: sound: port %d: %s; no sound\n", port, strerror(errno));
        close(ls);
        return;
    }
    A.d = d;
    if (pthread_create(&t, NULL, accept_thread, (void *)(intptr_t)ls)) { close(ls); return; }
    pthread_detach(t);
    if (pthread_create(&t, NULL, pump_thread, NULL)) return;
    pthread_detach(t);
    listening = 1;
    d->backend = (HvVirtioSndBackend){start, stop, d};
    pace_setup(d);
    fprintf(stderr, "hengevm: sound on 127.0.0.1:%d, for the app to play\n", port);
}

/* The Mac paused (instant resume) or stopping: nothing more is played; the stream's state stays in the device. */
void hv_audio_detach(void) {
    stop(NULL);
}
