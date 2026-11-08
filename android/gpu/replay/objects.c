/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The guest's GPU objects, as the CPU sees them: buffers and textures over their guest pages, and
   surfaces (IOSurfaces) over the pages the surface mapper lists. Entry layouts follow
   devices/pvg_metal.m, which reads the same objects for Metal. */
#include "replay.h"

#include <stdlib.h>
#include <string.h>

static unsigned slot_of(uint32_t task, uint32_t ref) { return (ref * 2654435761u ^ task * 40503u) % 4096u; }

/* ------------------------------------------------------------------ descriptions -- */
static void desc_v1(TexView *t, const uint8_t *d) {
    t->type = d[0] & 0xf;
    t->format = (uint16_t)(d[2] | d[3] << 8);
    memcpy(&t->w, d + 4, 4), memcpy(&t->h, d + 8, 4), memcpy(&t->depth, d + 12, 4);
    uint16_t mips, samples, array;
    memcpy(&mips, d + 16, 2), memcpy(&samples, d + 18, 2), memcpy(&array, d + 20, 2);
    t->mips = mips, t->samples = samples, t->slices = array;
}

static void desc_v2(TexView *t, const uint8_t *d) {
    t->type = d[0] & 0xf;
    t->format = (uint16_t)(d[1] | d[2] << 8);
    memcpy(&t->w, d + 7, 4), memcpy(&t->h, d + 11, 4), memcpy(&t->depth, d + 15, 4);
    uint16_t mips, samples, array;
    memcpy(&mips, d + 19, 2), memcpy(&samples, d + 21, 2), memcpy(&array, d + 23, 2);
    t->mips = mips, t->samples = samples, t->slices = array;
}

static void desc_fix(TexView *t) {
    if (!t->depth) t->depth = 1;
    if (!t->mips) t->mips = 1;
    if (t->mips > 16) t->mips = 16;
    if (!t->samples) t->samples = 1;
    if (!t->slices) t->slices = 1;
    if (t->type == 5 || t->type == 6) t->slices *= 6; /* cube, cube array: six faces a slice */
}

/* Linear storage for a description: level i after level i - 1, rows tightly packed. */
static uint64_t layout_linear(TexView *t, uint64_t row0) {
    unsigned bpp = px_bytes(t->format);
    uint64_t at = 0;
    for (uint32_t m = 0; m < t->mips; m++) {
        uint32_t w = t->w >> m ? t->w >> m : 1, h = t->h >> m ? t->h >> m : 1, d = t->depth >> m ? t->depth >> m : 1;
        uint64_t row = m == 0 && row0 ? row0 : (uint64_t)w * bpp;
        t->level_off[m] = at, t->level_row[m] = row, t->level_img[m] = row * h;
        at += row * h * d;
    }
    t->slice_stride = at;
    return at * t->slices;
}

/* -------------------------------------------------------------------- memory -- */
static void map_task(Objects *os, Obj *o) {
    uint64_t n = (o->length + GPAGE - 1) / GPAGE;
    if (!n || n > (1u << 20)) return;
    uint32_t *pages = calloc(n, 4);
    if (!pages) return;
    guest_task_pages(os->g, o->task, o->vpage, n, pages);
    uint8_t *mem = guest_map_pages(os->g, pages, n);
    if (!mem) { free(pages); return; }
    guest_unmap(o->mem_owned ? o->mem : NULL, n);
    free(o->pages);
    o->pages = pages, o->mem = mem, o->mem_owned = 1;
}

void obj_refresh(Objects *os, Obj *o) {
    if (!o || !o->pages || !o->mem_owned) return;
    uint64_t n = (o->length + GPAGE - 1) / GPAGE;
    uint32_t stack[256], *now = n <= 256 ? stack : malloc(n * 4);
    if (!now) return;
    guest_task_pages(os->g, o->task, o->vpage, n, now);
    int same = !memcmp(now, o->pages, n * 4);
    if (now != stack) free(now);
    if (!same) map_task(os, o);
}

