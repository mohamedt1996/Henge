/* The Gunyah hand-over's first code in the VM (host/gunyah.c copies it to the start of the VM's own region).

   Henge runs the boot ROM and iBoot on its own CPUs (Gunyah gives a VM no memory below its base, where the ROM
   and the config window are) and hands CPU 0 to Gunyah at iBoot's jump into the kernel. Gunyah starts this at
   the region's base, EL1, MMU off. It:
     1. finds the resource manager's message queue in that tree (as android/tools/gunyah_probe_guest.S does),
     2. accepts each piece of the Mac's RAM Henge shared after the start (MEM_ACCEPT, DroidVM's pseudo-unprotected
        way, proven on the tablet up to 512 MB in 0.3.72),
     3. loads CPU 0's state as Henge's JIT left it and returns to the Mac there.
   What it needs comes from the hand-over page, a device page of Henge's: every read there is an MMIO exit Henge
   answers (gunyah.c, HANDOFF_*), and every store to its second half is a report Henge logs.

   The hand-over pages (8-byte words, 16 KB of them):
     T[0] "HNGEHAND", T[1] pieces, T[128 + 3i] handle, gpa, size of piece i (up to 640)
     T[64 + r], r: 0-30 x0-x30, 31 SP_EL1, 32 PC, 33 SPSR (the PSTATE to return to), 34 SCTLR, 35 TTBR0, 36 TTBR1,
       37 TCR, 38 MAIR, 39 AMAIR, 40 VBAR, 41 CONTEXTIDR, 42 TPIDR_EL0, 43 TPIDRRO_EL0, 44 TPIDR_EL1, 45 SP_EL0,
       46 CPACR, 47 CNTKCTL, 48 FPCR, 49 FPSR, 50 MDSCR
   Reports (+0x4000 + 8 n): 0 phase, 1 MIDR, 2 MPIDR, 3 ISAR1, 4 SCTLR as Gunyah started it, 5 queue found,
     6/7 its send/receive capabilities, 8 the piece being accepted, 9 MSGQ_SEND, 10 the reply's error (-1 none),
     11 ESR, 12 ELR, 13 FAR of an exception taken here, 14 APIAKeyLo (read under a guard), 15 that read ran;
     +0x4ff8: the jump (the Mac runs from here).
   Phases: 1 started, 2 queue found, 3 pieces accepted, 4 state loading, 0xdead: stopped (a report says why).

   Only single, naturally aligned loads and stores at the page: Gunyah can hand Henge only those (no LDP). Memory is
   Device with the MMU off, so the same holds for the tree and for rmbuf. */
