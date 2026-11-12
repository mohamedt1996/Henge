/* SPDX-License-Identifier: GPL-2.0-or-later */
/* How large and how slow is a translation? Translates a body of real arm64 code block by block, as the dispatcher
   would on first sight of each block, and prints a block's size in guest instructions and host bytes, the share
   of each kind of instruction, and the time a block took. Sizes are exact; the time under qemu-user is that
   emulator's (about 1 ns a guest instruction on a desktop), so read it as instructions, not as the tablet's time.

   Not built by the Makefile. On Linux, with the engine's objects built (make -f android/Makefile):

     aarch64-linux-gnu-objcopy -O binary -j .text /usr/aarch64-linux-gnu/lib/libc.so.6 /tmp/libc.text
     clang --target=aarch64-linux-gnu -march=armv8.2-a+crypto -D_GNU_SOURCE -O2 -w -Iandroid/compat -Iandroid -Icore \
       -Ihost -Idevices tests/bench_translate.c build/android/{classify,operands,translate,dispatch,mmu,memop,exception,sysreg,machine,jit_region,disk,enter}.o \
       -static -lpthread -lm -o /tmp/bench_translate
     qemu-aarch64 -cpu max /tmp/bench_translate /tmp/libc.text [block flags, 1 = EL1] [repeats]

   glibc 2.39's .text, 2026-11-13: 65690 blocks of 4.4 instructions (18 bytes) each become 206 bytes of host code
   (11.6 times the guest's; 41% inline, 47% out of line, 12% literals); loads and stores are 30% of the
   instructions and 61% of the inline words (9.8 words each), branches 20% and 23% (5.3 each). 3.85 us a block
   under qemu-user 8.2, some 3 to 4 thousand instructions. */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int hv_translate_block(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    size_t bytes = ((size_t)n + 16383) & ~(size_t)16383;
    uint32_t *code = aligned_alloc(16384, bytes);
    memset(code, 0, bytes);
    if (fread(code, 1, (size_t)n, f) != (size_t)n) return 1;
    int flags = argc > 2 ? atoi(argv[2]) : 0, reps = argc > 3 ? atoi(argv[3]) : 1;
    static HvMachine m;
    if (hv_machine_init(&m, 512u << 20) != 0) { printf("init failed\n"); return 1; }
    uint64_t base = 0x180000000ull;
    hv_machine_add_ram(&m, base, bytes, code, 1);
    m.stats_interval = 1; m.chain = 1;
    uint64_t blocks = 0, insns = 0; double t0 = now();
    for (int r = 0; r < reps; r++) {
        uint32_t words = (uint32_t)(n / 4), i = 0;
        while (i < words) {
            HvBlock b = {.va = base + 4ull * i, .pa = base + 4ull * i, .flags = (uint32_t)flags, .ram = &m.ram[0], .pg = (4ull * i) >> 14, .maker = 1};
            uint32_t page_left = (uint32_t)((16384 - ((4ull * i) & 16383)) / 4);
            uint32_t avail = words - i < page_left ? words - i : page_left;
            if (hv_translate_block(&m, &b, code + i, avail) != 0) { m.jit.used = 0; m.nslots = 0; continue; }
            blocks++, insns += b.ninsns ? b.ninsns : 1;
            i += b.ninsns ? b.ninsns : 1;
        }
    }
    double dt = now() - t0;
    HvStats *s = &m.stats;
    double hostw = (double)(s->words_inline + s->words_ool + s->words_pool);
    printf("%llu blocks, %.1f guest insns each (%.0f bytes); host: %.0f bytes a block (inline %.0f%%, out of line %.0f%%, literals %.0f%%), %.1fx the guest's\n",
           (unsigned long long)blocks, (double)insns / blocks, 4.0 * insns / blocks, 4 * hostw / blocks, 100 * s->words_inline / hostw,
           100 * s->words_ool / hostw, 100 * s->words_pool / hostw, hostw / insns);
    printf("time: %.2f s, %.2f us a block here\n", dt, 1e6 * dt / blocks);
    static const char *kn[16] = {"copy", "pcrel", "branch", "branch-reg", "eret", "literal", "memory", "prefetch", "exception", "sysreg", "sys", "barrier", "wait", "pac", "undef", "?"};
    uint64_t ti = 0, tw = 0;
    for (int k = 0; k < 16; k++) ti += s->kind_insns[k], tw += s->kind_words[k];
    printf("inline words per insn %.2f:", (double)tw / ti);
    for (int k = 0; k < 16; k++) if (s->kind_insns[k] * 200 >= ti) printf(" %s %.0f%% x%.1f (%.0f%% of words)", kn[k], 100.0 * s->kind_insns[k] / ti, (double)s->kind_words[k] / s->kind_insns[k], 100.0 * s->kind_words[k] / tw);
    printf("\n");
    return 0;
}