Surface *surface_get(Objects *os, uint32_t mapping) {
    for (Surface *s = os->surfaces; s; s = s->next)
        if (s->mapping == mapping) return s;
    const uint8_t *d = guest_surface_desc(os->g, mapping);
    if (!d) return NULL;
    uint32_t list, first;
    uint64_t alloc, row, w, h;
    memcpy(&list, d, 4), memcpy(&alloc, d + 0x18, 8), memcpy(&row, d + 0x30, 8), memcpy(&first, d + 0x60, 4);
    memcpy(&w, d + 0x20, 8), memcpy(&h, d + 0x28, 8);
    if (!(list & 1) || !alloc || alloc > (1ull << 31)) return NULL;
    Surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->mapping = mapping, s->w = (uint32_t)w, s->h = (uint32_t)h, s->row = row, s->first = first, s->alloc = alloc;
    memcpy(&s->fourcc, d + 4, 4);
    s->length = (alloc + first + GPAGE - 1) & ~(GPAGE - 1);
    uint64_t n = s->length / GPAGE;
    /* The descriptor lists physical addresses >> 12, bit 0 present, one per 16 KB. */
    const uint32_t *pl = guest_phys(os->g, (uint64_t)(list & ~1u) << 12, n * 4);
    s->pages = calloc(n, 4);
    if (pl && s->pages)
        for (uint64_t i = 0; i < n; i++) s->pages[i] = (pl[i] & 1) ? (uint32_t)(((uint64_t)(pl[i] & ~1u) << 12) >> 14) : 0;
    s->mem = s->pages ? guest_map_pages(os->g, s->pages, n) : NULL;
    if (!s->mem) { free(s->pages); free(s); return NULL; }
    s->next = os->surfaces, os->surfaces = s;
    rlog("surface %u: %ux%u fourcc %c%c%c%c row %llu", mapping, s->w, s->h, d[7], d[6], d[5], d[4], (unsigned long long)row);
    return s;
}

/* ------------------------------------------------------------------- objects -- */
static void unref(Obj *o) {
    if (!o || --o->refs) return;
    if (o->mem_owned) guest_unmap(o->mem, (o->length + GPAGE - 1) / GPAGE);
    free(o->host);
    free(o->pages);
    unref(o->parent);
    free(o);
}

static Obj *lookup(Objects *os, uint32_t task, uint32_t ref) {
    for (Obj *o = os->table[slot_of(task, ref)]; o; o = o->next)
        if (o->task == task && o->ref == ref) return o;
    return NULL;
}

void obj_forget(Objects *os, uint32_t task, uint32_t ref) {
    Obj **at = &os->table[slot_of(task, ref)];
    for (; *at; at = &(*at)->next)
        if ((*at)->task == task && (*at)->ref == ref) {
            Obj *o = *at;
            *at = o->next;
            unref(o);
            return;
        }
}

void obj_forget_task(Objects *os, uint32_t task) {
    for (unsigned i = 0; i < 4096; i++)
        for (Obj **at = &os->table[i]; *at;) {
            if ((*at)->task == task) { Obj *o = *at; *at = o->next; unref(o); }
            else at = &(*at)->next;
        }
}

static void set_parent(Obj *o, Obj *p) {
    if (p) p->refs++;
    o->parent = p;
}

/* A texture with no place in guest memory: kept in the app, so clears and copies into it still work. */
static void host_texture(Obj *o) {
    uint64_t bytes = layout_linear(&o->tex, 0);
    if (!px_bytes(o->tex.format) || !bytes || bytes > (256ull << 20)) return;
    o->host = calloc(1, bytes);
    if (!o->host) return;
    o->tex.mem = &o->host, o->tex.limit = bytes;
    o->has_tex = 1;
}

static void make_texture(Objects *os, Obj *o, uint64_t at, uint32_t size) {
    uint8_t *all = size >= 12 + 0x18 && size < (1u << 20) ? malloc(size) : NULL;
    if (!all || guest_task_read(os->g, o->task, at, all, size)) { free(all); return; }
    /* {length, page}, a layout table, {tag, size, ref}, the description */
    memcpy(&o->length, all, 8), memcpy(&o->vpage, all + 8, 4);
    const uint8_t *layout = all + 12;
    uint16_t mips, slices;
    uint64_t base, stride;
    memcpy(&mips, layout, 2), memcpy(&slices, layout + 2, 2), memcpy(&base, layout + 4, 8), memcpy(&stride, layout + 12, 8);
    mips &= 0x3fff;
    uint32_t table = mips * 36u;
    if (!mips || mips > 16 || 12 + 0x18 + table + 12 + 24 > size) { free(all); return; }
    const uint8_t *rec = layout + 0x18 + table;
    uint32_t tag;
    memcpy(&tag, rec, 4);
    if (tag == 0x34 || tag == 0x39) desc_v2(&o->tex, rec + 12); else desc_v1(&o->tex, rec + 12);
    desc_fix(&o->tex);
    o->tex.mips = mips;
    for (uint32_t m = 0; m < mips; m++) {
        const uint8_t *e = layout + 0x18 + m * 36;
        uint64_t off, row;
        uint32_t h, d;
        memcpy(&off, e, 8), memcpy(&row, e + 0x10, 8), memcpy(&h, e + 0x1c, 4), memcpy(&d, e + 0x20, 4);
        o->tex.level_off[m] = base + off, o->tex.level_row[m] = row, o->tex.level_img[m] = row * (h ? h : 1);
    }
    o->tex.slice_stride = stride;
    free(all);
    map_task(os, o);
    if (o->mem && px_bytes(o->tex.format)) o->tex.mem = &o->mem, o->tex.limit = o->length, o->has_tex = 1;
}

