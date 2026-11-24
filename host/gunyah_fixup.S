/* The Mac kernel's device accesses that Gunyah cannot carry out (0.3.92, widened in 0.3.93).

   A hypervisor emulates a device access from the syndrome the CPU gives it (ESR.ISV: size, register, direction).
   Some loads and stores give none: those that also update their base register (pre/post-indexed), pairs (LDP/STP)
   and SIMD&FP registers. Gunyah answers such a device access with a synchronous external abort (ESR 0x96000010,
   DFSC 0x10, ISV 0) instead. Apple's hypervisor and Henge's JIT decode the instruction themselves.

   gunyah.c copies this, once per kernel exception vector it patches (EL1 synchronous, SP0 and SPx), into the
   kernel's code padding and makes the vector's first instruction a branch here. Here, on SP_EL1 as the vector would
   be: an external abort without a syndrome from one of
     - LDR/STR (integer, zero-extending, 1-8 bytes) pre- or post-indexed,
     - LDP/STP (32 or 64-bit integer registers), any addressing,
     - LDR/STR of a SIMD&FP register (B, H, S, D, Q), unsigned offset or pre/post-indexed,
     - LDP/STP of SIMD&FP registers (S, D, Q), any addressing,
   on a base other than SP is carried out as plain integer accesses (which Gunyah can emulate) plus the base's
   update, and the instruction is skipped. Anything else goes on to the vector: its displaced first instruction, then
   a branch back (the last two words, which gunyah.c fills in for each copy).

   Frame on SP_EL1 (128 bytes): x0-x5 at 0-40, x16/x17 at 48/56; 64 ELR, 72 the new base, 80 Rt, 88 Rt2, 96 the
   address, 104 the element size, 112 Rn, 120 the instruction. */

    .macro MEMLD                        /* x2 = [x0], x4 bytes (1, 2, 4, 8) */
    cmp  x4, #1
    b.ne .Lml2\@
    ldrb w2, [x0]
    b    .Lmld\@
.Lml2\@:
    cmp  x4, #2
    b.ne .Lml4\@
    ldrh w2, [x0]
    b    .Lmld\@
.Lml4\@:
    cmp  x4, #4
    b.ne .Lml8\@
    ldr  w2, [x0]
    b    .Lmld\@
.Lml8\@:
    ldr  x2, [x0]
.Lmld\@:
    .endm

    .macro MEMST                        /* [x0] = x2, x4 bytes (1, 2, 4, 8) */
    cmp  x4, #1
    b.ne .Lms2\@
    strb w2, [x0]
    b    .Lmsd\@
.Lms2\@:
    cmp  x4, #2
    b.ne .Lms4\@
    strh w2, [x0]
    b    .Lmsd\@
.Lms4\@:
    cmp  x4, #4
    b.ne .Lms8\@
    str  w2, [x0]
    b    .Lmsd\@
.Lms8\@:
    str  x2, [x0]
.Lmsd\@:
    .endm

    .macro VLD                          /* x2 (low), x5 (high) = [x0], x4 bytes (1-16) */
    cmp  x4, #16
    b.ne .Lvl\@
    ldr  x2, [x0]
    ldr  x5, [x0, #8]
    b    .Lvld\@
.Lvl\@:
    MEMLD
    mov  x5, #0
.Lvld\@:
    .endm

    .macro VST                          /* [x0] = x2 (low), x5 (high), x4 bytes (1-16) */
    cmp  x4, #16
    b.ne .Lvs\@
    str  x2, [x0]
    str  x5, [x0, #8]
    b    .Lvsd\@
.Lvs\@:
    MEMST
.Lvsd\@:
    .endm

    .macro CALL fn                      /* the helpers return through x17 */
    adr  x17, .Lret\@
    b    \fn
.Lret\@:
    .endm

    .macro FP_SIZE                      /* x4 = a SIMD&FP LDR/STR's size (size, opc<1>), at [sp, #104]; declines a bad one */
    ldr  x3, [sp, #120]
    ubfx x1, x3, #30, #2
    mov  x4, #1
    lsl  x4, x4, x1
    tbz  x3, #23, .Lfs\@               /* opc<1>: 128-bit when size is 0 */
    cbnz x1, decline
    mov  x4, #16
.Lfs\@:
    str  x4, [sp, #104]
    .endm

    .macro PAIR_ADDR                    /* x4 the element size: the new base (base + imm7 * size) at 72, the address at
                                           96 (post-indexed: the old base) */
    ldr  x3, [sp, #120]
    sbfx x1, x3, #15, #7
    mul  x1, x1, x4
    str  x1, [sp, #96]
    ldr  x1, [sp, #112]
    CALL getreg
    ldr  x1, [sp, #96]
    add  x4, x2, x1
    str  x4, [sp, #72]
    ldr  x3, [sp, #120]
    ubfx x1, x3, #23, #2
    cmp  x1, #1
    csel x0, x2, x4, eq
    str  x0, [sp, #96]
    .endm

    .text
    .balign 4
#ifdef HV_FIX_TB                        /* host/gunyah_fixup_tb.S: the same, and the timer's traps (0.3.121) */
#define FIX_START hv_gunyah_fixtb_start
#define FIX_END hv_gunyah_fixtb_end
#else
#define FIX_START hv_gunyah_fix_start
#define FIX_END hv_gunyah_fix_end
#endif
    .global FIX_START, FIX_END
FIX_START:
    sub  sp, sp, #128
    stp  x0, x1, [sp, #0]
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
    stp  x16, x17, [sp, #48]
    mrs  x0, esr_el1
    lsr  x1, x0, #26
    cmp  x1, #0x25                      /* data abort at EL1 */
#ifdef HV_FIX_TB
    b.ne tbcheck
#else
    b.ne decline
#endif
    tbnz x0, #24, decline               /* with a syndrome: Gunyah could have done it, so not ours */
    and  x1, x0, #0x3f
    cmp  x1, #0x10                      /* synchronous external abort */
    b.ne decline
    mrs  x0, elr_el1
    str  x0, [sp, #64]
    ldr  w3, [x0]                       /* the instruction */
    str  x3, [sp, #120]
    ubfx x5, x3, #5, #5                 /* Rn */
    cmp  x5, #31
    b.eq decline                        /* SP: never a device */
    str  x5, [sp, #112]
    ubfx x1, x3, #0, #5
    str  x1, [sp, #80]                  /* Rt */
    ubfx x1, x3, #10, #5
    str  x1, [sp, #88]                  /* Rt2 (pairs) */

    movz w1, #0x0400                    /* integer LDR/STR, pre/post-indexed */
    movk w1, #0x3f20, lsl #16
    and  w1, w3, w1
    movz w4, #0x0400
    movk w4, #0x3800, lsl #16
    cmp  w1, w4
    b.eq gpr1
    and  w1, w3, #0x3e000000            /* integer LDP/STP */
    movz w4, #0x2800, lsl #16
    cmp  w1, w4
    b.eq gpr2
    and  w1, w3, #0x3f000000            /* SIMD&FP LDR/STR, unsigned offset */
    movz w4, #0x3d00, lsl #16
    cmp  w1, w4
    b.eq fp1u
    movz w1, #0x0400                    /* SIMD&FP LDR/STR, pre/post-indexed */
    movk w1, #0x3f20, lsl #16
    and  w1, w3, w1
    movz w4, #0x0400
    movk w4, #0x3c00, lsl #16
    cmp  w1, w4
    b.eq fp1i
    and  w1, w3, #0x3e000000            /* SIMD&FP LDP/STP */
    movz w4, #0x2c00, lsl #16
    cmp  w1, w4
    b.eq fp2
    b    decline

/* ---- integer LDR/STR pre/post: imm9, one access */
gpr1:
    ubfx x4, x3, #22, #2                /* opc: 0 store, 1 load (zero-extending); signed loads not here */
    cmp  x4, #1
    b.hi decline
    ubfx x4, x3, #30, #2
    mov  x1, #1
    lsl  x4, x1, x4                     /* size */
    str  x4, [sp, #104]
    sbfx x1, x3, #12, #9
    str  x1, [sp, #96]                  /* (offset, for the moment) */
    ldr  x1, [sp, #112]
    CALL getreg                         /* x2 = base */
    ldr  x1, [sp, #96]
    add  x4, x2, x1                     /* new base */
    str  x4, [sp, #72]
    ldr  x3, [sp, #120]
    tst  x3, #0x800                     /* pre-indexed: at the new base */
    csel x0, x4, x2, ne
    str  x0, [sp, #96]
    ldr  x4, [sp, #104]
    tbnz x3, #22, 1f
    ldr  x1, [sp, #80]
    CALL getreg
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    MEMST
    b    wb
1:  MEMLD
    ldr  x1, [sp, #80]
    CALL setreg
    b    wb

/* ---- integer LDP/STP: imm7 scaled, two accesses; modes 0/2 offset, 1 post, 3 pre */
gpr2:
    ubfx x1, x3, #30, #2                /* opc: 0 32-bit, 2 64-bit (1 LDPSW: not here) */
    mov  x4, #4
    cbz  x1, 1f
    cmp  x1, #2
    b.ne decline
    mov  x4, #8
1:  str  x4, [sp, #104]
    PAIR_ADDR
    ldr  x3, [sp, #120]
    tbnz x3, #22, 2f
    ldr  x1, [sp, #80]                  /* store: Rt, then Rt2 */
    CALL getreg
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    MEMST
    ldr  x1, [sp, #88]
    CALL getreg
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    add  x0, x0, x4
    MEMST
    b    pairwb
2:  ldr  x0, [sp, #96]                  /* load: both read before either register changes (Rt may be the base) */
    ldr  x4, [sp, #104]
    MEMLD
    str  x2, [sp, #80 + 0]              /* (Rt's number is re-read below from the instruction) */
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    add  x0, x0, x4
    MEMLD
    ldr  x3, [sp, #120]
    ubfx x1, x3, #10, #5
    CALL setreg                         /* Rt2 */
    ldr  x2, [sp, #80]
    ldr  x3, [sp, #120]
    ubfx x1, x3, #0, #5
    CALL setreg                         /* Rt */
    b    pairwb

/* ---- SIMD&FP LDR/STR, unsigned offset: imm12 scaled */
fp1u:
    FP_SIZE
    ldr  x3, [sp, #120]
    ubfx x1, x3, #10, #12
    mul  x1, x1, x4
    str  x1, [sp, #96]
    ldr  x1, [sp, #112]
    CALL getreg
    ldr  x1, [sp, #96]
    add  x0, x2, x1
    str  x0, [sp, #96]
    str  x2, [sp, #72]                  /* no writeback: the base stays */
    b    fp1go
/* ---- SIMD&FP LDR/STR pre/post: imm9 */
fp1i:
    FP_SIZE
    ldr  x3, [sp, #120]
    sbfx x1, x3, #12, #9
    str  x1, [sp, #96]
    ldr  x1, [sp, #112]
    CALL getreg
    ldr  x1, [sp, #96]
    add  x4, x2, x1
    str  x4, [sp, #72]
    ldr  x3, [sp, #120]
    tst  x3, #0x800
    csel x0, x4, x2, ne
    str  x0, [sp, #96]
fp1go:
    ldr  x3, [sp, #120]
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    tbnz x3, #22, 1f
    ldr  x1, [sp, #80]
    CALL getv                           /* x2, x5 */
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    VST
    b    wb
1:  VLD
    ldr  x1, [sp, #80]
    ldr  x4, [sp, #104]
    CALL setv
    b    wb

/* ---- SIMD&FP LDP/STP: opc 0 S, 1 D, 2 Q */
fp2:
    ubfx x1, x3, #30, #2
    cmp  x1, #2
    b.hi decline
    add  x1, x1, #2
    mov  x4, #1
    lsl  x4, x4, x1
    str  x4, [sp, #104]
    PAIR_ADDR
    ldr  x3, [sp, #120]
    tbnz x3, #22, 2f
    ldr  x1, [sp, #80]
    CALL getv
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    VST
    ldr  x1, [sp, #88]
    CALL getv
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    add  x0, x0, x4
    VST
    b    pairwb
2:  ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    VLD
    ldr  x1, [sp, #80]
    ldr  x4, [sp, #104]
    CALL setv
    ldr  x0, [sp, #96]
    ldr  x4, [sp, #104]
    add  x0, x0, x4
    VLD
    ldr  x1, [sp, #88]
    ldr  x4, [sp, #104]
    CALL setv
    b    pairwb

/* Pairs: modes 1 and 3 write the base back. */
pairwb:
    ldr  x3, [sp, #120]
    ubfx x1, x3, #23, #2
    cmp  x1, #1
    b.eq wb
    cmp  x1, #3
    b.eq wb
    b    done
wb: ldr  x3, [sp, #120]
    and  w1, w3, #0x3f000000            /* unsigned-offset forms never write back */
    movz w4, #0x3d00, lsl #16
    cmp  w1, w4
    b.eq done
    ldr  x2, [sp, #72]
    ldr  x1, [sp, #112]
    CALL setreg
done:
    ldr  x0, [sp, #64]
    add  x0, x0, #4
    msr  elr_el1, x0
    ldp  x0, x1, [sp, #0]
    ldp  x2, x3, [sp, #16]
    ldp  x4, x5, [sp, #32]
    ldp  x16, x17, [sp, #48]
    add  sp, sp, #128
    eret

/* x2 = integer register x1 (31: zero) as the faulting code had it; returns to x17. Uses x16. */
getreg:
    adr  x16, gtab
    add  x16, x16, x1, lsl #3
    br   x16
/* integer register x1 = x2; returns to x17. Uses x16. */
setreg:
    adr  x16, stab
    add  x16, x16, x1, lsl #3
    br   x16
/* x2, x5 = SIMD&FP register x1's low and high halves; returns to x17. Uses x16. */
getv:
    adr  x16, gvtab
    add  x16, x16, x1, lsl #4
    br   x16
/* SIMD&FP register x1 = x2 (low), x5 (high, when x4 is 16; else zero, as a load does); returns to x17. */
setv:
    adr  x16, svtab
    add  x16, x16, x1, lsl #4
    br   x16

    .balign 8
gtab:
    .irp r, 0,1,2,3,4,5
    ldr  x2, [sp, #(\r * 8)]
    br   x17
    .endr
    .irp r, 6,7,8,9,10,11,12,13,14,15
    mov  x2, x\r
    br   x17
    .endr
    ldr  x2, [sp, #48]
    br   x17
    ldr  x2, [sp, #56]
    br   x17
    .irp r, 18,19,20,21,22,23,24,25,26,27,28,29,30
    mov  x2, x\r
    br   x17
    .endr
    mov  x2, xzr
    br   x17
stab:
    .irp r, 0,1,2,3,4,5
    str  x2, [sp, #(\r * 8)]
    br   x17
    .endr
    .irp r, 6,7,8,9,10,11,12,13,14,15
    mov  x\r, x2
    br   x17
    .endr
    str  x2, [sp, #48]
    br   x17
    str  x2, [sp, #56]
    br   x17
    .irp r, 18,19,20,21,22,23,24,25,26,27,28,29,30
    mov  x\r, x2
    br   x17
    .endr
    nop
    br   x17
    .balign 16
gvtab:
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fmov x2, d\r
    mov  x5, v\r\().d[1]
    br   x17
    nop
    .endr
svtab:
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fmov d\r, x2
    tbz  x4, #4, . + 8
    mov  v\r\().d[1], x5
    br   x17
    .endr

#ifdef HV_FIX_TB
/* 0.3.121: the Mac's clock at 24 MHz on a 19.2 MHz counter. gunyah.c makes each generic-timer instruction of the
   kernel the permanently undefined word 0x00007a00 | op << 5 | Rt (exception class 0), carried out here:
   op 0-5 read CNTVCT, CNTPCT, CNTVCTSS, CNTPCTSS, CNTV_CVAL, CNTP_CVAL: the value and a quarter of it;
   op 6-7 read CNTV_TVAL, CNTP_TVAL: the signed 32-bit value and a quarter of it; op 8 reads CNTFRQ: 24,000,000;
   op 9-12 write CNTV_CVAL, CNTP_CVAL, CNTV_TVAL, CNTP_TVAL: the value less a fifth of it (TVAL: of its signed 32 bits). */
tbcheck:
    cbnz x1, decline                    /* class 0: an undefined instruction */
    mrs  x0, elr_el1
    str  x0, [sp, #64]
    ldr  w3, [x0]
    lsr  w1, w3, #9
    cmp  w1, #0x3d                      /* 0x00007a00 >> 9 */
    b.ne decline
    ubfx x4, x3, #5, #4                 /* op */
    cmp  x4, #12
    b.hi decline
    cmp  x4, #9
    b.lo 1f
    and  x1, x3, #31
    CALL getreg                         /* a write: x2 = Rt (x3, x4 kept) */
    cmp  x4, #11
    b.lo 0f
    sxtw x2, w2
0:  mov  x1, #5
    sdiv x1, x2, x1
    sub  x2, x2, x1
1:  adr  x16, ttab
    add  x16, x16, x4, lsl #3
    br   x16
ttab:
    mrs  x2, cntvct_el0
    b    tbrs
    mrs  x2, cntpct_el0
    b    tbrs
    mrs  x2, S3_3_C14_C0_6              /* CNTVCTSS_EL0 */
    b    tbrs
    mrs  x2, S3_3_C14_C0_5              /* CNTPCTSS_EL0 */
    b    tbrs
    mrs  x2, cntv_cval_el0
    b    tbrs
    mrs  x2, cntp_cval_el0
    b    tbrs
    mrs  x2, cntv_tval_el0
    b    tbrt
    mrs  x2, cntp_tval_el0
    b    tbrt
    movz x2, #0x3600
    b    tbrf
    msr  cntv_cval_el0, x2
    b    done
    msr  cntp_cval_el0, x2
    b    done
    msr  cntv_tval_el0, x2
    b    done
    msr  cntp_tval_el0, x2
    b    done
tbrf:
    movk x2, #0x16e, lsl #16            /* 24,000,000 */
    b    tbset
tbrt:
    sxtw x2, w2
    add  x2, x2, x2, asr #2
    b    tbset
tbrs:
    add  x2, x2, x2, lsr #2
tbset:
    and  x1, x3, #31
    CALL setreg
    b    done
#endif

decline:
    ldp  x0, x1, [sp, #0]
    ldp  x2, x3, [sp, #16]
    ldp  x4, x5, [sp, #32]
    ldp  x16, x17, [sp, #48]
    add  sp, sp, #128
    .word 0                             /* the vector's own first instruction (gunyah.c) */
    .word 0                             /* b vector + 4 (gunyah.c) */
FIX_END:
