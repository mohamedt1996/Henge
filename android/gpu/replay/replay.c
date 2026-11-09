/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The replay: the ring of commands from the engine, carried out on the CPU in guest memory (see
   replay.h), and a presenter thread that shows the surface macOS last swapped to, with the pointer. */
#include "replay.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

struct Replay {
    Guest g;
    int own_guest, have_guest;
    char link_path[1024], tool[1024], cache[1024];
    Objects os;
    pthread_mutex_t lock; /* objects and surfaces: the ring thread and the presenter */
    Shaders *shaders;
    Gpu *gpu;
    Screen *screen;
    int test_screen;
    void *window, *new_window;
    int window_changed;
    pthread_t ring, presenter;
    int stop, running;
    pthread_cond_t wake, applied;
    /* what is on screen */
    uint32_t mapping;
    int dirty;
    uint8_t *frame;
    uint32_t fw, fh;
    /* the pointer */
    uint8_t *cursor;
    uint32_t cw, ch, cstride;
    int hx, hy, cursor_shown;
    uint32_t cursor_at;
    /* what happened */
    uint64_t commands, execs, passes, clears, draws, blits, infos, swaps, frames, unknown;
};

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void on_function(void *ctx, uint32_t task, const uint8_t *lib, uint32_t len) {
    (void)task;
    Replay *r = ctx;
    shaders_library(r->shaders, lib, len);
}

/* ------------------------------------------------------------------ texels -- */
static uint8_t *texel(TexView *t, uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t z, uint64_t bytes) {
    if (!t->mem || !*t->mem || level >= t->mips || slice >= t->slices) return NULL;
    uint64_t at = t->level_off[level] + (uint64_t)slice * t->slice_stride + (uint64_t)z * t->level_img[level] +
                  (uint64_t)y * t->level_row[level] + (uint64_t)x * px_bytes(t->format);
    if (at + bytes > t->limit) return NULL;
    return *t->mem + at;
}

static uint32_t level_dim(uint32_t v, uint32_t level) { return v >> level ? v >> level : 1; }

static void fill_level(Replay *r, TexView *t, uint32_t level, uint32_t slice, uint32_t w, uint32_t h, const double rgba[4]) {
    uint8_t px[16];
    unsigned n = px_encode(t->format, rgba, px);
    if (!n) { r->unknown++; return; }
    uint32_t lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    if (!w || w > lw) w = lw;
    if (!h || h > lh) h = lh;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = texel(t, level, slice, 0, y, 0, (uint64_t)w * n);
        if (!row) return;
        if (n == 4) { uint32_t v; memcpy(&v, px, 4); uint32_t *p = (uint32_t *)row; for (uint32_t x = 0; x < w; x++) p[x] = v; }
        else for (uint32_t x = 0; x < w; x++) memcpy(row + (size_t)x * n, px, n);
    }
    r->clears++;
}

/* ---------------------------------------------------------- command records -- */
typedef struct { const uint8_t *p; uint32_t size; } Rec;
#define U32(off) ({ uint32_t _v = 0; if ((off) + 4 <= rc.size) memcpy(&_v, rc.p + (off), 4); _v; })
#define U16(off) ({ uint16_t _v = 0; if ((off) + 2 <= rc.size) memcpy(&_v, rc.p + (off), 2); _v; })
#define U64(off) ({ uint64_t _v = 0; if ((off) + 8 <= rc.size) memcpy(&_v, rc.p + (off), 8); _v; })

/* A render pass: what it clears is cleared now. Attachments are 0x3c bytes each from 0x4c:
   {texture, resolve, level, slice, depth plane, ..., load action @20, store action @22}, clear colour
   as four doubles at +0x1c; the render target's size at 0x238. */