static void make_view(Obj *o, Obj *parent, uint16_t format, uint16_t type, uint64_t level0, uint64_t levels,
                      uint64_t slice0, uint64_t nslices, int whole) {
    if (!parent || !parent->has_tex) return;
    set_parent(o, parent);
    TexView *p = &parent->tex, *t = &o->tex;
    *t = *p;
    t->format = format;
    if (px_bytes(format) != px_bytes(p->format)) { o->has_tex = 0; return; }
    if (whole) { o->has_tex = 1; return; }
    t->type = type;
    if (level0 >= p->mips) return;
    if (!levels || level0 + levels > p->mips) levels = p->mips - level0;
    for (uint64_t m = 0; m < levels; m++) {
        t->level_off[m] = p->level_off[level0 + m] + slice0 * p->slice_stride;
        t->level_row[m] = p->level_row[level0 + m], t->level_img[m] = p->level_img[level0 + m];
    }
    t->mips = (uint32_t)levels;
    t->w = p->w >> level0 ? p->w >> level0 : 1, t->h = p->h >> level0 ? p->h >> level0 : 1;
    t->slices = nslices && slice0 + nslices <= p->slices ? (uint32_t)nslices : p->slices - (uint32_t)slice0;
    o->has_tex = 1;
}

static void make_linear(Objects *os, Obj *o, uint64_t at, uint32_t size) {
    struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    if (guest_task_read(os->g, o->task, at, &h, size < sizeof h ? size : (uint32_t)sizeof h)) return;
    if (h.tag == 0x34 || h.tag == 0x14 || h.tag == 1) { /* private: {tag, size, ref, description} */
        if (h.tag == 0x34) desc_v2(&o->tex, (const uint8_t *)&h.buffer); else desc_v1(&o->tex, (const uint8_t *)&h.buffer);
        desc_fix(&o->tex);
        host_texture(o);
        return;
    }
    if (h.tag == 0x15 || h.tag == 0x38) { /* in a heap, at an offset of a layout only Apple's GPU knows */
        const uint8_t *desc = (const uint8_t *)&h.offset;
        if (h.tag == 0x38) desc_v2(&o->tex, desc); else desc_v1(&o->tex, desc);
        desc_fix(&o->tex);
        set_parent(o, obj_get(os, o->task, h.buffer));
        host_texture(o);
        return;
    }
    if (h.tag == 7 || h.tag == 8 || h.tag == 0x1b) { /* a view */
        struct __attribute__((packed)) { uint16_t format, type; uint64_t levels[2], slices[2]; uint8_t swizzle[4]; } v;
        memcpy(&v, &h.offset, sizeof v);
        Obj *parent = obj_get(os, o->task, h.buffer);
        make_view(o, parent, v.format, v.type, v.levels[0], v.levels[1], v.slices[0], v.slices[1], h.tag == 7);
        if (parent) o->surface = parent->surface;
        return;
    }
    if (h.tag == 9 || h.tag == 0x37) { /* over a buffer: {buffer ref, offset, bytes per row, description} */
        Obj *b = obj_get(os, o->task, h.buffer);
        if (!b || !b->mem) return;
        if (h.tag == 0x37) desc_v2(&o->tex, h.d); else desc_v1(&o->tex, h.d);
        desc_fix(&o->tex);
        set_parent(o, b);
        layout_linear(&o->tex, h.row);
        for (uint32_t m = 0; m < o->tex.mips; m++) o->tex.level_off[m] += h.offset;
        o->tex.mem = b->buffer_base ? &b->buffer_base : &b->mem, o->tex.limit = b->length;
        o->has_tex = px_bytes(o->tex.format) != 0;
        return;
    }
}

