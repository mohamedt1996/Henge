/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest memory, from the app's process: the RAM file the engine keeps while linked, and the GPU
   tasks' page tables (as devices/pvg_lite.c walks them). */
#include "replay.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int guest_open(Guest *g, const char *link_path) {
    memset(g, 0, sizeof *g);
    g->ram_fd = -1;
    int fd = open(link_path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < HV_LINK_FILE_BYTES) { close(fd); return -1; }
    HvLinkHeader *h = mmap(NULL, HV_LINK_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (h == MAP_FAILED) return -1;
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != HV_LINK_MAGIC || h->version != HV_LINK_VERSION) {
        munmap(h, HV_LINK_FILE_BYTES);
        return -1;
    }
    g->link = h;
    g->ram_pa = h->ram_pa, g->ram_size = h->ram_size;
    g->ram_fd = open(h->ram_file, O_RDWR);
    if (g->ram_fd < 0) {
        rlog("guest RAM %s: %s", h->ram_file, strerror(errno));
        guest_close(g);
        return -1;
    }
    g->ram = mmap(NULL, g->ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, g->ram_fd, 0);
    if (g->ram == MAP_FAILED) {
        g->ram = NULL;
        guest_close(g);
        return -1;
    }
    /* Both processes hold the file now: the name can go, so it never outlives them. */
    unlink(h->ram_file);
    return 0;
}

void guest_close(Guest *g) {
    if (g->ram) munmap(g->ram, g->ram_size);
    if (g->ram_fd >= 0) close(g->ram_fd);
    if (g->link) munmap(g->link, HV_LINK_FILE_BYTES);
    memset(g, 0, sizeof *g);
    g->ram_fd = -1;
}

void *guest_phys(Guest *g, uint64_t pa, uint64_t len) {
    if (pa < g->ram_pa || pa - g->ram_pa + len > g->ram_size) return NULL;
    return g->ram + (pa - g->ram_pa);
}

static uint32_t *table_page(Guest *g, uint32_t page) {
    return page ? guest_phys(g, (uint64_t)page << 14, GPAGE) : NULL;
}

/* The root page holds {table page, depth}; each level has 4096 32-bit page numbers (bit 31 a flag). */
uint32_t guest_task_page(Guest *g, uint32_t task, uint64_t off) {
    if (task >= HV_LINK_TASKS) return 0;
    uint32_t *root = table_page(g, g->link->task_root[task]);
    if (!root) return 0;
    uint32_t page = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return 0;
    uint64_t vpn = off >> 14;
    for (int level = (int)depth - 1; level >= 0; level--) {
        uint32_t *t = table_page(g, page);
        if (!t) return 0;
        page = t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff;
    }
    return page;
}

void guest_task_pages(Guest *g, uint32_t task, uint64_t first, uint64_t n, uint32_t *out) {
    memset(out, 0, n * 4);
    if (task >= HV_LINK_TASKS) return;
    uint32_t *root = table_page(g, g->link->task_root[task]);
    if (!root) return;
    uint32_t top = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return;
    for (uint64_t i = 0; i < n;) {
        uint64_t vpn = first + i;
        uint32_t page = top;
        for (int level = (int)depth - 1; level >= 1 && page; level--) {
            uint32_t *t = table_page(g, page);
            page = t ? t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff : 0;
        }
        uint32_t *leaf = table_page(g, page);
        uint64_t in = vpn & 0xfff, run = 4096 - in;
        if (run > n - i) run = n - i;
        if (leaf)
            for (uint64_t k = 0; k < run; k++) out[i + k] = leaf[in + k] & 0x7fffffff;
        i += run;
    }
}

int guest_task_read(Guest *g, uint32_t task, uint64_t off, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t page = guest_task_page(g, task, off);
        uint8_t *q = page ? guest_phys(g, ((uint64_t)page << 14) + (off & 0x3fff), GPAGE - (off & 0x3fff)) : NULL;
        if (!q) return -1;
        uint32_t avail = (uint32_t)(GPAGE - (off & 0x3fff)), c = n < avail ? n : avail;
        memcpy(d, q, c);
        d += c, off += c, n -= c;
    }
    return 0;
}

/* Each guest page is 16 KB of the RAM file, so it can be mapped straight from the file: a buffer or
   surface scattered over guest memory reads as one range, and writes land in the guest's pages. */
uint8_t *guest_map_pages(Guest *g, const uint32_t *pages, uint64_t n) {
    if (!n) return NULL;
    uint8_t *base = mmap(NULL, n * GPAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t pa = (uint64_t)pages[i] << 14;
        if (!pages[i] || pa < g->ram_pa || pa - g->ram_pa + GPAGE > g->ram_size) continue;
        if (mmap(base + i * GPAGE, GPAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g->ram_fd, (off_t)(pa - g->ram_pa)) == MAP_FAILED) {
            munmap(base, n * GPAGE);
            return NULL;
        }
    }
    return base;
}

void guest_unmap(uint8_t *p, uint64_t n) {
    if (p) munmap(p, n * GPAGE);
}

const uint8_t *guest_surface_desc(Guest *g, uint32_t mapping) {
    uint64_t table = g->link->surface_table;
    uint32_t *e = table ? guest_phys(g, table + (mapping / 16) * 4ull, 4) : NULL;
    if (!e || !(*e & 1)) return NULL;
    return guest_phys(g, ((uint64_t)(*e & ~1u) << 12) + (mapping % 16) * 0x400ull, 0x400);
}
