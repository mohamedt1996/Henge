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
     +0x30  the VM's device tree (0: none): the guest parses it for the resource-manager message queue and asks
            the resource manager its own VM id (VM_GET_VMID), to prove the guest can drive the resource manager
   and reads `flags` (image + 0x440): a byte a started CPU sets, by its number.

   Report slots (8 bytes each): 0x00 MIDR, 0x08 MMFR0, 0x10 ISAR1, 0x18 CurrentEL, 0x20 the PAC hypercall's
   answer, 0x28 SMCCC OEM UID, 0x30 PSCI version, 0x38 PFR0, 0x40 ESR and 0x48 ELR of an exception taken,
   0x50 pages written, 0x58 pages that read back wrong, 0x60 the PSCI CPU_ON answers or'ed together,
   0x68 CPUs that came up, 0x70 the address stored to once that store has returned, 0x80 what was read back
   through 16 KB pages (0x16c), 0x88 resource-manager message queue found in the tree (1), 0x90/0x98 its send and
   receive capabilities, 0xa0 MSGQ_SEND result, 0xa8 MSGQ_RECV result, 0xb0 the reply's error code, 0xb8 the VM
   id the resource manager returned, 0x78 done (0xd0e) or failed (0xbad). */
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
    bl   rmcall                         /* ask the resource manager its VM id, if a tree was given */
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
    .quad 0x10000000, 0, 0, 1, 0, 0, 0
    .org 0x440
flags:
    .fill 64, 1, 0

    .org 0x480
rmbuf:
    .fill 256, 1, 0

/* ---- the resource-manager conversation ----
   Parse the VM's device tree for the message-queue the resource manager gave this VM (a node whose compatible is
   "qcom,gunyah-message-queue", its reg = <tx-cap rx-cap>, two big-endian 64-bit values), then send one request
   (VM_GET_VMID) over it and read the reply. The message queues are Gunyah's own ABI (GH_HYPERCALL_MSGQ_SEND/RECV,
   function 0xc600801b/0xc600801c); the request is the resource manager's RPC: an 8-byte header (api 0x21, type 1
   request, seq, message id) with no payload, and the reply is that header, a 32-bit error code, then the id. */
    .org 0x580
rmcall:
    mov  x19, x30
    ldr  x20, [x10, #0x30]              /* the device tree (0: nothing to do) */
    cbz  x20, rmret
    ldr  w21, [x20, #8]
    rev  w21, w21                       /* off_dt_struct */
    ldr  w22, [x20, #12]
    rev  w22, w22                       /* off_dt_strings */
    add  x23, x20, x21                  /* the structure block */
    add  x24, x20, x22                  /* the strings block */
    mov  x25, #0                        /* in a message-queue node */
    mov  x28, #0                        /* its caps were found */
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
    b    rmsend                         /* end of tree, or a token we do not expect */
rmnode:
1:  ldrb w1, [x23], #1                  /* skip the node name, then align to four */
    cbnz w1, 1b
    add  x23, x23, #3
    bic  x23, x23, #3
rmclr:
    mov  x25, #0
    b    rmwalk
rmprop:
    ldr  w1, [x23], #4
    rev  w1, w1                         /* the property's length */
    ldr  w2, [x23], #4
    rev  w2, w2                         /* its name's offset in the strings block */
    add  x3, x24, x2                    /* the name */
    mov  x4, x23                        /* the data */
    add  x5, x1, #3
    bic  x5, x5, #3
    add  x23, x23, x5                   /* past the data, aligned to four */
    adr  x6, s_compatible
    bl   streq
    cbz  w0, rmreg
    mov  x3, x4                         /* compatible: does its first string name the message queue? */
    adr  x6, s_msgq
    bl   streq
    cbz  w0, rmwalk
    mov  x25, #1
    b    rmwalk
rmreg:
    adr  x6, s_reg
    bl   streq
    cbz  w0, rmwalk
    cbz  x25, rmwalk
    cmp  w1, #16
    b.lo rmwalk
    ldr  x26, [x4]
    rev  x26, x26                       /* the send capability */
    ldr  x27, [x4, #8]
    rev  x27, x27                       /* the receive capability */
    mov  x28, #1
    b    rmwalk
rmsend:
    cbz  x28, rmret                     /* no message queue: the guest cannot reach the resource manager */
    mov  x0, #1
    str  x0, [x9, #0x88]
    str  x26, [x9, #0x90]
    str  x27, [x9, #0x98]
    adr  x5, rmbuf                      /* the request: api 0x21, type 1, seq 1, message id 0x56000024 */
    movz w0, #0x0121
    strh w0, [x5]
    movz w0, #0x0001
    strh w0, [x5, #2]
    movz w0, #0x0024
    movk w0, #0x5600, lsl #16
    str  w0, [x5, #4]
    movz x0, #0x801b                    /* MSGQ_SEND */
    movk x0, #0xc600, lsl #16
    mov  x1, x26
    mov  x2, #8
    mov  x3, x5
    mov  x4, #1                         /* push the message */
    hvc  #0
    str  x0, [x9, #0xa0]
    cbnz x0, rmret
    movz x12, #0x0020, lsl #16          /* wait a bounded while for the reply */
rmrecv:
    adr  x5, rmbuf
    movz x0, #0x801c                    /* MSGQ_RECV */
    movk x0, #0xc600, lsl #16
    mov  x1, x27
    mov  x2, x5
    mov  x3, #240
    hvc  #0
    cbz  x0, rmgot
    subs x12, x12, #1
    b.ne rmrecv
    movn x0, #0
    str  x0, [x9, #0xa8]
    b    rmret
rmgot:
    str  x0, [x9, #0xa8]
    adr  x5, rmbuf
    ldr  w0, [x5, #8]                   /* the reply's error code */
    str  x0, [x9, #0xb0]
    ldr  w0, [x5, #12]                  /* the VM id it returned */
    str  x0, [x9, #0xb8]
rmret:
    mov  x30, x19
    ret

/* w0 = 1 if the string at x3 equals the one at x6 (both end at a zero byte). */
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

s_compatible: .asciz "compatible"
s_reg:        .asciz "reg"
s_msgq:       .asciz "qcom,gunyah-message-queue"

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