static void make_mapped(Objects *os, Obj *o, uint64_t at) {
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    if (guest_task_read(os->g, o->task, at, &h, 20 + 32)) return;
    if (h.size > 32) guest_task_read(os->g, o->task, at, &h, 20 + (h.size < 64 ? h.size : 64));
    Surface *s = surface_get(os, (uint32_t)h.mapping);
    if (!s) return;
    if (h.tag == 0x39) desc_v2(&o->tex, h.d); else desc_v1(&o->tex, h.d);
    desc_fix(&o->tex);
    o->surface = s;
    o->tex.mips = 1;
    o->tex.level_off[0] = s->first, o->tex.level_row[0] = s->row, o->tex.level_img[0] = s->row * s->h;
    o->tex.slice_stride = 0, o->tex.slices = 1;
    o->tex.mem = &s->mem, o->tex.limit = s->length;
    o->has_tex = px_bytes(o->tex.format) != 0;
}

Obj *obj_get(Objects *os, uint32_t task, uint32_t ref) {
    if (!ref || task >= HV_LINK_TASKS) return NULL;
    uint64_t list = os->g->link->task_objects[task];
    uint32_t e[3];
    if (!list || ref >= 1u << 20 || guest_task_read(os->g, task, list + (uint64_t)ref * 12, e, 12)) return NULL;
    Obj *o = lookup(os, task, ref);
    if (o && !memcmp(o->entry, e, 12)) return o;
    if (o) obj_forget(os, task, ref); /* the guest reused the ref */
    o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->task = task, o->ref = ref, o->refs = 1, o->unique_id = ++os->next_id;
    memcpy(o->entry, e, 12);
    o->type = e[0] & 0xff;
    uint32_t size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    unsigned slot = slot_of(task, ref);
    o->next = os->table[slot], os->table[slot] = o;
    switch (o->type) {
    case OBJ_BUFFER: case OBJ_BUFFER27: { /* {length, page, flags} */
        struct __attribute__((packed)) { uint64_t length; uint32_t vpage, flags; } b;
        if (guest_task_read(os->g, task, at, &b, sizeof b)) break;
        o->length = b.length, o->vpage = b.vpage;
        map_task(os, o);
        break;
    }
    case OBJ_HEAP: { /* {page, length, flags}: private memory, with its copy in guest memory */
        struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } hp;
        if (guest_task_read(os->g, task, at, &hp, sizeof hp)) break;
        o->length = hp.length, o->vpage = hp.vpage;
        map_task(os, o);
        break;
    }
    case OBJ_HEAP_BUFFER: { /* {heap, offset, length} */
        struct __attribute__((packed)) { uint32_t heap; uint64_t offset, length; } hb;
        if (guest_task_read(os->g, task, at, &hb, sizeof hb)) break;
        Obj *heap = obj_get(os, task, hb.heap);
        set_parent(o, heap);
        if (heap && heap->mem && hb.offset + hb.length <= heap->length)
            o->buffer_base = heap->mem + hb.offset, o->length = hb.length;
        break;
    }
    case OBJ_TEXTURE: make_texture(os, o, at, size); break;
    case OBJ_LINEAR_TEXTURE: case OBJ_LINEAR_TEXTURE2: make_linear(os, o, at, size); break;
    case OBJ_MAPPED_TEXTURE: make_mapped(os, o, at); break;
    case OBJ_FUNCTION: { /* {offset, length, tag, size} of a .metallib */
        struct __attribute__((packed)) { uint64_t off; uint32_t len, tag, size; } f;
        if (guest_task_read(os->g, task, at, &f, sizeof f) || !f.len || f.len > (64u << 20) || !os->on_function) break;
        uint8_t *lib = malloc(f.len);
        if (lib && guest_task_read(os->g, task, f.off, lib, f.len) == 0) os->on_function(os->ctx, task, lib, f.len);
        free(lib);
        break;
    }
    default: break;
    }
    return o;
}

uint8_t *obj_bytes(Obj *o, uint64_t *len) {
    if (!o) return NULL;
    if (o->buffer_base) { *len = o->length; return o->buffer_base; }
    if (o->mem) { *len = o->length; return o->mem; }
    return NULL;
}
