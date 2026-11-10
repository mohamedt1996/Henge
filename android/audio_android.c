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

void hv_audio_attach(HvVirtioSnd *d) {
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
    d->backend = (HvVirtioSndBackend){start, stop, d};
    fprintf(stderr, "hengevm: sound on 127.0.0.1:%d, for the app to play\n", port);
}