static void begin_pass(Replay *r, Rec rc, uint32_t task) {
    r->passes++;
    uint32_t rw = 0, rh = 0;
    if (rc.size >= 0x248) rw = (uint32_t)U64(0x238), rh = (uint32_t)U64(0x240);
    for (int i = 0; i < 8; i++) {
        uint32_t o = 0x4c + (uint32_t)i * 0x3c;
        if (o + 0x3c > rc.size) break;
        uint32_t ref = U32(o);
        uint16_t level = U16(o + 8), slice = U16(o + 10), load = U16(o + 20), store = U16(o + 22);
        if (!ref || load != 2 /* MTLLoadActionClear */ || store == 0 /* DontCare */) continue;
        double c[4];
        memcpy(c, rc.p + o + 0x1c, sizeof c);
        Obj *t = obj_get(&r->os, task, ref);
        if (t && t->has_tex) fill_level(r, &t->tex, level, slice, rw, rh, c);
        else r->unknown++;
    }
}

typedef struct { uint64_t x, y, z; } Size3;

static void copy_rows(Replay *r, TexView *t, uint32_t level, uint32_t slice, Size3 org, Size3 sz,
                      uint8_t *buf, uint64_t blen, uint64_t off, uint64_t row, uint64_t img, int to_tex) {
    unsigned bpp = px_bytes(t->format);
    if (!bpp || !buf) { r->unknown++; return; }
    uint64_t line = sz.x * bpp;
    if (!row) row = line;
    if (!img) img = row * sz.y;
    for (uint64_t z = 0; z < (sz.z ? sz.z : 1); z++)
        for (uint64_t y = 0; y < sz.y; y++) {
            uint64_t b = off + z * img + y * row;
            uint8_t *p = texel(t, level, slice, (uint32_t)org.x, (uint32_t)(org.y + y), (uint32_t)(org.z + z), line);
            if (!p || b + line > blen) return;
            if (to_tex) memcpy(p, buf + b, line); else memcpy(buf + b, p, line);
        }
    r->blits++;
}

static Size3 size3(Rec rc, uint32_t at) { Size3 s = {U64(at), U64(at + 8), U64(at + 16)}; return s; }

static void blit(Replay *r, uint32_t tag, Rec rc, uint32_t task) {
    switch (tag) {
    case 300: { /* buffer to texture */
        Obj *b = obj_get(&r->os, task, U32(0)), *t = obj_get(&r->os, task, U32(4));
        uint64_t blen = 0;
        uint8_t *bytes = obj_bytes(b, &blen);
        if (!t || !t->has_tex) { r->unknown++; break; }
        copy_rows(r, &t->tex, U16(0x52), U16(0x50), size3(rc, 0x38), size3(rc, 0x20), bytes, blen, U64(8), U64(0x10), U64(0x18), 1);
        break;
    }
    case 301: { /* buffer to buffer */
        uint64_t al = 0, bl = 0;
        uint8_t *a = obj_bytes(obj_get(&r->os, task, U32(0)), &al), *b = obj_bytes(obj_get(&r->os, task, U32(4)), &bl);
        uint64_t from = U64(8), to = U64(0x10), n = U64(0x18);
        if (a && b && from + n <= al && to + n <= bl) memmove(b + to, a + from, n), r->blits++;
        else r->unknown++;
        break;
    }
    case 302: { /* texture to buffer */
        Obj *t = obj_get(&r->os, task, U32(0)), *b = obj_get(&r->os, task, U32(4));
        uint64_t blen = 0;
        uint8_t *bytes = obj_bytes(b, &blen);
        if (!t || !t->has_tex) { r->unknown++; break; }
        copy_rows(r, &t->tex, U16(0x52), U16(0x50), size3(rc, 8), size3(rc, 0x20), bytes, blen, U64(0x38), U64(0x40), U64(0x48), 0);
        break;
    }
    case 303: { /* texture to texture */
        Obj *a = obj_get(&r->os, task, U32(0)), *b = obj_get(&r->os, task, U32(4));
        if (!a || !b || !a->has_tex || !b->has_tex || px_bytes(a->tex.format) != px_bytes(b->tex.format)) { r->unknown++; break; }
        Size3 from = size3(rc, 8), sz = size3(rc, 0x20), to = size3(rc, 0x38);
        uint64_t line = sz.x * px_bytes(a->tex.format);
        for (uint64_t z = 0; z < (sz.z ? sz.z : 1); z++)
            for (uint64_t y = 0; y < sz.y; y++) {
                uint8_t *s = texel(&a->tex, U16(0x52), U16(0x50), (uint32_t)from.x, (uint32_t)(from.y + y), (uint32_t)(from.z + z), line);
                uint8_t *d = texel(&b->tex, U16(0x56), U16(0x54), (uint32_t)to.x, (uint32_t)(to.y + y), (uint32_t)(to.z + z), line);
                if (s && d) memmove(d, s, line);
            }
        r->blits++;
        break;
    }
    case 306: { /* fill: {buffer, location, length, value} */
        uint64_t bl = 0;
        uint8_t *b = obj_bytes(obj_get(&r->os, task, U32(0)), &bl);
        uint64_t at = U64(4), n = U64(12);
        if (b && at + n <= bl && rc.size > 20) memset(b + at, rc.p[20], n), r->blits++;
        break;
    }
    case 307: case 308: case 309: case 310: case 311: case 314: case 315: case 316: case 317:
        break; /* mipmaps (left as they are), hints, fences: nothing to do on the CPU */
    default: r->unknown++; break;
    }
}

/* What the guest asks about objects, answered in its own buffers ({ref, buffer ref, offset}). The
   values only have to be consistent: the replay that later reads argument buffers maps them back. */
static void info(Replay *r, uint32_t tag, Rec rc, uint32_t task) {
    r->infos++;
    uint32_t ref = U32(0), bufref = U32(4);
    uint64_t off = U64(8);
    if (tag == 451 || tag == 469) { /* a texture's size in a heap: {description, buffer, offset} */
        uint32_t at = tag == 451 ? 0x20 : 0x28;
        Obj *to = obj_get(&r->os, task, U32(at));
        uint64_t len = 0, where = U64(at + 4);
        uint8_t *out = obj_bytes(to, &len);
        if (!out || where + 16 > len) return;
        uint32_t w, h;
        uint16_t fmt;
        if (tag == 451) memcpy(&w, rc.p + 4, 4), memcpy(&h, rc.p + 8, 4), fmt = (uint16_t)(rc.p[2] | rc.p[3] << 8);
        else memcpy(&w, rc.p + 7, 4), memcpy(&h, rc.p + 11, 4), fmt = (uint16_t)(rc.p[1] | rc.p[2] << 8);
        uint64_t bytes = (uint64_t)(px_bytes(fmt) ? px_bytes(fmt) : 4) * (w ? w : 1) * (h ? h : 1) * 2; /* room for mips */
        uint64_t v[2] = {(bytes + GPAGE - 1) & ~(GPAGE - 1), GPAGE};
        memcpy(out + where, v, 16);
        return;
    }
    if (tag == 460) bufref = U32(8), off = U64(12);
    Obj *buf = obj_get(&r->os, task, bufref), *obj = obj_get(&r->os, task, ref);
    uint64_t len = 0;
    uint8_t *out = obj_bytes(buf, &len);
    if (!out || off + 28 > len || !obj) return;
    uint64_t id = obj->unique_id;
    switch (tag) {
    case 460: memcpy(out + off, &id, 8); break;
    case 457: { uint32_t v[2] = {1024, 0}; memcpy(out + off, v, 8); out[off + 8] = 0, out[off + 9] = 0; break; }
    case 450: { uint64_t v[3] = {1024, 32, 0}; memcpy(out + off, v, 24); out[off + 24] = 0; break; }
    case 461: case 463: { uint64_t a = id << 36; memcpy(out + off, &a, 8); break; } /* a buffer's or heap's "address" */
    case 462: case 464: case 466: case 467: case 468: { uint64_t v[2] = {id, id}; memcpy(out + off, v, 16); break; }
    default: r->unknown++; break;
    }
}

/* The engine takes a stale heartbeat for a closed app: beat during long work too, not only between commands. */
static void beat(Replay *r) {
    if (r->g.link) __atomic_store_n(&r->g.link->heartbeat, now_ns(), __ATOMIC_RELEASE);
}

static int is_draw(uint32_t tag) { return tag <= 19 || tag == 28 || tag == 29 || tag == 155 || tag == 162; }

