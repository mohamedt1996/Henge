/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The JIT cache in parts (0.3.62, core/dispatch.c evict_part): a part dropped alone. Blocks and chains are laid out
   by hand, as the translator and chain() leave them: an exit of a kept part chained into the part that goes must go
   back to its stub (slot emptied, its B and the hot part's conditional branch as the translator left them), one
   chained within the kept parts must stay, and the block hash must hold the kept blocks alone, stale ones left out. */
#include "machine.h"
#include "translate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void hv_flush_blocks(HvMachine *m);

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

#define BASE 0x80000000ull
#define PAGE (1ull << HV_TLB_PAGE_BITS)

static HvMachine m;

static HvBlock *block_at(uint32_t index, uint64_t va, uint32_t *code) {
    HvBlock *b = &m.blocks[index];
    memset(b, 0, sizeof *b);
    b->va = va, b->pa = va, b->ramx = 0, b->pg = (uint32_t)((va - BASE) / PAGE), b->code = code;
    b->gen = m.ram[0].page_gen[b->pg];
    uint32_t h = hv_block_hash(b->va, b->pa, b->flags);
    b->next = m.hash[h], m.hash[h] = b;
    return b;
}

static void put_word(uint32_t *at, uint32_t w) { hv_jit_write(&m.jit, at, &w, 4); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_JIT_PARTS", "4", 1), setenv("HV_JIT_PART_MIN", "64", 1);
    if (hv_machine_init(&m, 4 << 20) != 0) { printf("machine init failed\n"); return 1; }
    uint8_t *ram = mmap(NULL, 64 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED || hv_machine_add_ram(&m, BASE, 64 * PAGE, ram, 0) != 0) { printf("no RAM\n"); return 1; }
    CHECK(m.parts == 4 && m.part_cur == 0, "the cache is in %d parts, part %d first", m.parts, m.part_cur);
    uint32_t B = m.max_blocks / 4, L = m.max_slots / 4;
    size_t W = m.part_words;
    uint32_t *p0 = m.jit.base, *p1 = m.jit.base + W, *p2 = m.jit.base + 2 * W;

    /* Parts 1 and 2 were filled before (one block each); part 0 is current again (a wrap) and the next to go is 1. */
    m.part_nb[1] = 1, m.part_used[1] = 64;
    HvBlock *b1 = block_at(B * 1, BASE + 1 * PAGE, p1 + 8);
    m.part_nb[2] = 2, m.part_used[2] = 64;
    HvBlock *b2 = block_at(B * 2, BASE + 2 * PAGE, p2 + 8);
    HvBlock *b2s = block_at(B * 2 + 1, BASE + 3 * PAGE, p2 + 32); /* stale: its page written over since */
    m.ram[0].page_gen[3]++;
    /* Part 0: block A with two chained exits, one into part 1 (goes), one into part 2 (stays). */
    m.nblocks = 1;
    HvBlock *a = block_at(0, BASE + 4 * PAGE, p0 + 0);
    uint32_t *site0 = p0 + 10, *stub0 = p0 + 20, *cond0 = p0 + 2, *site1 = p0 + 12, *stub1 = p0 + 24;
    put_word(site0, 0x14000000u | (uint32_t)((b1->code - site0) & 0x3ffffff));      /* B to part 1's block */
    put_word(cond0, 0x54000000u | (uint32_t)(((b1->code - cond0) & 0x7ffff) << 5));  /* b.eq to it too */
    put_word(site1, 0x14000000u | (uint32_t)((b2->code - site1) & 0x3ffffff));       /* B to part 2's block */
    m.slots[0] = (uint64_t)(uintptr_t)b1->code, m.slot_site[0] = hv_slot_at(&m, site0), m.slot_stub[0] = hv_slot_at(&m, stub0);
    m.slot_cond[0] = hv_slot_at(&m, cond0);
    m.slots[1] = (uint64_t)(uintptr_t)b2->code, m.slot_site[1] = hv_slot_at(&m, site1), m.slot_stub[1] = hv_slot_at(&m, stub1);
    m.slot_xpa[1] = (BASE + 2 * PAGE) | 1; /* chained to another page: on the list */
    m.nslots = 2;
    m.jit.used = 64;

    m.flush_why = 2; /* for room */
    hv_flush_blocks(&m);
    CHECK(m.part_cur == 1 && m.stats.part_drops == 1, "part %d is current after %llu drops (1 and 1 expected)", m.part_cur,
          (unsigned long long)m.stats.part_drops);
    CHECK(m.nblocks == (int)B && m.nslots == L && m.jit.used == W && m.jit.limit == 2 * W, "the current part's ranges: blocks %d slots %u code %zu..%zu",
          m.nblocks, m.nslots, m.jit.used, m.jit.limit);
    CHECK(m.slots[0] == 0 && m.stats.part_unchained == 1, "the exit into the part dropped is still chained (%llx)", (unsigned long long)m.slots[0]);
    CHECK(*site0 == (0x14000000u | (uint32_t)((stub0 - site0) & 0x3ffffff)), "its B does not go to its stub: %08x", *site0);
    int64_t coff = (int32_t)((*cond0 >> 5) & 0x7ffff) << 13 >> 13;
    CHECK((*cond0 & 0xff00001f) == 0x54000000u && cond0 + coff == site0, "its conditional branch goes %lld words on, not to its B", (long long)coff);
    CHECK(m.slots[1] == (uint64_t)(uintptr_t)b2->code && *site1 == (0x14000000u | (uint32_t)((b2->code - site1) & 0x3ffffff)),
          "the exit into a kept part was undone");
    CHECK(m.nxchained == 1 && m.xchained[0] == 1 && m.xhead[hv_xhash(m.slot_xpa[1])] == 1, "the list of exits to other pages: %u", m.nxchained);
    CHECK(hv_block_find(&m, hv_block_hash(a->va, a->pa, 0), a->va, a->pa, 0, a->gen) == a, "block A is not in the hash");
    CHECK(hv_block_find(&m, hv_block_hash(b2->va, b2->pa, 0), b2->va, b2->pa, 0, b2->gen) == b2, "part 2's block is not in the hash");
    CHECK(!hv_block_find(&m, hv_block_hash(b1->va, b1->pa, 0), b1->va, b1->pa, 0, b1->gen), "part 1's block is still in the hash");
    CHECK(!hv_block_find(&m, hv_block_hash(b2s->va, b2s->pa, 0), b2s->va, b2s->pa, 0, b2s->gen), "a stale block is still in the hash");
    CHECK(hv_jit_live_blocks(&m) == 3, "%u blocks held (A, and part 2's two)", hv_jit_live_blocks(&m));

    /* A whole drop (any other reason) empties every part and starts at part 0. */
    hv_flush_blocks(&m);
    CHECK(m.part_cur == 0 && m.nblocks == 0 && hv_jit_live_blocks(&m) == 0 && !hv_block_find(&m, hv_block_hash(a->va, a->pa, 0), a->va, a->pa, 0, a->gen),
          "after a whole drop: part %d, %u blocks", m.part_cur, hv_jit_live_blocks(&m));
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
