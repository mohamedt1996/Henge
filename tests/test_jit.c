#include "harness.h"

#include <stdlib.h>

DECLARE_PROGRAM(arith);
DECLARE_PROGRAM(loop);
DECLARE_PROGRAM(condbr);
DECLARE_PROGRAM(csel_ops);
DECLARE_PROGRAM(bl_sub);
DECLARE_PROGRAM(simd_fp);
DECLARE_PROGRAM(stolen);
DECLARE_PROGRAM(pcrel);
DECLARE_PROGRAM(long_block);
DECLARE_PROGRAM(pstate_bits);
DECLARE_PROGRAM(pac);
extern const char jit_programs_begin[], jit_programs_finish[];

static const TestProgram programs[] = {
    TEST_PROGRAM(arith),   TEST_PROGRAM(loop),   TEST_PROGRAM(condbr),
    TEST_PROGRAM(csel_ops), TEST_PROGRAM(bl_sub), TEST_PROGRAM(simd_fp),
    TEST_PROGRAM(stolen),  TEST_PROGRAM(pcrel),  TEST_PROGRAM(long_block), TEST_PROGRAM(pstate_bits),
};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static HvMachine m;
    if (hv_machine_init(&m, 4 << 20) != 0) {
        printf("machine init failed\n");
        return 1;
    }
    /* The programs' own text is guest RAM at PA = host address, so ADR and
       pointer values agree with the native run. */
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)jit_programs_begin, jit_programs_finish - jit_programs_begin,
                       (void *)jit_programs_begin, 1);
    uint8_t *stack = aligned_alloc(16384, 65536);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
    uint64_t sp = (uint64_t)(uintptr_t)stack + 32768;

    m.trace = getenv("HV_TRACE") != NULL;
    int failed = 0;
    size_t count = sizeof programs / sizeof programs[0];
    for (size_t i = 0; i < count; i++) {
        const TestProgram *p = &programs[i];
        /* Two seeds per program: one with flags clear, one with NZCV set going in. */
        for (int variant = 0; variant < 2; variant++) {
            static HvCpu init, native, jit;
            memset(&init, 0, sizeof init);
            seed_cpu(&init, i * 2 + variant + 1, sp);
            if (variant) init.nzcv = 0xa0000000;
            run_native(p, &init, &native, NULL);
            int bad = run_jit(&m, p, &init, &jit, NULL);
            if (!bad) bad = compare_cpus(p->name, &native, &jit);
            printf("%-12s seed %d: %s\n", p->name, variant, bad ? "FAIL" : "ok");
            failed += bad != 0;
        }
    }
    /* PAC: compare against the inputs, with canonical pointers in the registers. */
    {
        static HvCpu init, jit;
        static const TestProgram pac_prog = TEST_PROGRAM(pac);
        memset(&init, 0, sizeof init);
        seed_cpu(&init, 99, sp);
        for (int r = 0; r < 31; r++) init.x[r] = r & 1 ? 0xfffffe0007123450ull + r * 8 : 0x100004000ull + r * 8;
        int bad = run_jit(&m, &pac_prog, &init, &jit, NULL);
        HvCpu want = init;
        want.x[4] = 0;
        want.x[9] = init.x[30];  /* RETAB came back here, so x30 was restored */
        want.x[10] = init.x[30];
        if (!bad) bad = compare_cpus("pac", &want, &jit);
        printf("%-12s       : %s\n", "pac", bad ? "FAIL" : "ok");
        failed += bad != 0;
    }
    /* Again with a JIT region so small that it fills (and everything is flushed)
       in the middle of a program, chains pending and all. */
    {
        static HvMachine small;
        if (hv_machine_init(&small, 1024) != 0) return 1;
        hv_machine_add_ram(&small, (uint64_t)(uintptr_t)jit_programs_begin, jit_programs_finish - jit_programs_begin,
                           (void *)jit_programs_begin, 1);
        hv_machine_add_ram(&small, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
        for (size_t i = 0; i < count; i++) {
            static HvCpu init, native, jit;
            memset(&init, 0, sizeof init);
            seed_cpu(&init, i * 2 + 1, sp);
            run_native(&programs[i], &init, &native, NULL);
            int bad = run_jit(&small, &programs[i], &init, &jit, NULL);
            if (!bad) bad = compare_cpus(programs[i].name, &native, &jit);
            if (bad) printf("%-12s with a full JIT region: FAIL\n", programs[i].name);
            failed += bad != 0;
        }
        printf("with a 1 KB JIT region: %llu flushes\n", (unsigned long long)small.stats.block_flushes);
    }
    /* 0.3.62: the cache in four parts, one dropped after every third translation, chains into it undone while
       blocks of the other parts keep theirs. */
    {
        static HvMachine parts;
        setenv("HV_JIT_PARTS", "4", 1), setenv("HV_JIT_PART_MIN", "64", 1), setenv("HV_JIT_EVICT_EVERY", "7", 1);
        if (hv_machine_init(&parts, 4 << 20) != 0) return 1;
        unsetenv("HV_JIT_PART_MIN"), unsetenv("HV_JIT_EVICT_EVERY");
        hv_machine_add_ram(&parts, (uint64_t)(uintptr_t)jit_programs_begin, jit_programs_finish - jit_programs_begin,
                           (void *)jit_programs_begin, 1);
        hv_machine_add_ram(&parts, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
        int pbad = parts.parts != 4;
        if (pbad) printf("the cache is not in parts (%d)\n", parts.parts);
        for (int round = 0; round < 3 && !pbad; round++)
            for (size_t i = 0; i < count; i++) {
                static HvCpu init, native, jit;
                memset(&init, 0, sizeof init);
                seed_cpu(&init, i * 2 + 1 + round, sp);
                run_native(&programs[i], &init, &native, NULL);
                int bad = run_jit(&parts, &programs[i], &init, &jit, NULL);
                if (!bad) bad = compare_cpus(programs[i].name, &native, &jit);
                if (bad) printf("%-12s with the cache in parts: FAIL\n", programs[i].name);
                pbad += bad != 0;
            }
        printf("with the cache in 4 parts: %llu parts dropped, %llu exits chained into them undone, %u blocks held\n",
               (unsigned long long)parts.stats.part_drops, (unsigned long long)parts.stats.part_unchained, hv_jit_live_blocks(&parts));
        if (!parts.stats.part_drops) printf("FAIL no part was dropped\n"), pbad++;
        failed += pbad;
    }
    printf("%zu programs x 2 seeds, %d failures, %d blocks translated\n", count, failed, m.nblocks);
    return failed != 0;
}
