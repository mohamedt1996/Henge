/* Gunyah probe guest: reports what this hypervisor gives an EL1 guest, through MMIO writes to 0x10000000. */
    .text
    .global _start
_start:
    movz x9, #0x1000, lsl #16           /* report page, outside guest RAM: every store is an MMIO exit */
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
    movz x1, #0xd0e
    str  x1, [x9, #0x78]                /* done */
    movz x0, #0x8400, lsl #16
    movk x0, #0x0008                    /* PSCI SYSTEM_OFF */
    hvc  #0
1:  wfi
    b    1b

    .balign 2048
vectors:
    .rept 16
    mrs  x1, esr_el1
    str  x1, [x9, #0x40]
    mrs  x1, elr_el1
    str  x1, [x9, #0x48]
    movz x1, #0xbad
    str  x1, [x9, #0x78]
2:  wfi
    b    2b
    .balign 128
    .endr
