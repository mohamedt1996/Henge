/* The Mac kernel's device accesses that Gunyah cannot carry out (0.3.92).

   A hypervisor emulates a device access from the syndrome the CPU gives it (ESR.ISV: size, register, direction).
   Loads and stores that also update their base register (pre- or post-indexed) give none, and Gunyah answers them
   with a synchronous external abort (ESR 0x96000010, DFSC 0x10, ISV 0) instead. XNU's own GIC set-up uses one,
   `str w9, [x8, #0x80]!` in pe_init_fiq, and panicked on it. Apple's hypervisor and Henge's JIT decode the
   instruction themselves.

   gunyah.c copies this, once per kernel exception vector it patches (EL1 synchronous, SP0 and SPx), into the
   kernel's code padding and makes the vector's first instruction a branch here. Here, on SP_EL1 as the vector would
   be: an external abort without a syndrome, from an integer LDR/STR (zero-extending, 1-8 bytes) pre- or
   post-indexed on a base other than SP, is done as a plain access (which Gunyah can emulate) plus the base's update,
   and the instruction is skipped. Anything else goes on to the vector: its displaced first instruction, then a
   branch back (the last two words, which gunyah.c fills in for each copy).

   Frame on SP_EL1: x0-x5 at 0-40, x16/x17 at 48/56, then ELR at 64, the new base at 72, Rt at 80. */
    .text
    .balign 4
    .global hv_gunyah_fix_start, hv_gunyah_fix_end
hv_gunyah_fix_start:
    sub  sp, sp, #96
    stp  x0, x1, [sp, #0]
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
    stp  x16, x17, [sp, #48]
    mrs  x0, esr_el1
    lsr  x1, x0, #26
    cmp  x1, #0x25                      /* data abort at EL1 */
    b.ne decline
    tbnz x0, #24, decline               /* with a syndrome: Gunyah could have done it, so not ours */
    and  x1, x0, #0x3f
    cmp  x1, #0x10                      /* synchronous external abort */
    b.ne decline
    mrs  x0, elr_el1
    ldr  w3, [x0]                       /* the instruction */
    movz w1, #0x0400
    movk w1, #0x3f20, lsl #16
    and  w1, w3, w1
    movz w4, #0x0400
    movk w4, #0x3800, lsl #16
    cmp  w1, w4                         /* LDR/STR (immediate), pre/post-indexed, integer */
    b.ne decline
    ubfx x4, x3, #22, #2                /* opc: 0 store, 1 load (zero-extending); signed loads not here */
    cmp  x4, #1
    b.hi decline
    ubfx x5, x3, #5, #5                 /* Rn */
    cmp  x5, #31
    b.eq decline                        /* SP: never a device */
    str  x0, [sp, #64]
    mov  x1, x5
    adr  x17, 1f
    b    getreg
1:  sbfx x1, x3, #12, #9                /* imm9 */
    add  x4, x2, x1                     /* the new base */
    tst  x3, #0x800                     /* pre-indexed: the access at the new base */
    csel x0, x4, x2, ne
    str  x4, [sp, #72]
    ubfx x1, x3, #0, #5                 /* Rt */
    str  x1, [sp, #80]
    ubfx x4, x3, #30, #2                /* size */
    tbnz x3, #22, load
    adr  x17, 2f
    b    getreg
2:  cmp  x4, #0
    b.ne 3f
    strb w2, [x0]
    b    wb
3:  cmp  x4, #1
    b.ne 4f
    strh w2, [x0]
    b    wb
4:  cmp  x4, #2
    b.ne 5f
    str  w2, [x0]
    b    wb
5:  str  x2, [x0]
    b    wb
load:
    cmp  x4, #0
    b.ne 6f
    ldrb w2, [x0]
    b    7f
6:  cmp  x4, #1
    b.ne 8f
    ldrh w2, [x0]
    b    7f
8:  cmp  x4, #2
    b.ne 9f
    ldr  w2, [x0]
    b    7f
9:  ldr  x2, [x0]
7:  ldr  x1, [sp, #80]
    adr  x17, wb
    b    setreg
wb: ldr  x2, [sp, #72]
    mov  x1, x5
    adr  x17, 10f
    b    setreg
10: ldr  x0, [sp, #64]
    add  x0, x0, #4
    msr  elr_el1, x0
    ldp  x0, x1, [sp, #0]
    ldp  x2, x3, [sp, #16]
    ldp  x4, x5, [sp, #32]
    ldp  x16, x17, [sp, #48]
    add  sp, sp, #96
    eret

/* x2 = register x1 (31: zero) as the faulting code had it; returns to x17. Uses x16. */
getreg:
    adr  x16, gtab
    add  x16, x16, x1, lsl #3
    br   x16
/* register x1 = x2; returns to x17. Uses x16. */
setreg:
    adr  x16, stab
    add  x16, x16, x1, lsl #3
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

decline:
    ldp  x0, x1, [sp, #0]
    ldp  x2, x3, [sp, #16]
    ldp  x4, x5, [sp, #32]
    ldp  x16, x17, [sp, #48]
    add  sp, sp, #96
    .word 0                             /* the vector's own first instruction (gunyah.c) */
    .word 0                             /* b vector + 4 (gunyah.c) */
hv_gunyah_fix_end:
