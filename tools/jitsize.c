/* SPDX-License-Identifier: GPL-2.0-or-later */
/* How big the translator's output is: translates a file of arm64 code (a library's .text, say) block after block
   and says the words per instruction, by part. For measuring a change to the translator's code size without a
   guest: build/android/jitsize code.bin [superblocks 0|1]. */
#include "machine.h"
#include "translate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define BASE 0x100000000ull

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f) & ~0x3fffl;
    fseek(f, 0, SEEK_SET);
    uint8_t *ram = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED || fread(ram, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    static HvMachine m;
    if (hv_machine_init(&m, 256 << 20) != 0) return 1;
    hv_machine_add_ram(&m, BASE, (uint64_t)size, ram, 1);
    m.chain = 1;
    HvCpu *cpu = calloc(1, sizeof *cpu);
    cpu->m = &m;
    hv_self = cpu;
    uint64_t insns = 0, blocks = 0;
    for (uint64_t off = 0; off + 0x4000 <= (uint64_t)size;) {
        HvBlock b;
        memset(&b, 0, sizeof b);
        b.va = b.pa = BASE + off;
        uint32_t avail = (uint32_t)((0x4000 - (off & 0x3fff)) / 4);
        if (hv_translate_block(&m, &b, (const uint32_t *)(ram + off), avail) != 0 || !b.ninsns) {
            if (m.jit.used + 65536 > m.jit.capacity) { hv_jit_reset(&m.jit); m.nslots = 0; continue; }
            off += 4;
            continue;
        }
        insns += b.ninsns, blocks++;
        off += 4ull * b.ninsns;
    }
    HvStats *s = &m.stats;
    double w = (double)(s->words_inline + s->words_ool + s->words_pool);
    printf("%llu blocks, %.1f insns each; %.2f words per insn (inline %.2f, out of line %.2f, literals %.2f); %.0f bytes a block\n",
           (unsigned long long)blocks, (double)insns / blocks, w / insns, s->words_inline / (double)insns, s->words_ool / (double)insns,
           s->words_pool / (double)insns, w * 4 / blocks);
    return 0;
}
