#include "virtio_snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTLQ 0
#define EVENTQ 1
#define TXQ 2
#define RXQ 3

/* request codes */
#define R_JACK_INFO 1
#define R_JACK_REMAP 2
#define R_PCM_INFO 0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE 0x0102
#define R_PCM_RELEASE 0x0103
#define R_PCM_START 0x0104
#define R_PCM_STOP 0x0105
#define R_CHMAP_INFO 0x0200
/* status codes */
#define S_OK 0x8000
#define S_BAD_MSG 0x8001
#define S_NOT_SUPP 0x8002

#define CHMAP_FL 3
#define CHMAP_FR 4

static int logging(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_SND_LOG") != NULL;
    return on;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 0.3.118: the first 36 events after each of the first 6 starts, each with its time since that start in ms. */
static void tr_event(HvVirtioSnd *d, const char *what, uint64_t a, uint64_t b) {
    if (d->tr_starts > 6 || d->tr_first_events >= 36 || d->tr_first_len + 48 > sizeof d->tr_first) return;
    uint64_t now = now_ns();
    int n = snprintf(d->tr_first + d->tr_first_len, sizeof d->tr_first - d->tr_first_len, " +%.1f %s%llu/%llu",
                     d->tr_start_ns ? (double)(now - d->tr_start_ns) / 1e6 : 0.0, what, (unsigned long long)a, (unsigned long long)b);
    if (n > 0) d->tr_first_len += (uint32_t)n, d->tr_first_events++;
}

int hv_virtio_snd_trace_line(HvVirtioSnd *d, char *out, unsigned cap) {
    if (!d->tr_arrivals && !d->tr_dones && !d->tr_first_len) return 0;
    int n = snprintf(out, cap, "%u buffers arrived %.1f ms apart (%.1f to %.1f; %u within 5 ms of the one before), %u went back %.1f ms "
                               "apart (%.1f to %.1f), at most %u out at once",
                     d->tr_arrivals, d->tr_arrivals > 1 ? (double)d->tr_arrive_sum_us / 1e3 / (d->tr_arrivals - 1) : 0.0,
                     d->tr_arrive_min_us / 1e3, d->tr_arrive_max_us / 1e3, d->tr_bursts, d->tr_dones,
                     d->tr_dones > 1 ? (double)d->tr_done_sum_us / 1e3 / (d->tr_dones - 1) : 0.0, d->tr_done_min_us / 1e3,
                     d->tr_done_max_us / 1e3, d->tr_out_max);
    if (d->tr_first_len && n > 0 && (unsigned)n < cap)
        snprintf(out + n, cap - (unsigned)n, "; after a start (ms; tx size/out, done end/latency, ctl code/status):%s", d->tr_first);
    d->tr_arrivals = d->tr_dones = d->tr_bursts = d->tr_out_max = 0;
    d->tr_arrive_sum_us = d->tr_done_sum_us = 0;
    d->tr_arrive_min_us = d->tr_arrive_max_us = d->tr_done_min_us = d->tr_done_max_us = 0;
    d->tr_last_arrive_ns = d->tr_last_done_ns = 0;
    d->tr_first_len = 0, d->tr_first[0] = 0;
    return 1;
}

static unsigned format_bytes(unsigned f) {
    return f == HV_VSND_FMT_S16 ? 2 : 4;
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int i = 0; i < e->nseg; i++)
        if (!e->seg[i].write) n += e->seg[i].len;
    return n;
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ---- control queue ---- */

static uint32_t control(HvVirtioSnd *d, const HvVirtqElem *e) {
    HvVirtio *v = &d->vio;
    uint8_t req[32] = {0}, resp[4 + 32];
    uint32_t got = hv_virtq_read(v, e, 0, req, sizeof req);
    uint32_t code, status = S_OK, rlen = 4;
    memcpy(&code, req, 4);
    memset(resp, 0, sizeof resp);
    uint32_t start, count, size, stream;
    memcpy(&start, req + 4, 4);
    memcpy(&count, req + 8, 4);
    memcpy(&size, req + 12, 4);
    stream = start; /* virtio_snd_pcm_hdr: stream_id at the same place */
    if (got < 4) code = 0;
    switch (code) {
    case R_PCM_INFO:
    case R_CHMAP_INFO:
        if (start != 0 || count != 1 || size == 0 || size > 32) {
            status = S_BAD_MSG;
            break;
        }
        if (code == R_PCM_INFO) {
            uint64_t formats = 1ull << HV_VSND_FMT_S16 | 1ull << HV_VSND_FMT_S32 | 1ull << HV_VSND_FMT_FLOAT;
            uint64_t rates = 1ull << HV_VSND_RATE_44100 | 1ull << HV_VSND_RATE_48000;
            memcpy(resp + 4 + 8, &formats, 8);
            memcpy(resp + 4 + 16, &rates, 8);
            resp[4 + 24] = 0; /* output */
            resp[4 + 25] = 1; /* channels min */
            resp[4 + 26] = 2; /* channels max */
        } else {
            resp[4 + 4] = 0; /* output */
            resp[4 + 5] = 2;
            resp[4 + 6] = CHMAP_FL;
            resp[4 + 7] = CHMAP_FR;
        }
        rlen = 4 + size;
        break;
    case R_JACK_INFO: /* one line-out jack, always connected */
        if (start != 0 || count != 1 || size == 0 || size > 32) {
            status = S_BAD_MSG;
            break;
        }
        put32(resp + 4 + 8, 0x01014010); /* HDA pin default config: line out, rear, green jack */
        put32(resp + 4 + 12, 0x10);      /* pin capabilities: output */
        resp[4 + 16] = 1;                /* connected */
        rlen = 4 + size;
        break;
    case R_JACK_REMAP:
        break;
    case R_PCM_SET_PARAMS: {
        uint32_t buffer_bytes, period_bytes;
        memcpy(&buffer_bytes, req + 8, 4);
        memcpy(&period_bytes, req + 12, 4);
        unsigned ch = req[20], fmt = req[21], rate = req[22];
        if (stream != 0 || ch < 1 || ch > 2 || (fmt != HV_VSND_FMT_S16 && fmt != HV_VSND_FMT_S32 && fmt != HV_VSND_FMT_FLOAT) ||
            (rate != HV_VSND_RATE_44100 && rate != HV_VSND_RATE_48000) || d->running) {
            status = S_NOT_SUPP;
            break;
        }
        d->channels = ch;
        d->format = fmt;
        d->rate = rate;
        d->rate_hz = rate == HV_VSND_RATE_44100 ? 44100 : 48000;
        d->frame_bytes = ch * format_bytes(fmt);
        if (buffer_bytes != d->buffer_bytes || period_bytes != d->period_bytes)
            fprintf(stderr, "hengevm: sound: the Mac's driver asks for a buffer of %u frames in periods of %u (%u bytes a frame)\n",
                    d->frame_bytes ? buffer_bytes / d->frame_bytes : 0, d->frame_bytes ? period_bytes / d->frame_bytes : 0, d->frame_bytes);
        d->buffer_bytes = buffer_bytes;
        d->period_bytes = period_bytes;
        break;
    }
    case R_PCM_PREPARE:
        if (stream != 0 || !d->frame_bytes) status = S_BAD_MSG;
        else d->prepared = 1;
        break;
    case R_PCM_START:
        if (stream != 0 || !d->prepared) {
            status = S_BAD_MSG;
            break;
        }
        if (!d->running) {
            if (d->pace_on) {
                pthread_mutex_lock(&d->pace_mu);
                hv_snd_pace_start(&d->pace, atomic_load(&d->written), now_ns(), d->rate_hz * d->frame_bytes,
                                  d->rate_hz / 1000 * d->pace_lead_ms * d->frame_bytes);
                pthread_mutex_unlock(&d->pace_mu);
            }
            d->running = 1;
            d->clock_start_ns = now_ns();
            d->clock_start_consumed = atomic_load(&d->consumed);
            if (d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
                d->backend.start = NULL; /* the host cannot play it: drain by the clock instead */
        }
        break;
    case R_PCM_STOP:
        if (stream != 0) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        break;
    case R_PCM_RELEASE:
        if (stream != 0) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        d->prepared = 0;
        /* Every buffer still held goes back now (the spec requires it). */
        for (; d->pcopy != d->ptail; d->pcopy++) d->pending_end[d->pcopy % HV_VSND_PENDING] = atomic_load(&d->written);
        atomic_store(&d->consumed, atomic_load(&d->written));
        break;
    default:
        status = S_NOT_SUPP;
        break;
    }
    if (code != R_PCM_INFO && code != R_JACK_INFO && code != R_CHMAP_INFO) {
        /* 0.3.118: every request that changes the stream, said, a refused one too (the first 40). */
        static int said;
        if (code == R_PCM_START) d->tr_starts++, d->tr_first_events = 0, d->tr_start_ns = now_ns();
        tr_event(d, "ctl", code, status);
        if (said++ < 40)
            fprintf(stderr, "hengevm: sound: the Mac's driver: request %#x -> %s (%u channels, format %u, rate code %u = %u Hz, buffer %u "
                            "bytes, period %u; %s)\n", code, status == S_OK ? "ok" : status == S_NOT_SUPP ? "NOT SUPPORTED" : "BAD MESSAGE",
                    code == R_PCM_SET_PARAMS ? req[20] : d->channels, code == R_PCM_SET_PARAMS ? req[21] : d->format,
                    code == R_PCM_SET_PARAMS ? req[22] : d->rate, d->rate_hz, d->buffer_bytes, d->period_bytes,
                    d->running ? "running" : d->prepared ? "prepared" : "stopped");
    }
    if (logging())
        fprintf(stderr, "virtio-snd: request %#x [%u %u %u] -> %#x (ch %u fmt %u rate %u buffer %u period %u)\n", code, start, count, size, status,
                d->channels, d->format, d->rate_hz, d->buffer_bytes, d->period_bytes);
    put32(resp, status);
    return hv_virtq_write(v, e, 0, resp, rlen);
}

/* ---- transmit ---- */

/* Copy waiting buffers' audio into the ring while there is room. Their data is
   still in guest memory, so copying can wait. */

static void fill_ring(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    while (d->pcopy != d->ptail) {
        HvVirtqElem *e = &d->pending[d->pcopy % HV_VSND_PENDING];
        uint32_t len = readable(e);
        len = len > 4 ? len - 4 : 0;
        uint64_t w = atomic_load(&d->written);
        if (len > HV_VSND_RING) len = HV_VSND_RING;
        if (w + len - atomic_load(&d->consumed) > HV_VSND_RING) { d->ring_full++; return; } /* full: later */
        uint32_t off = 0;
        while (off < len) {
            uint32_t at = (uint32_t)((w + off) % HV_VSND_RING), n = len - off;
            if (n > HV_VSND_RING - at) n = HV_VSND_RING - at;
            hv_virtq_read(v, e, 4 + off, d->ring + at, n);
            off += n;
        }
        atomic_store_explicit(&d->written, w + len, memory_order_release);
        d->tx_count++;
        if (!d->tx_bytes_min || len < d->tx_bytes_min) d->tx_bytes_min = len;
        if (len > d->tx_bytes_max) d->tx_bytes_max = len;
        d->pending_end[d->pcopy % HV_VSND_PENDING] = w + len;
        d->pcopy++;
        d->xfers++;
    }
}

static void take_tx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    if (!(v->status & 4) || !v->q[TXQ].enabled) return;
    while (d->ptail - d->phead < HV_VSND_PENDING) {
        if (hv_virtq_pop(v, TXQ, &d->pending[d->ptail % HV_VSND_PENDING]) <= 0) break;
        d->ptail++;
        {
            uint64_t t = now_ns();
            uint32_t out = d->ptail - d->phead;
            if (d->tr_last_arrive_ns) {
                uint32_t us = (uint32_t)((t - d->tr_last_arrive_ns) / 1000);
                if (!d->tr_arrive_min_us || us < d->tr_arrive_min_us) d->tr_arrive_min_us = us;
                if (us > d->tr_arrive_max_us) d->tr_arrive_max_us = us;
                d->tr_arrive_sum_us += us;
                if (us < 5000) d->tr_bursts++;
            }
            d->tr_last_arrive_ns = t, d->tr_arrivals++;
            if (out > d->tr_out_max) d->tr_out_max = out;
            tr_event(d, "tx", readable(&d->pending[(d->ptail - 1) % HV_VSND_PENDING]), out);
        }
    }
    fill_ring(d);
}

void hv_virtio_snd_poll(HvVirtioSnd *d) {
    if (!d) return;
    HvVirtio *v = &d->vio;
    /* No backend: the stream plays at its rate by the wall clock. */
    if (d->running && !d->backend.start && d->frame_bytes) {
        uint64_t now = now_ns();
        uint64_t target = d->clock_start_consumed +
                          (now - d->clock_start_ns) * d->rate_hz / 1000000000ull * d->frame_bytes;
        uint64_t w = atomic_load(&d->written);
        if (target > w) { /* ran dry: restart the clock from here */
            target = w;
            d->clock_start_ns = now;
            d->clock_start_consumed = w;
        }
        if (target > atomic_load(&d->consumed)) atomic_store(&d->consumed, target);
    }
    take_tx(d);
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_acquire);
    if (d->pace_on && d->running && d->backend.start && d->pace.rate) {
        /* 0.3.117: a buffer goes back when the device's clock passes its end (snd_pace.h), not when the player has
           taken its last byte. Never behind the player: what it has taken is played. */
        pthread_mutex_lock(&d->pace_mu);
        uint64_t pos = hv_snd_pace_advance(&d->pace, atomic_load(&d->written), now_ns());
        pthread_mutex_unlock(&d->pace_mu);
        if (pos > c) c = pos;
    }
    int any = 0;
    while (d->phead != d->pcopy && d->pending_end[d->phead % HV_VSND_PENDING] <= c) {
        HvVirtqElem *e = &d->pending[d->phead % HV_VSND_PENDING];
        uint8_t st[8] = {0};
        put32(st, S_OK);
        /* latency_bytes. 0.3.115: 0, as QEMU's device answers. Up to 0.3.114 this was what the ring still held, about
           one of the guest's buffers (a quarter second); in 0.3.114's run coreaudiod refused every timestamp of the
           sound device as about that far in the future ("Out of bounds host time detected", each 0.26 s), re-anchored
           its clock and reported IO overloads: the cuts in Chrome's sound, and what Safari's GPU process dies logging.
           HV_SND_LATENCY=1 answers the ring's fill again. */
        static int report_latency = -1;
        if (report_latency < 0) { const char *e = getenv("HV_SND_LATENCY"); report_latency = e && *e == '1'; }
        uint64_t queued = report_latency ? atomic_load(&d->written) - c : 0;
        put32(st + 4, (uint32_t)queued);
        hv_virtq_write(v, e, 0, st, 8);
        hv_virtq_push(v, TXQ, e, 8);
        {
            uint64_t t = now_ns();
            if (d->tr_last_done_ns) {
                uint32_t us = (uint32_t)((t - d->tr_last_done_ns) / 1000);
                if (!d->tr_done_min_us || us < d->tr_done_min_us) d->tr_done_min_us = us;
                if (us > d->tr_done_max_us) d->tr_done_max_us = us;
                d->tr_done_sum_us += us;
            }
            d->tr_last_done_ns = t, d->tr_dones++;
            tr_event(d, "done", d->pending_end[d->phead % HV_VSND_PENDING], queued);
        }
        d->phead++;
        any = 1;
    }
    if (any) {
        hv_virtio_interrupt(v);
        take_tx(d); /* room for more */
    }
}

uint32_t hv_virtio_snd_pull(HvVirtioSnd *d, void *buf, uint32_t len) {
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->written, memory_order_acquire);
    uint64_t have = w - c;
    int late = 1; /* a short pull is the guest's lateness (without the device's clock it always is) */
    if (d->pace_on && d->running && d->frame_bytes && d->pace.rate) {
        /* 0.3.117: the player stays the lead behind the device's clock (snd_pace.h). */
        pthread_mutex_lock(&d->pace_mu);
        hv_snd_pace_advance(&d->pace, w, now_ns());
        uint64_t to = hv_snd_pace_play_to(&d->pace, w);
        late = hv_snd_pace_held(&d->pace);
        pthread_mutex_unlock(&d->pace_mu);
        uint64_t may = to > c ? to - c : 0;
        /* A player that fell behind (its thread held up for longer than it makes up for) drops what is over a
           tenth of a second, rather than play that much later from then on. */
        uint64_t most = (uint64_t)len + (uint64_t)d->rate_hz / 10 * d->frame_bytes;
        if (may > most) {
            uint64_t drop = may - len;
            drop -= drop % d->frame_bytes;
            if (atomic_compare_exchange_strong(&d->consumed, &c, c + drop)) d->dropped_bytes += drop, c += drop;
            may = to > c ? to - c : 0;
            have = w - c;
        }
        may -= may % d->frame_bytes;
        if (have > may) have = may;
    }
    uint32_t n = have < len ? (uint32_t)have : len;
    uint32_t off = 0;
    while (off < n) {
        uint32_t at = (uint32_t)((c + off) % HV_VSND_RING), k = n - off;
        if (k > HV_VSND_RING - at) k = HV_VSND_RING - at;
        memcpy((uint8_t *)buf + off, d->ring + at, k);
        off += k;
    }
    if (n < len) {
        memset((uint8_t *)buf + n, 0, len - n);
        if (d->running && late) d->underruns++, d->short_bytes += len - n;
    }
    /* compare-exchange: a release on the CPU thread may have moved it on */
    atomic_compare_exchange_strong(&d->consumed, &c, c + n);
    if (n || d->pace_on) hv_machine_wake(d->vio.m); /* with the device's clock a buffer may be due between pulls */
    return n;
}

static void notify(HvVirtio *v, int q) {
    HvVirtioSnd *d = v->dev;
    static HvVirtqElem e;
    if (q == CTLQ) {
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            hv_virtq_push(v, q, &e, control(d, &e));
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
        hv_virtio_snd_poll(d); /* a release may have freed buffers */
    } else if (q == TXQ) {
        take_tx(d);
    } else if (q == RXQ) { /* no input stream: hand capture buffers straight back */
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            uint8_t st[8] = {0};
            put32(st, S_BAD_MSG);
            hv_virtq_write(v, &e, 0, st, 8);
            hv_virtq_push(v, q, &e, 8);
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
    } /* event queue: kept for events we never send */
}

HvVirtioSnd *hv_virtio_snd_create(HvMachine *m, HvPcie *bus) {
    HvVirtioSnd *d = calloc(1, sizeof *d);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 25, 0x040100, 4, 0); /* sound device, multimedia audio */
    v->dev = d;
    v->notify = notify;
    uint32_t jacks = 1, streams = 1, chmaps = 1;
    memcpy(v->config, &jacks, 4);
    memcpy(v->config + 4, &streams, 4);
    memcpy(v->config + 8, &chmaps, 4);
    v->config_len = 12;
    d->ring = calloc(1, HV_VSND_RING);
    pthread_mutex_init(&d->pace_mu, NULL);
    hv_pcie_attach(bus, &v->pci);
    return d;
}

/* After a pause or a restore: the host's player again for a stream that was playing, and the clock that drains it
   without one starts from now (host times from before mean nothing). */
void hv_virtio_snd_resume(HvVirtioSnd *d) {
    if (!d) return;
    d->clock_start_ns = now_ns();
    d->clock_start_consumed = atomic_load(&d->consumed);
    if (d->pace_on && d->running && d->frame_bytes) { /* the device's clock from now too */
        pthread_mutex_lock(&d->pace_mu);
        hv_snd_pace_start(&d->pace, atomic_load(&d->written), now_ns(), d->rate_hz * d->frame_bytes,
                          d->rate_hz / 1000 * d->pace_lead_ms * d->frame_bytes);
        pthread_mutex_unlock(&d->pace_mu);
    }
    if (d->running && d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
        d->backend.start = NULL;
}
