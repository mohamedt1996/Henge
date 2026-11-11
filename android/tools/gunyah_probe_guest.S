/* Gunyah probe guest: reports what this hypervisor gives an EL1 guest, through MMIO writes to its report page
   (every store there leaves the guest: an MMIO exit the probe reads). Position independent; no stack, no MMU.

   The probe fills `params` (image + 0x400) before the VM starts:
     +0x00  report page                 (0x10000000, or wherever the layout has no device of Gunyah's own)
     +0x08  memory to touch, first byte (0: touch nothing)
     +0x10  memory to touch, bytes
     +0x18  CPUs                        (more than 1: the others are started through PSCI)
     +0x20  an address to store to, last (0: none): whether a store there leaves the guest, as a device's would
     +0x28  16 KB of zeroed memory for a page table (0: none): the MMU is turned on with 16 KB pages, as macOS
            has it, whatever ID_AA64MMFR0 says of them
   and reads `flags` (image + 0x440): a byte a started CPU sets, by its number.

   Report slots (8 bytes each): 0x00 MIDR, 0x08 MMFR0, 0x10 ISAR1, 0x18 CurrentEL, 0x20 the PAC hypercall's
   answer, 0x28 SMCCC OEM UID, 0x30 PSCI version, 0x38 PFR0, 0x40 ESR and 0x48 ELR of an exception taken,
   0x50 pages written, 0x58 pages that read back wrong, 0x60 the PSCI CPU_ON answers or'ed together,
   0x68 CPUs that came up, 0x70 the address stored to once that store has returned, 0x80 what was read back
   through 16 KB pages (0x16c), 0x78 done (0xd0e) or failed (0xbad). */
    .text
    .global _start