/* One exec: {task, resources, buffers}, {ref, flags} per resource, {offset, length} per buffer.
   A buffer: segments {length, type | continues << 8 | more << 16} of records {tag, size, ...}. */
static void exec(Replay *r, const uint32_t *a, uint32_t n) {
    uint32_t task = a[0], nres = a[1], nbuf = a[2];
    if (12 + nres * 8ull + nbuf * 16ull > n) return;
    r->execs++;
    const uint32_t *res = a + 3;
    for (uint32_t i = 0; i < nres; i++) obj_refresh(&r->os, obj_get(&r->os, task, res[i * 2]));
    const uint8_t *bufs = (const uint8_t *)(res + nres * 2);
    for (uint32_t b = 0; b < nbuf; b++) {
        beat(r);
        uint64_t off, blen;
        memcpy(&off, bufs + b * 16, 8), memcpy(&blen, bufs + b * 16 + 8, 8);
        if (blen < 8 || blen > (64u << 20)) continue;
        uint8_t *copy = malloc(blen);
        if (!copy || guest_task_read(&r->g, task, off, copy, (uint32_t)blen)) { free(copy); continue; }
        const uint8_t *p = copy, *end = copy + blen;
        uint32_t seglen, inf;
        memcpy(&seglen, p, 4), memcpy(&inf, p + 4, 4);
        p += 8;
        if ((inf & 0xff) == 5) { /* protection options, then the real header */
            p += 8;
            if (p + 8 > end) { free(copy); continue; }
            memcpy(&seglen, p, 4), memcpy(&inf, p + 4, 4);
            p += 8;
        }
        uint32_t type = inf & 0xff, cont = (inf >> 8) & 0xff, more = (inf >> 16) & 0xff;
        if (p + seglen < end) end = p + seglen;
        /* A render encoder runs on across buffers while each says it continues; the pass ends with it. */
        if (!cont || type != 0) gpu_pass_end(r->gpu);
        while (p + 8 <= end) {
            uint32_t tag, size;
            memcpy(&tag, p, 4), memcpy(&size, p + 4, 4);
            if (size < 8 || p + size > end) break;
            Rec rc = {p + 8, size - 8};
            if (type == 0) {
                if (tag == 26) {
                    beat(r);
                    gpu_pass_end(r->gpu);
                    begin_pass(r, rc, task); /* the clears, on the CPU; a pass that draws clears again on the GPU */
                    gpu_pass_begin(r->gpu, task, rc.p, rc.size);
                } else if (gpu_record(r->gpu, tag, rc.p, rc.size)) {
                    if (is_draw(tag)) r->draws++;
                } else if (is_draw(tag)) r->draws++;
            } else if (type == 2) blit(r, tag, rc, task);
            else if (type == 4) info(r, tag, rc, task);
            p += size;
        }
        if (!more) gpu_pass_end(r->gpu);
        free(copy);
    }
    gpu_pass_end(r->gpu);
}

/* ---------------------------------------------------------------- commands -- */
static void cursor_glyph(Replay *r, const uint32_t *a, uint32_t n) {
    if (n < 0x2c) return;
    uint64_t off, stride;
    memcpy(&off, a + 2, 8), memcpy(&stride, a + 6, 8);
    uint16_t w, h, hx, hy;
    memcpy(&w, (const uint8_t *)a + 0x20, 2), memcpy(&h, (const uint8_t *)a + 0x22, 2);
    memcpy(&hx, (const uint8_t *)a + 0x24, 2), memcpy(&hy, (const uint8_t *)a + 0x26, 2);
    if (!w || !h || w > 512 || h > 512 || stride < (uint64_t)w * 4 || stride > 8192) return;
    uint8_t *bits = malloc((size_t)stride * h);
    if (!bits || guest_task_read(&r->g, 0, off, bits, (uint32_t)(stride * h))) { free(bits); return; }
    free(r->cursor);
    r->cursor = bits, r->cw = w, r->ch = h, r->cstride = (uint32_t)stride, r->hx = hx, r->hy = hy;
    r->dirty = 1;
}

void replay_command(Replay *r, uint32_t ch, const uint32_t *cmd, uint32_t len) {
    (void)ch;
    if (len < 12) return;
    uint32_t op = cmd[0] & 0xffff, skip = (cmd[0] >> 16) * 8;
    const uint32_t *args = (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
    uint32_t n = len > 12 + skip ? len - 12 - skip : 0;
    r->commands++;
    pthread_mutex_lock(&r->lock);
    if ((op == 0x43 || op == 0x45) && n >= 12) exec(r, args, n);
    else if ((op == 0x25 || op == 0x28) && n >= 8) obj_forget(&r->os, args[0], op == 0x25 ? args[1] : (n >= 16 ? args[3] : 0));
    else if (op == 0x20 && n >= 4) obj_forget_task(&r->os, args[0] >> 1);
    else if (op == 8 && n >= 16) { r->mapping = args[2], r->dirty = 1, r->swaps++; }
    else if (op == 4) { cursor_glyph(r, args, n); }
    else if (op == 5 && n >= 8) { r->cursor_shown = args[1] & 1, r->dirty = 1; }
    pthread_mutex_unlock(&r->lock);
    if (op == 8) pthread_cond_signal(&r->wake);
}

/* ---------------------------------------------------------------- presenting -- */
/* Copies the surface on screen into the frame, with the pointer over it. Lock held. */
static int compose(Replay *r) {
    Surface *s = r->mapping ? surface_get(&r->os, r->mapping) : NULL;
    if (!s || !s->w || !s->h || s->w > 8192 || s->h > 8192) return -1;
    if (r->fw != s->w || r->fh != s->h) {
        free(r->frame);
        r->frame = malloc((size_t)s->w * s->h * 4);
        r->fw = s->w, r->fh = s->h;
        if (!r->frame) { r->fw = r->fh = 0; return -1; }
    }
    for (uint32_t y = 0; y < s->h; y++) {
        uint64_t at = s->first + y * s->row;
        if (at + (uint64_t)s->w * (s->fourcc == 0x52476841 ? 8 : 4) > s->length) break;
        px_row_to_rgba8(s->fourcc, s->mem + at, r->frame + (size_t)y * s->w * 4, s->w);
    }
    const uint8_t *page = r->g.link->display_page ? guest_phys(&r->g, r->g.link->display_page, GPAGE) : NULL;
    if (!page || !r->cursor || !(r->cursor_shown || page[0xe04])) return 0;
    uint32_t where = __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE);
    if (where == 0xffffffff) return 0;
    int x = (int)(where & 0xffff) - r->hx, y = (int)(where >> 16) - r->hy;
    for (uint32_t cy = 0; cy < r->ch; cy++) {
        int py = y + (int)cy;
        if (py < 0 || py >= (int)r->fh) continue;
        const uint8_t *src = r->cursor + (size_t)cy * r->cstride;
        for (uint32_t cx = 0; cx < r->cw; cx++, src += 4) {
            int px = x + (int)cx;
            if (px < 0 || px >= (int)r->fw) continue;
            uint8_t *d = r->frame + ((size_t)py * r->fw + (size_t)px) * 4;
            uint32_t keep = 255u - src[3]; /* premultiplied BGRA over RGBA */
            d[0] = (uint8_t)(src[2] + (d[0] * keep + 127) / 255);
            d[1] = (uint8_t)(src[1] + (d[1] * keep + 127) / 255);
            d[2] = (uint8_t)(src[0] + (d[2] * keep + 127) / 255);
        }
    }
    return 0;
}

static uint32_t cursor_where(Replay *r) {
    const uint8_t *page = r->have_guest && r->g.link->display_page ? guest_phys(&r->g, r->g.link->display_page, GPAGE) : NULL;
    return page ? __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE) : 0;
}

void replay_present_now(Replay *r) {
    pthread_mutex_lock(&r->lock);
    int ok = compose(r) == 0;
    r->dirty = 0;
    pthread_mutex_unlock(&r->lock);
    if (ok && r->screen && screen_show(r->screen, r->frame, r->fw, r->fh) == 0) r->frames++;
}

static void set_screen_window(Replay *r) {
    pthread_mutex_lock(&r->lock);
    void *w = r->new_window;
    r->window_changed = 0;
    pthread_mutex_unlock(&r->lock);
    if (r->screen) screen_destroy(r->screen), r->screen = NULL;
#ifdef __ANDROID__
    if (r->window) ANativeWindow_release(r->window);
#endif
    r->window = w;
    if (w) r->screen = screen_create(w);
    r->dirty = 1;
    pthread_mutex_lock(&r->lock);
    pthread_cond_broadcast(&r->applied);
    pthread_mutex_unlock(&r->lock);
}

static void *present_loop(void *arg) {
    Replay *r = arg;
    uint64_t last_report = now_ns();
    while (!r->stop) {
        if (r->window_changed) set_screen_window(r);
        pthread_mutex_lock(&r->lock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 16 * 1000000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        if (!r->dirty) pthread_cond_timedwait(&r->wake, &r->lock, &until);
        pthread_mutex_unlock(&r->lock);
        uint32_t where = cursor_where(r);
        if (where != r->cursor_at) r->cursor_at = where, r->dirty = 1;
        if (r->dirty && r->screen && r->have_guest) replay_present_now(r);
        if (now_ns() - last_report > 10000000000ull) {
            uint64_t st[3], gs[4];
            shaders_stats(r->shaders, st);
            pthread_mutex_lock(&r->lock);
            gpu_stats(r->gpu, gs);
            pthread_mutex_unlock(&r->lock);
            rlog("%llu commands, %llu execs, %llu passes (%llu on the GPU), %llu clears, %llu blits, %llu draws: %llu drawn, %llu skipped; "
                 "%llu pipelines, %llu swaps, %llu frames, %llu not done; shaders %llu translated, %llu refused, %llu waiting",
                 (unsigned long long)r->commands, (unsigned long long)r->execs, (unsigned long long)r->passes, (unsigned long long)gs[0],
                 (unsigned long long)r->clears, (unsigned long long)r->blits, (unsigned long long)r->draws, (unsigned long long)gs[1],
                 (unsigned long long)gs[2], (unsigned long long)gs[3], (unsigned long long)r->swaps, (unsigned long long)r->frames,
                 (unsigned long long)r->unknown, (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2]);
            last_report = now_ns();
        }
    }
    if (r->screen) screen_destroy(r->screen), r->screen = NULL;
    return NULL;
}

/* -------------------------------------------------------------------- the ring -- */
static int serve(Replay *r) {
    HvLinkHeader *h = r->g.link;
    uint8_t *ring = hv_link_ring(h);
    h->app_pid = (uint32_t)getpid();
    uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
    uint32_t *cmd = malloc(HV_LINK_RING_BYTES / 2);
    if (!cmd) return -1;
    unsigned idle = 0;
    while (!r->stop) {
        __atomic_store_n(&h->heartbeat, now_ns(), __ATOMIC_RELEASE);
        uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
        if (head < tail || __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != HV_LINK_MAGIC) break; /* the engine started over */
        if (head == tail) {
            usleep(idle++ < 1000 ? 50 : 1000);
            continue;
        }
        idle = 0;
        uint32_t at = (uint32_t)(tail % HV_LINK_RING_BYTES), room = HV_LINK_RING_BYTES - at;
        if (room < sizeof(HvLinkRecord)) { tail += room; __atomic_store_n(&h->tail, tail, __ATOMIC_RELEASE); continue; }
        HvLinkRecord rec;
        memcpy(&rec, ring + at, sizeof rec);
        if (rec.bytes < sizeof rec || rec.bytes > room) break; /* not a record: the ring is not ours to read */
        if (rec.kind == HV_REC_COMMAND && rec.length <= rec.bytes - sizeof rec) {
            memcpy(cmd, ring + at + sizeof rec, rec.length);
            replay_command(r, rec.channel, cmd, rec.length);
            __atomic_store_n(&h->done, rec.seq, __ATOMIC_RELEASE);
        }
        tail += rec.bytes;
        __atomic_store_n(&h->tail, tail, __ATOMIC_RELEASE);
    }
    free(cmd);
    return 0;
}

static void reset_objects(Replay *r) {
    for (uint32_t t = 0; t < HV_LINK_TASKS; t++) obj_forget_task(&r->os, t);
    for (Surface *s = r->os.surfaces; s;) {
        Surface *n = s->next;
        guest_unmap(s->mem, s->length / GPAGE);
        free(s->pages);
        free(s);
        s = n;
    }
    r->os.surfaces = NULL;
    r->mapping = 0;
}

static void *ring_loop(void *arg) {
    Replay *r = arg;
    while (!r->stop) {
        if (guest_open(&r->g, r->link_path) != 0) { usleep(500000); continue; }
        rlog("linked to the engine: guest RAM %llu MB", (unsigned long long)(r->g.ram_size >> 20));
        pthread_mutex_lock(&r->lock);
        r->have_guest = 1;
        pthread_mutex_unlock(&r->lock);
        serve(r);
        pthread_mutex_lock(&r->lock);
        r->have_guest = 0;
        reset_objects(r);
        pthread_mutex_unlock(&r->lock);
        guest_close(&r->g);
        if (!r->stop) rlog("engine gone; waiting for it");
    }
    return NULL;
}

/* ------------------------------------------------------------------- lifetime -- */
static Replay *replay_new(void) {
    Replay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->wake, NULL);
    pthread_cond_init(&r->applied, NULL);
    r->os.g = &r->g, r->os.on_function = on_function, r->os.ctx = r;
    r->g.ram_fd = -1;
    return r;
}

Replay *replay_start(const char *link_path, void *window, const char *tool, const char *cache) {
    Replay *r = replay_new();
    if (!r) return NULL;
    snprintf(r->link_path, sizeof r->link_path, "%s", link_path);
    r->shaders = shaders_create(tool, cache);
    r->gpu = r->shaders ? gpu_create(&r->os, r->shaders) : NULL;
    r->new_window = window, r->window_changed = 1;
    if (pthread_create(&r->presenter, NULL, present_loop, r) || pthread_create(&r->ring, NULL, ring_loop, r)) {
        rlog("no threads");
        return r;
    }
    r->running = 1;
    return r;
}

void replay_set_window(Replay *r, void *window) {
    pthread_mutex_lock(&r->lock);
#ifdef __ANDROID__
    if (r->window_changed && r->new_window) ANativeWindow_release(r->new_window); /* never taken */
#endif
    r->new_window = window, r->window_changed = 1;
    pthread_cond_signal(&r->wake);
    /* Android wants a surface's last use over before surfaceDestroyed returns: wait for the presenter
       to let go of the old one (a second at most). */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    while (r->window_changed && r->running && !r->stop)
        if (pthread_cond_timedwait(&r->applied, &r->lock, &until)) break;
    pthread_mutex_unlock(&r->lock);
}

void replay_stop(Replay *r) {
    if (!r) return;
    r->stop = 1;
    pthread_cond_signal(&r->wake);
    if (r->running) pthread_join(r->ring, NULL), pthread_join(r->presenter, NULL);
#ifdef __ANDROID__
    if (r->window) ANativeWindow_release(r->window);
    if (r->window_changed && r->new_window) ANativeWindow_release(r->new_window);
#endif
    gpu_destroy(r->gpu);
    shaders_destroy(r->shaders);
    if (r->have_guest || r->g.link) { reset_objects(r); if (!r->own_guest) guest_close(&r->g); }
    free(r->frame);
    free(r->cursor);
    free(r);
}

void replay_enable_draws(Replay *r, const char *tool, const char *cache) {
    r->shaders = shaders_create(tool, cache);
    r->gpu = r->shaders ? gpu_create(&r->os, r->shaders) : NULL;
}

Replay *replay_for_test(Guest *g, Screen *screen) {
    Replay *r = replay_new();
    if (!r) return NULL;
    r->g = *g, r->own_guest = 1, r->have_guest = 1;
    r->screen = screen;
    return r;
}
