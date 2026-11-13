/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exits chained to another page, and their undoing when that page's code is written (hv_invalidate_code): the
   list of them is threaded by the target's page (HvMachine.xhead, xlink), so the write walks the exits into that
   page alone. This checks the threading against the plain rule it replaced (every chained exit whose target is
   the page goes back to its stub, no other does), over random chains, writes and flushes, pages that share a
   bucket included. The entries are made as chain() in core/dispatch.c makes them. */
#include "machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void hv_flush_blocks(HvMachine *m);

#define RAM_PAGES (1u << 17) /* 2 GB of address space, untouched: twice the buckets, so pages must share them (the hash
                                spreads neighbouring pages evenly: 8192 in a row share none) */
#define PAGES 96       /* the pages the test chains to and writes, picked from them */
#define PAGE (1ull << HV_TLB_PAGE_BITS)
#define BASE 0x80000000ull

static HvMachine m;
static uint8_t model[1 << 16]; /* per slot: 0 not chained, else the number of its target in pool + 1 */
static uint32_t pool[PAGES];   /* the pages used: pairs that share a bucket first */

/* As chain() does for an exit to another page. Returns 0 where the list is full. */
static int chain_to(uint32_t idx, uint32_t which) {
    uint64_t page_pa = BASE + pool[which] * PAGE;
    if (m.nxchained >= m.max_slots) return 0;
    m.slot_xpa[idx] = page_pa | 1;
    uint32_t k = m.nxchained++, h = hv_xhash(page_pa);
    m.xchained[k] = idx;
    m.xlink[k] = m.xhead[h], m.xhead[h] = k + 1;
    m.slots[idx] = 0x1000 + idx; /* "the target's code" */
    return 1;
}

static int check(const char *when, uint32_t slots) {
    for (uint32_t i = 0; i < slots; i++) {
        int chained = m.slots[i] != 0 && m.slot_xpa[i] != 0;
        if (model[i] && (!chained || m.slot_xpa[i] != ((BASE + (uint64_t)pool[model[i] - 1] * PAGE) | 1))) {
            printf("FAIL %s: slot %u should be chained to page %u; slot %llx, page %llx\n", when, i, model[i] - 1,
                   (unsigned long long)m.slots[i], (unsigned long long)m.slot_xpa[i]);
            return 1;
        }
        if (!model[i] && (m.slots[i] || m.slot_xpa[i])) {
            printf("FAIL %s: slot %u should be unchained; slot %llx, page %llx\n", when, i, (unsigned long long)m.slots[i],
                   (unsigned long long)m.slot_xpa[i]);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 1 << 20) != 0) { printf("machine init failed\n"); return 1; }
    uint8_t *ram = mmap(NULL, RAM_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (ram == MAP_FAILED || hv_machine_add_ram(&m, BASE, RAM_PAGES * PAGE, ram, 0) != 0) { printf("no RAM\n"); return 1; }
    uint32_t slots = m.max_slots < sizeof model ? m.max_slots : (uint32_t)sizeof model;
    /* The pages used: first pairs that share a bucket (their exits lie on one chain), then others. */
    uint32_t npool = 0, pairs = 0;
    static uint32_t first_in[1 << HV_XHASH_BITS]; /* page + 1 of the first page seen in each bucket */
    for (uint32_t pg = 0; pg < RAM_PAGES && npool + 2 <= PAGES / 2; pg++) {
        uint32_t h = hv_xhash(BASE + pg * PAGE);
        if (first_in[h]) pool[npool++] = first_in[h] - 1, pool[npool++] = pg, pairs++, first_in[h] = 0;
        else first_in[h] = pg + 1;
    }
    for (uint32_t pg = 3; npool < PAGES; pg += 61) { /* any others, each once */
        int have = 0;
        for (uint32_t i = 0; i < npool; i++) have |= pool[i] == pg;
        if (!have) pool[npool++] = pg;
    }
    uint32_t twin_a = 0, twin_b = pairs ? 1 : 0; /* pool[0] and pool[1] share a bucket where a pair was found */
    srand(11);
    int fail = 0;
    unsigned long chains = 0, writes = 0, undone = 0, flushes = 0, refused = 0;
    for (int step = 0; step < 200000 && !fail; step++) {
        int r = rand() % 1000;
        if (r < 700) { /* chain a free slot to some page */
            uint32_t idx = (uint32_t)rand() % slots, page = (uint32_t)rand() % PAGES;
            if (model[idx]) continue;
            if (chain_to(idx, page)) model[idx] = (uint8_t)(page + 1), chains++;
            else refused++;
        } else if (r < 995) { /* the Mac writes over a page that holds translated code */
            uint32_t page = (uint32_t)rand() % PAGES;
            m.ram[0].code_pages[pool[page]] = 1;
            hv_invalidate_code(&m, BASE + pool[page] * PAGE + (uint64_t)(rand() % (int)PAGE));
            writes++;
            for (uint32_t i = 0; i < slots; i++)
                if (model[i] == page + 1) model[i] = 0, undone++;
            fail = check("after a write", slots);
        } else { /* every translation dropped: the slots start over, as the translator hands them out again */
            hv_flush_blocks(&m);
            memset(model, 0, sizeof model);
            for (uint32_t i = 0; i < slots; i++) m.slots[i] = 0, m.slot_xpa[i] = 0;
            flushes++;
            if (m.nxchained) { printf("FAIL the list is not empty after a flush\n"); fail = 1; }
        }
    }
    if (!fail) fail = check("at the end", slots);
    /* The pair in one bucket: a write to one leaves the other's exits chained. */
    if (!fail && twin_b) {
        hv_flush_blocks(&m);
        memset(model, 0, sizeof model);
        for (uint32_t i = 0; i < slots; i++) m.slots[i] = 0, m.slot_xpa[i] = 0;
        for (uint32_t i = 0; i < 64; i++) chain_to(i, i & 1 ? twin_a : twin_b), model[i] = (uint8_t)((i & 1 ? twin_a : twin_b) + 1);
        m.ram[0].code_pages[pool[twin_a]] = 1;
        hv_invalidate_code(&m, BASE + pool[twin_a] * PAGE);
        for (uint32_t i = 0; i < 64; i++)
            if (model[i] == twin_a + 1) model[i] = 0;
        fail = check("two pages in one bucket", slots);
    }
    printf("%lu chains, %lu code pages written (%lu exits back to their stubs), %lu flushes, %lu refused by a full list, %u pairs of pages "
           "in one bucket: %s\n", chains, writes, undone, flushes, refused, pairs, fail ? "FAILED" : "0 failures");
    return fail;
}