_start:
    adr  x10, params
    ldr  x9, [x10, #0x00]               /* report page */
    adr  x1, vectors
    msr  vbar_el1, x1
    isb
    mrs  x1, midr_el1
    str  x1, [x9, #0x00]
    mrs  x1, id_aa64mmfr0_el1           /* TGran16: 16K pages, which macOS needs */
    str  x1, [x9, #0x08]
    mrs  x1, id_aa64isar1_el1           /* pointer authentication */
    str  x1, [x9, #0x10]
    mrs  x1, CurrentEL
    str  x1, [x9, #0x18]
    movz x0, #0xc100, lsl #16           /* VMAPPLE_PAC_SET_INITIAL_STATE: XNU stops unless this returns 0 */
    hvc  #0
    str  x0, [x9, #0x20]
    movz x0, #0xff01
    movk x0, #0x8300, lsl #16           /* SMCCC OEM Call UID */
    hvc  #0
    str  x0, [x9, #0x28]
    movz x0, #0x8400, lsl #16           /* PSCI_VERSION */
    hvc  #0
    str  x0, [x9, #0x30]
    mrs  x1, id_aa64pfr0_el1
    str  x1, [x9, #0x38]

    /* Every page of the memory asked for: its own address written, then read back. */
    ldr  x11, [x10, #0x08]              /* first byte */
    ldr  x12, [x10, #0x10]              /* bytes */
    cbz  x12, cpus
    add  x12, x11, x12                  /* end */
    mov  x13, x11
    mov  x14, #0
1:  str  x13, [x13]
    add  x14, x14, #1
    add  x13, x13, #0x1000
    cmp  x13, x12
    b.lo 1b
    str  x14, [x9, #0x50]
    mov  x13, x11
    mov  x14, #0
2:  ldr  x15, [x13]
    cmp  x15, x13
    cinc x14, x14, ne
    add  x13, x13, #0x1000
    cmp  x13, x12
    b.lo 2b
    str  x14, [x9, #0x58]

    /* The other CPUs, through PSCI CPU_ON; each sets its flag. */
cpus:
    ldr  x11, [x10, #0x18]
    cmp  x11, #2
    b.lo done
    mov  x12, #1                        /* the CPU to start */
    mov  x13, #0                        /* the answers, or'ed */
3:  movz x0, #0xc400, lsl #16
    movk x0, #0x0003                    /* PSCI CPU_ON (SMC64) */
    mov  x1, x12                        /* its MPIDR: the number, as the VM's tree gives it */
    adr  x2, secondary
    mov  x3, x12                        /* handed to it in x0 */
    hvc  #0
    orr  x13, x13, x0
    add  x12, x12, #1
    cmp  x12, x11
    b.lo 3b
    str  x13, [x9, #0x60]
    /* Wait for them, a bounded while. */
    movz x14, #0x0040, lsl #16          /* about four million looks */
    adr  x15, flags
4:  mov  x12, #1
    mov  x16, #0
5:  ldrb w17, [x15, x12]
    add  x16, x16, x17
    add  x12, x12, #1
    cmp  x12, x11
    b.lo 5b
    sub  x17, x11, #1
    cmp  x16, x17
    b.hs 6f
    subs x14, x14, #1
    b.ne 4b
6:  str  x16, [x9, #0x68]

done:
    ldr  x11, [x10, #0x20]              /* a store where the Mac has a device: an exit, or an exception */
    cbz  x11, 10f
    str  x11, [x11]
    str  x11, [x9, #0x70]
10: ldr  x11, [x10, #0x28]              /* the page table: one level-2 table of 16 KB pages, 32 MB a block */
    cbz  x11, 12f
    adr  x12, _start
    and  x12, x12, #0xfffffffffe000000  /* this program's 32 MB, mapped at its own address, normal memory */
    lsr  x13, x12, #25
    and  x13, x13, #0x7ff
    movz x14, #0x701                    /* block, AF, inner shareable, read-write, attribute 0 */
    orr  x14, x14, x12
    str  x14, [x11, x13, lsl #3]
    and  x12, x9, #0xfffffffffe000000   /* the report page's 32 MB: device memory, never executed */
    lsr  x13, x12, #25
    and  x13, x13, #0x7ff
    movz x14, #0x405                    /* block, AF, attribute 1 */
    movk x14, #0x0060, lsl #48          /* UXN, PXN */
    orr  x14, x14, x12
    str  x14, [x11, x13, lsl #3]
    movz x1, #0x04ff                    /* attribute 0: normal, write-back; 1: device */
    msr  mair_el1, x1
    movz x1, #0xb51c                    /* 36-bit addresses (T0SZ 28), 16 KB pages (TG0 = 2), write-back walks */
    movk x1, #0x0080, lsl #16           /* no walks of the upper half */
    movk x1, #0x0001, lsl #32           /* 36-bit physical addresses */
    msr  tcr_el1, x1
    msr  ttbr0_el1, x11
    isb
    mrs  x1, sctlr_el1
    bic  x1, x1, #0x80000               /* writable memory may be executed (WXN off): this program's is */
    orr  x1, x1, #1                     /* the MMU on */
    msr  sctlr_el1, x1
    isb
    movz x1, #0x16c
    add  x2, x11, #0x3000               /* a word of the table's own memory, through the new mapping */
    str  x1, [x2, #0xff8]
    ldr  x3, [x2, #0xff8]
    str  x3, [x9, #0x80]
12: movz x1, #0xd0e
    str  x1, [x9, #0x78]
    movz x0, #0x8400, lsl #16
    movk x0, #0x0008                    /* PSCI SYSTEM_OFF */
    hvc  #0
7:  wfi
    b    7b

/* A CPU started by CPU_ON: x0 is its number. */
secondary:
    adr  x1, flags
    mov  w2, #1
    strb w2, [x1, x0]
    movz x0, #0x8400, lsl #16
    movk x0, #0x0002                    /* PSCI CPU_OFF */
    hvc  #0
8:  wfi
    b    8b

    .org 0x400
params:
    .quad 0x10000000, 0, 0, 1, 0, 0
    .org 0x440
flags:
    .fill 64, 1, 0

    .org 0x800
vectors:
    .rept 16
    adr  x10, params
    ldr  x9, [x10]
    mrs  x1, esr_el1
    str  x1, [x9, #0x40]
    mrs  x1, elr_el1
    str  x1, [x9, #0x48]
    movz x1, #0xbad
    str  x1, [x9, #0x78]
9:  wfi
    b    9b
    .balign 128
    .endr