/* A guarded probe: arm the guard, do one access, report the value and whether it faulted. */
    .macro gpre
    adr  x16, shim_try
    mov  x17, #1
    str  x17, [x16]
    str  xzr, [x16, #8]
    mov  x0, #0
    .endm
    .macro gpost n
    str  xzr, [x16]
    ldr  x17, [x16, #8]
    str  x0, [x9, #(0x100 + 16 * \n)]
    str  x17, [x9, #(0x108 + 16 * \n)]
    .endm
    .macro gaddr a
    movz x2, #((\a) & 0xffff)
    movk x2, #(((\a) >> 16) & 0xffff), lsl #16
    .endm
    .macro gld n, a, sz
    gaddr \a
    gpre
    .ifc \sz, w
    ldr  w0, [x2]
    .else
    ldr  x0, [x2]
    .endif
    gpost \n
    .endm
    .macro gst n, a
    gaddr \a
    gpre
    str  w3, [x2]
    gpost \n
    .endm
    .macro gsys n, op:vararg
    gpre
    \op
    gpost \n
    .endm

    .text
    .balign 4096
    .global hv_gunyah_shim_start, hv_gunyah_shim_end, hv_gunyah_shim_params
hv_gunyah_shim_start:
    adr  x1, hv_gunyah_shim_params
    ldr  x10, [x1]                      /* the hand-over page */
    ldr  x20, [x1, #8]                  /* the VM's device tree (as the resource manager wrote it back) */
    add  x9, x10, #0x4000               /* its reports */
    adr  x1, shim_vectors
    msr  vbar_el1, x1
    isb
    mov  x0, #1
    str  x0, [x9, #0x00]
    mrs  x0, midr_el1
    str  x0, [x9, #0x08]
    mrs  x0, mpidr_el1
    str  x0, [x9, #0x10]
    mrs  x0, id_aa64isar1_el1
    str  x0, [x9, #0x18]
    mrs  x0, sctlr_el1
    str  x0, [x9, #0x20]
    adr  x16, shim_try                  /* APIAKeyLo: does Gunyah let EL1 at the PAC keys */
    mov  x17, #1
    str  x17, [x16]
    mov  x0, #0
    mrs  x0, s3_0_c2_c1_0
    ldr  x17, [x16]
    str  x17, [x9, #0x78]
    str  x0, [x9, #0x70]

    /* 0.3.89: Gunyah's GIC as the kernel's pe_init_fiq will use it, each access under the guard: value at
       +0x100 + 16 n, then 0 (it ran) or ESR | 1 << 63 (it faulted) at +0x108 + 16 n. */
    gld  0, 0x10000000, w
    gld  1, 0x10000004, w
    gld  2, 0x10010008, x
    gld  3, 0x10010014, w
    gld  4, 0x10020080, w
    movz w3, #0xffff
    movk w3, #0x81ff, lsl #16
    gst  5, 0x10020080
    gld  6, 0x10020080, w
    gld  7, 0x10020100, w
    gld  8, 0x10030008, x
    gld  9, 0x10050008, x
    gsys 10, mrs x0, s3_0_c12_c12_5
    mov  x3, #0
    gsys 11, msr s3_0_c12_c8_3, x3
    mov  x3, #0xff
    gsys 12, msr s3_0_c4_c6_0, x3
    gsys 13, mrs x0, s3_0_c12_c12_4
    mov  x3, #1
    gsys 14, msr s3_0_c12_c12_6, x3
    gsys 15, mrs x0, s3_0_c12_c12_6
    gld  16, 0x40000000, w              /* 0.3.93: the PCIe windows, where the kernel's PCI configuration faulted */
    gld  17, 0x40008000, w
    gld  18, 0x50000000, w
    gld  19, 0x31000000, w              /* 0.3.95: where the PCIe ECAM moves (Henge's host bridge: 0x00081b36) */
    str  xzr, [x16]

    bl   rmfind
    cbz  x28, stop                      /* no queue: nothing can be accepted */
    mov  x0, #2
    str  x0, [x9, #0x00]

    ldr  x21, [x10, #0x08]              /* pieces */
    mov  x22, #0
    add  x23, x10, #0x400               /* T[128]: the pieces */
1:  cmp  x22, x21
    b.hs 2f
    str  x22, [x9, #0x40]
    ldr  x13, [x23]                     /* handle */
    ldr  x14, [x23, #8]                 /* address */
    ldr  x15, [x23, #16]                /* size */
    mov  x24, #64                       /* tries: a refusal may be for the moment (0.3.78: NORESOURCE on piece 1; 0.3.86:
                                           ARGUMENT_INVALID on piece 5 or 6 in half the runs, 4 quick tries all refused) */
3:  bl   accept
    cbz  x0, 4f
    subs x24, x24, #1
    b.eq stop
    mov  x0, #64
    sub  x0, x0, x24
    str  x0, [x9, #0x80]                /* the tries so far */
    mrs  x12, cntvct_el0                /* 50 ms by the counter (19.2 MHz) before the next: 3 s in all */
    movz x11, #0x000e, lsl #16
    movk x11, #0xa600
    add  x11, x12, x11
5:  mrs  x12, cntvct_el0
    cmp  x12, x11
    b.lo 5b
    b    3b
4:
    add  x23, x23, #24
    add  x22, x22, #1
    b    1b
2:  bl   dbfind                         /* 0.3.99: every interrupt doorbell clears itself once delivered */
    mov  x0, #3
    str  x0, [x9, #0x00]
    ic   iallu
    tlbi vmalle1
    dsb  sy
    isb

    mov  x0, #4
    str  x0, [x9, #0x00]
    add  x30, x10, #0x200               /* T[64]: the state */
    ldr  x0, [x30, #34 * 8]
    bic  x0, x0, #1                     /* the MMU stays off here (gunyah.c hands over only with it off) */
    msr  sctlr_el1, x0
    ldr  x0, [x30, #35 * 8]
    msr  ttbr0_el1, x0
    ldr  x0, [x30, #36 * 8]
    msr  ttbr1_el1, x0
    ldr  x0, [x30, #37 * 8]
    msr  tcr_el1, x0
    ldr  x0, [x30, #38 * 8]
    msr  mair_el1, x0
    ldr  x0, [x30, #39 * 8]
    msr  amair_el1, x0
    ldr  x0, [x30, #41 * 8]
    msr  contextidr_el1, x0
    ldr  x0, [x30, #42 * 8]
    msr  tpidr_el0, x0
    ldr  x0, [x30, #43 * 8]
    msr  tpidrro_el0, x0
    ldr  x0, [x30, #44 * 8]
    msr  tpidr_el1, x0
    ldr  x0, [x30, #45 * 8]
    msr  sp_el0, x0
    ldr  x0, [x30, #46 * 8]
    msr  cpacr_el1, x0
    ldr  x0, [x30, #47 * 8]
    msr  cntkctl_el1, x0
    ldr  x0, [x30, #48 * 8]
    msr  fpcr, x0
    ldr  x0, [x30, #49 * 8]
    msr  fpsr, x0
    ldr  x0, [x30, #50 * 8]
    msr  mdscr_el1, x0
    ldr  x0, [x30, #32 * 8]
    msr  elr_el1, x0                    /* the Mac's PC */
    ldr  x0, [x30, #33 * 8]
    msr  spsr_el1, x0                   /* and its PSTATE (EL1, its SP, DAIF, NZCV) */
    ldr  x0, [x30, #31 * 8]
    mov  sp, x0                         /* SP_EL1 */
    ldr  x0, [x30, #40 * 8]
    msr  vbar_el1, x0                   /* the Mac's vectors: from here an exception is the Mac's */
    isb
    mov  x0, #1
    str  x0, [x9, #0xff8]               /* the jump */
    ldr  x0, [x30, #0 * 8]
    ldr  x1, [x30, #1 * 8]
    ldr  x2, [x30, #2 * 8]
    ldr  x3, [x30, #3 * 8]
    ldr  x4, [x30, #4 * 8]
    ldr  x5, [x30, #5 * 8]
    ldr  x6, [x30, #6 * 8]
    ldr  x7, [x30, #7 * 8]
    ldr  x8, [x30, #8 * 8]
    ldr  x9, [x30, #9 * 8]
    ldr  x10, [x30, #10 * 8]
    ldr  x11, [x30, #11 * 8]
    ldr  x12, [x30, #12 * 8]
    ldr  x13, [x30, #13 * 8]
    ldr  x14, [x30, #14 * 8]
    ldr  x15, [x30, #15 * 8]
    ldr  x16, [x30, #16 * 8]
    ldr  x17, [x30, #17 * 8]
    ldr  x18, [x30, #18 * 8]
    ldr  x19, [x30, #19 * 8]
    ldr  x20, [x30, #20 * 8]
    ldr  x21, [x30, #21 * 8]
    ldr  x22, [x30, #22 * 8]
    ldr  x23, [x30, #23 * 8]
    ldr  x24, [x30, #24 * 8]
    ldr  x25, [x30, #25 * 8]
    ldr  x26, [x30, #26 * 8]
    ldr  x27, [x30, #27 * 8]
    ldr  x28, [x30, #28 * 8]
    ldr  x29, [x30, #29 * 8]
    ldr  x30, [x30, #30 * 8]
    eret

stop:
    movz x0, #0xdead
    str  x0, [x9, #0x00]
9:  wfi
    b    9b

/* ---- MEM_ACCEPT of one piece: x13 handle, x14 address, x15 size; x0 = 0 accepted. With x26/x27 the queue's
   send and receive capabilities. The request in 32-bit stores: header (api 0x21, type 1, seq 2, MEM_ACCEPT
   0x51000011), handle, mem type 0, transaction 2 (share), flags 0x90 (map IPA contiguous, done), validate label 0,
   no ACL, one {address, size}, no attributes. */
accept:
    adr  x5, shim_rmbuf
    movz w0, #0x0121
    movk w0, #0x0002, lsl #16
    str  w0, [x5, #0]
    movz w0, #0x0011
    movk w0, #0x5100, lsl #16
    str  w0, [x5, #4]
    str  w13, [x5, #8]
    movz w0, #0x0200
    movk w0, #0x0090, lsl #16
    str  w0, [x5, #12]
    str  wzr, [x5, #16]
    str  wzr, [x5, #20]
    mov  w0, #1
    str  w0, [x5, #24]
    str  w14, [x5, #28]
    lsr  x0, x14, #32
    str  w0, [x5, #32]
    str  w15, [x5, #36]
    lsr  x0, x15, #32
    str  w0, [x5, #40]
    str  wzr, [x5, #44]
    movz x0, #0x801b                    /* MSGQ_SEND */
    movk x0, #0xc600, lsl #16
    mov  x1, x26
    mov  x2, #48
    mov  x3, x5
    mov  x4, #1
    hvc  #0
    str  x0, [x9, #0x48]
    cbnz x0, 9f
    movz x12, #0x0100, lsl #16          /* wait a bounded while for the reply */
1:  adr  x5, shim_rmbuf
    movz x0, #0x801c                    /* MSGQ_RECV */
    movk x0, #0xc600, lsl #16
    mov  x1, x27
    mov  x2, x5
    mov  x3, #240
    hvc  #0
    cbnz x0, 2f
    ldrb w0, [x5, #1]
    and  w0, w0, #3
    cmp  w0, #2                         /* a reply */
    b.ne 2f
    ldr  w0, [x5, #8]                   /* its error code */
    str  x0, [x9, #0x50]
    ret
2:  subs x12, x12, #1
    b.ne 1b
    movn x0, #0
    str  x0, [x9, #0x50]
9:  mov  x0, #1
    ret

/* ---- the resource manager's message queue in the tree at x20: x28 = 1 and x26/x27 its send and receive
   capabilities when found (the node whose compatible names the resource manager or a message queue, its reg the
   two capabilities, one or two cells each). */
rmfind:
    mov  x19, x30
    ldr  w21, [x20, #8]
    rev  w21, w21                       /* off_dt_struct */
    ldr  w22, [x20, #12]
    rev  w22, w22                       /* off_dt_strings */
    add  x23, x20, x21
    add  x24, x20, x22
    mov  x25, #0
    mov  x14, #0
    mov  x28, #0
rmwalk:
    ldr  w0, [x23], #4
    rev  w0, w0
    cmp  w0, #1
    b.eq rmnode
    cmp  w0, #2
    b.eq rmclr
    cmp  w0, #3
    b.eq rmprop
    cmp  w0, #4
    b.eq rmwalk
    b    rmdone
rmnode:
1:  ldrb w1, [x23], #1
    cbnz w1, 1b
    add  x23, x23, #3
    bic  x23, x23, #3
rmclr:
    mov  x25, #0
    mov  x14, #0
    b    rmwalk
rmprop:
    ldr  w1, [x23], #4
    rev  w1, w1
    ldr  w2, [x23], #4
    rev  w2, w2
    add  x3, x24, x2
    mov  x4, x23
    add  x5, x1, #3
    bic  x5, x5, #3
    add  x23, x23, x5
    adr  x6, s_compatible
    bl   streq
    cbz  w0, rmreg
    mov  x3, x4
    add  x16, x4, x1
3:  cmp  x3, x16
    b.hs rmwalk
    adr  x6, s_rm1
    bl   strpre
    cbnz w0, 4f
    adr  x6, s_rm2
    bl   strpre
    cbnz w0, 4f
    adr  x6, s_msgq
    bl   strpre
    cbnz w0, 4f
5:  ldrb w0, [x3], #1
    cbnz w0, 5b
    b    3b
4:  mov  x25, #1
    b    rmcaps
rmreg:
    adr  x6, s_reg
    bl   streq
    cbz  w0, rmwalk
    cmp  w1, #8
    b.ne 6f
    ldr  w17, [x4]
    rev  w17, w17
    ldr  w18, [x4, #4]
    rev  w18, w18
    mov  x14, #1
    b    rmcaps
6:  cmp  w1, #16
    b.lo rmwalk
    ldr  w17, [x4, #4]
    rev  w17, w17
    ldr  w0, [x4]
    rev  w0, w0
    orr  x17, x17, x0, lsl #32
    ldr  w18, [x4, #12]
    rev  w18, w18
    ldr  w0, [x4, #8]
    rev  w0, w0
    orr  x18, x18, x0, lsl #32
    mov  x14, #1
rmcaps:
    cbz  x25, rmwalk
    cbz  x14, rmwalk
    mov  x26, x17
    mov  x27, x18
    mov  x28, #1
    b    rmwalk
rmdone:
    str  x28, [x9, #0x28]
    cbz  x28, 1f
    str  x26, [x9, #0x30]
    str  x27, [x9, #0x38]
1:  mov  x30, x19
    ret

/* ---- 0.3.99: the interrupt doorbells, made to clear on delivery. A doorbell's flags stay set until its receiver
   clears them or its ack mask does, and only a receiver may set the masks (doorbell_mask needs RECEIVE rights). The
   host's irqfd asks for level behaviour (enable 1, ack 1) with the send capability the resource manager gave it, which
   lacks RECEIVE unless the node says source-can-clear: refused, the masks stay at their defaults (all enabled, none
   acknowledged), and after its first ring a line stays asserted for good. XNU takes the PCIe INTx lines as level,
   so each one rung re-entered its handler at once and for ever (0.3.98: CPU 0 read the ISR of the two devices that
   had rung, slot 2's disk and slot 7's sound, 75,000 times a second each, always 0). This VM is the receiver: for each
   "qcom,gunyah-doorbell" node in its tree (reg: the capability), doorbell_mask(cap, all, all) makes every ring one
   assertion that clears itself, and doorbell_receive(cap, all) drops anything already latched. Reports: +0x88 the
   doorbells done, +0x90 the first error (0: none), +0x98 the first capability. */
dbfind:
    mov  x19, x30
    ldr  w21, [x20, #8]
    rev  w21, w21
    ldr  w22, [x20, #12]
    rev  w22, w22
    add  x23, x20, x21
    add  x24, x20, x22
    mov  x25, #0
    mov  x14, #0
    mov  x26, #0
    mov  x27, #0
    mov  x28, #0
dbwalk:
    ldr  w0, [x23], #4
    rev  w0, w0
    cmp  w0, #1
    b.eq dbnode
    cmp  w0, #2
    b.eq dbend
    cmp  w0, #3
    b.eq dbprop
    cmp  w0, #4
    b.eq dbwalk
    b    dbdone
dbnode:
1:  ldrb w1, [x23], #1
    cbnz w1, 1b
    add  x23, x23, #3
    bic  x23, x23, #3
    mov  x25, #0
    mov  x14, #0
    b    dbwalk
dbend:
    cbz  x25, 2f
    cbz  x14, 2f
    cbnz x28, 3f
    mov  x28, x17
3:  movz x0, #0x8015                    /* doorbell_mask(cap, enable all, ack all) */
    movk x0, #0xc600, lsl #16
    mov  x1, x17
    movn x2, #0
    movn x3, #0
    mov  x4, #0
    hvc  #0
    cbz  x0, 4f
    cbnz x27, 4f
    mov  x27, x0
4:  movz x0, #0x8013                    /* doorbell_receive(cap, all): whatever is latched now */
    movk x0, #0xc600, lsl #16
    mov  x1, x17
    movn x2, #0
    mov  x3, #0
    hvc  #0
    add  x26, x26, #1
2:  mov  x25, #0
    mov  x14, #0
    b    dbwalk
dbprop:
    ldr  w1, [x23], #4
    rev  w1, w1
    ldr  w2, [x23], #4
    rev  w2, w2
    add  x3, x24, x2
    mov  x4, x23
    add  x5, x1, #3
    bic  x5, x5, #3
    add  x23, x23, x5
    adr  x6, s_compatible
    bl   streq
    cbz  w0, 5f
    mov  x3, x4
    adr  x6, s_dbl
    bl   streq
    cbz  w0, dbwalk
    mov  x25, #1
    b    dbwalk
5:  adr  x6, s_reg
    bl   streq
    cbz  w0, dbwalk
    cmp  w1, #8
    b.ne dbwalk
    ldr  w17, [x4]
    rev  w17, w17
    ldr  w0, [x4, #4]
    rev  w0, w0
    orr  x17, x0, x17, lsl #32
    mov  x14, #1
    b    dbwalk
dbdone:
    str  x28, [x9, #0x98]
    str  x27, [x9, #0x90]
    str  x26, [x9, #0x88]
    mov  x30, x19
    ret

streq:
    mov  x7, x3
    mov  x8, x6
1:  ldrb w0, [x7], #1
    ldrb w2, [x8], #1
    cmp  w0, w2
    b.ne 2f
    cbnz w0, 1b
    mov  w0, #1
    ret
2:  mov  w0, #0
    ret

strpre:
    mov  x7, x3
    mov  x8, x6
1:  ldrb w2, [x8], #1
    cbz  w2, 2f
    ldrb w0, [x7], #1
    cmp  w0, w2
    b.eq 1b
    mov  w0, #0
    ret
2:  mov  w0, #1
    ret

s_rm1:        .asciz "gunyah-resource-manager"
s_rm2:        .asciz "qcom,resource-manager"
s_compatible: .asciz "compatible"
s_reg:        .asciz "reg"
s_msgq:       .asciz "qcom,gunyah-message-queue"
s_dbl:        .asciz "qcom,gunyah-doorbell"

    .balign 8
hv_gunyah_shim_params:
    .quad 0                             /* the hand-over page's address (gunyah.c fills these in) */
    .quad 0                             /* the VM's device tree */
shim_try:
    .quad 0                             /* 1 while a read that may trap is tried: the vectors skip it */
shim_tryesr:
    .quad 0                             /* and the skipped access's ESR | 1 << 63 */
shim_rmbuf:
    .fill 256, 1, 0

    .balign 2048
shim_vectors:
    .rept 16
    adr  x16, shim_try
    ldr  x17, [x16]
    cbz  x17, 1f
    str  xzr, [x16]
    mrs  x17, esr_el1
    orr  x17, x17, #(1 << 63)
    str  x17, [x16, #8]
    mrs  x17, elr_el1
    add  x17, x17, #4
    msr  elr_el1, x17
    eret
1:  adr  x1, hv_gunyah_shim_params
    ldr  x10, [x1]
    add  x9, x10, #0x4000
    mrs  x1, esr_el1
    str  x1, [x9, #0x58]
    mrs  x1, elr_el1
    str  x1, [x9, #0x60]
    mrs  x1, far_el1
    str  x1, [x9, #0x68]
    b    stop
    .balign 128
    .endr
hv_gunyah_shim_end:
