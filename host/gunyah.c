/* The virtual Mac's CPUs on Gunyah, from the jump into the kernel on (see gunyah.h). Linux/Android only.

   Why a hand-over: Gunyah's resource manager on the tablet (Xiaomi Pad 7, SM8635, android14-6.1) gives a VM no
   memory at the bottom of its address space: a VM based at 0 is refused when it starts, and memory shared at the boot
   ROM's place (0x100000) is refused when the guest accepts it (the app's Gunyah test, 0.3.73). The Mac's boot ROM and
   config window live there. So Henge's own CPUs run the ROM and iBoot as always, and at iBoot's jump into the kernel
   (the third time CPU 0 turns its MMU off in RAM, host/vm.c) CPU 0 stops and this goes on from the same state:

   - The VM: its own 2 MB region at BOOT_PA, just under the Mac's RAM (lent: the shim and the resource manager's
     device tree), the Mac's RAM shared into it after it starts through DroidVM's /dev/gunyah_share (the
     gunyah-host-share module) in 2 MB pages, and accepted by the guest itself (host/gunyah_shim.S). Both work on the
     tablet (0.3.72-0.3.74). Runs of 2 MB pages go in pieces of up to 256 MB, runs of 4 KB pages in pieces of 8 MB (a
     64 MB parcel of them reset the tablet, 8 MB did not); henge-reserve holds the RAM in 2 MB pages from boot
     (android/tools/henge_reserve.c), so there are few of the small ones.
   - The shim loads CPU 0's state from the hand-over page (HANDOFF_PA: reads there come back here) and returns to the
     Mac where the JIT stopped. Nothing below 0x70000000 is the VM's memory: the ROM and the config window are not
     used past this point (0.3.75's log says whether anything still reads them; each read would come here as an MMIO
     exit and be answered from Henge's copy, which works for single loads only).
   - Before the start, the kernel (the kernel collection iBoot loaded, found from boot_args) is made to run on a
     Qualcomm CPU: Apple's implementation-defined registers (op0 3, CRn 11 and 15) read as 0 and ignore writes, as
     Henge's JIT has them; MIDR_EL1 reads Apple's value the JIT gives (a branch to a stub in the padding at the end of
     the code); vmapple's PAC hypercalls answer success (as before). The kernel's device tree gets the CPU's own counter
     rate as its timebase-frequency (19.2 MHz here, where the JIT counts 24).
   - GIC and timer: Gunyah's virtual GICv3 at vmapple's addresses, the CPU's own virtual timer. Device interrupts: one
     Gunyah doorbell (irqfd) per vmapple SPI, raised from Henge's GIC entry points through hv_gic_external.
   - MMIO: each exit goes to hv_phys_read/hv_phys_write, Henge's device dispatch, under the same device lock; a thread
     runs the machine's tick (virtio queues, USB, network, display) every millisecond and after any MMIO write.
   - vCPUs: one per Henge CPU, each run by its own host thread on the core the JIT gave that CPU; Gunyah answers the
     guest's PSCI, so XNU's CPU_ON brings the others up itself. */
#include "gunyah.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "gicv3.h"
#include "machine.h"
#include "vm.h"
#include "vmapple.h"

/* Android common kernel include/uapi/linux/gunyah.h (android14-6.1). */
#define GH_IOCTL_TYPE 'G'
#define GH_CREATE_VM _IO(GH_IOCTL_TYPE, 0x0)
struct gh_userspace_memory_region { uint32_t label, flags; uint64_t guest_phys_addr, memory_size, userspace_addr; };
#define GH_VM_SET_USER_MEM_REGION _IOW(GH_IOCTL_TYPE, 0x1, struct gh_userspace_memory_region)
struct gh_vm_dtb_config { uint64_t guest_phys_addr, size; };
#define GH_VM_SET_DTB_CONFIG _IOW(GH_IOCTL_TYPE, 0x2, struct gh_vm_dtb_config)
#define GH_VM_START _IO(GH_IOCTL_TYPE, 0x3)
struct gh_fn_desc { uint32_t type, arg_size; uint64_t arg; };
#define GH_VM_ADD_FUNCTION _IOW(GH_IOCTL_TYPE, 0x4, struct gh_fn_desc)
#define GH_VCPU_RUN _IO(GH_IOCTL_TYPE, 0x5)
#define GH_VCPU_MMAP_SIZE _IO(GH_IOCTL_TYPE, 0x6)
#define GH_VM_ANDROID_LEND_USER_MEM _IOW('A', 0x11, struct gh_userspace_memory_region)
#define GH_FN_VCPU 1
#define GH_FN_IRQFD 2
struct gh_fn_irqfd_arg { uint32_t fd, label, flags, padding; };
#define GH_IRQFD_FLAGS_LEVEL 1u
#define GH_MEM_R 1u
#define GH_MEM_W 2u
#define GH_MEM_X 4u
enum { GH_VCPU_EXIT_UNKNOWN, GH_VCPU_EXIT_MMIO, GH_VCPU_EXIT_STATUS };
struct gh_vcpu_run {
    uint8_t immediate_exit, padding[7];
    uint32_t exit_reason;
    union {
        struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
        struct { uint32_t status; uint16_t type, pad; uint32_t reason_size; uint8_t reason[8]; } status;
    };
};

/* DroidVM's gunyah_host_share module (Droid-VM/gunyah_host_mod, GKI6.1/uapi_gunyah_share.h, GPL-2.0). */
struct ghsm_share_blob { int32_t vm_fd; uint32_t label, flags, mem_handle; uint64_t guest_phys_addr, memory_size, userspace_addr; };
#define GHSM_SHARE_BLOB _IOWR(0x47, 0x14, struct ghsm_share_blob)

#define BOOT_PA 0x6fe00000ull   /* the VM's own region: just under the Mac's RAM (the tablet takes it, 0.3.74) */
#define BOOT_SIZE 0x200000ull
/* The tree's region: the second megabyte, as the probe has it (its 12 pieces were all accepted; 0.3.78's tree region of
   1.9 MB from 64 KB had the second piece refused, NORESOURCE: the resource manager's own room in the VM's region?). */
#define DTB_OFF 0x100000ull
/* The shim's reads and reports come here: 20 KB with nothing behind them, beside vmapple's UART (0x20010000, where
   the probe's report page has always exited); 0.3.77's 0x6f000000, just under the VM's base, never exited once. */
#define HANDOFF_PA 0x20020000ull
#define SHARE_CHUNK (256ull << 20)  /* a piece of 2 MB pages */
#define SMALL_CHUNK (8ull << 20)    /* a piece of 4 KB pages: 8 MB of them shared fine, 64 MB reset the tablet (0.3.75) */
#define MAX_PIECES 640
#define SPI_LOW_FIRST 32        /* UART, RTC, GPIO, AES, graphics: INTIDs 33..63 */
#define SPI_LOW_LAST 67         /* and PCIe INTx 64..67 */
#define NIRQ 160
#define APPLE_MIDR 0x611f0220ull /* what Henge's JIT answers (core/sysreg.c MIDR) */

extern void (*hv_vm_stop_hook)(void);
void hv_host_cpu_thread(void); /* the core this thread's CPU has (android/runner.c; a no-op elsewhere) */
extern const uint8_t hv_gunyah_shim_start[], hv_gunyah_shim_end[], hv_gunyah_shim_params[];

static HvMachine *M;
static HvVmapple *B;
static int vm_fd = -1, dev_kick = -1, nvcpu;
static int vcpu_fd[HV_MAX_CPUS];
static struct gh_vcpu_run *run[HV_MAX_CPUS];
static volatile int stop_req, vm_reason = -1;
static pthread_t vcpu_thread_id[HV_MAX_CPUS];
static volatile uint64_t mmio_exits[HV_MAX_CPUS];
static int irq_fd[NIRQ];
static volatile uint8_t irq_level[NIRQ];
static uint64_t handoff[2048];  /* the hand-over pages, as the shim reads them (16 KB) */
static uint64_t shim_rep[512];  /* its reports (+0x4000) */
static volatile int mac_runs;   /* the shim jumped to the Mac */

/* ---- a minimal flattened device tree writer ---- */

typedef struct { uint8_t s[60000]; size_t n; char str[4096]; size_t ns; } Fdt;

static void be32(Fdt *f, uint32_t v) { f->s[f->n++] = v >> 24; f->s[f->n++] = v >> 16; f->s[f->n++] = v >> 8; f->s[f->n++] = v; }
static void pad4(Fdt *f) { while (f->n & 3) f->s[f->n++] = 0; }
static void begin(Fdt *f, const char *name) {
    be32(f, 1);
    size_t l = strlen(name) + 1;
    memcpy(f->s + f->n, name, l);
    f->n += l;
    pad4(f);
}
static void end(Fdt *f) { be32(f, 2); }
static uint32_t stroff(Fdt *f, const char *name) {
    for (size_t i = 0; i < f->ns; i += strlen(f->str + i) + 1)
        if (!strcmp(f->str + i, name)) return (uint32_t)i;
    size_t l = strlen(name) + 1, at = f->ns;
    memcpy(f->str + f->ns, name, l);
    f->ns += l;
    return (uint32_t)at;
}
static void prop(Fdt *f, const char *name, const void *v, size_t len) {
    be32(f, 3);
    be32(f, (uint32_t)len);
    be32(f, stroff(f, name));
    if (len) memcpy(f->s + f->n, v, len);
    f->n += len;
    pad4(f);
}
static void prop_str(Fdt *f, const char *name, const char *v) { prop(f, name, v, strlen(v) + 1); }
static void prop_u32s(Fdt *f, const char *name, const uint32_t *v, int count) {
    uint8_t b[64 * 4];
    for (int i = 0; i < count; i++) b[4 * i] = v[i] >> 24, b[4 * i + 1] = v[i] >> 16, b[4 * i + 2] = v[i] >> 8, b[4 * i + 3] = v[i];
    prop(f, name, b, (size_t)count * 4);
}
static void prop_u32(Fdt *f, const char *name, uint32_t v) { prop_u32s(f, name, &v, 1); }

static int irq_wanted(int intid) {
    return (intid >= SPI_LOW_FIRST + 1 && intid <= SPI_LOW_LAST) ||
           (intid >= VMAPPLE_MSI_BASE && intid < VMAPPLE_MSI_BASE + VMAPPLE_MSI_COUNT);
}
static int irq_is_msi(int intid) { return intid >= VMAPPLE_MSI_BASE; }

/* The VM's configuration for Gunyah's resource manager (as android/tools/gunyah_probe.c has it, which the tablet
   takes): the VM's own region as its memory, room above it for the Mac's RAM (size-max), a channel to the resource
   manager for the shim's MEM_ACCEPTs, the GIC at vmapple's addresses, a doorbell per interrupt line. */
static size_t build_dtb(uint8_t *out, size_t cap, int ncpus, uint64_t size_max) {
    static Fdt f;
    memset(&f, 0, sizeof f);
    const uint32_t GIC = 1;
    begin(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "linux,dummy-virt");
    prop_u32(&f, "interrupt-parent", GIC);
    /* One memory node, named `memory` and nothing more: the resource manager fails the tree for any other. */
    begin(&f, "memory");
    prop_str(&f, "device_type", "memory");
    uint32_t boot_reg[4] = {(uint32_t)(BOOT_PA >> 32), (uint32_t)BOOT_PA, 0, (uint32_t)BOOT_SIZE};
    prop_u32s(&f, "reg", boot_reg, 4);
    end(&f);
    begin(&f, "chosen"); /* the resource manager adds to it, and refuses its changes whole if it is not there */
    end(&f);
    begin(&f, "cpus");
    prop_u32(&f, "#address-cells", 1);
    prop_u32(&f, "#size-cells", 0);
    for (int i = 0; i < ncpus; i++) {
        char n[16];
        snprintf(n, sizeof n, "cpu@%d", i);
        begin(&f, n);
        prop_str(&f, "device_type", "cpu");
        prop_str(&f, "compatible", "arm,armv8");
        prop_u32(&f, "reg", (uint32_t)i);
        prop_str(&f, "enable-method", "psci");
        end(&f);
    }
    end(&f);
    begin(&f, "psci");
    prop_str(&f, "compatible", "arm,psci-1.0");
    prop_str(&f, "method", "hvc");
    end(&f);
    begin(&f, "intc@10000000");
    prop_str(&f, "compatible", "arm,gic-v3");
    prop_u32(&f, "#interrupt-cells", 3);
    prop(&f, "interrupt-controller", NULL, 0);
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    uint32_t gic[8] = {0, (uint32_t)VMAPPLE_GIC_DIST, 0, 0x10000, 0, (uint32_t)VMAPPLE_GIC_REDIST, 0, 0x20000u * (uint32_t)ncpus};
    prop_u32s(&f, "reg", gic, 8);
    prop_u32(&f, "phandle", GIC);
    end(&f);
    begin(&f, "timer");
    prop_str(&f, "compatible", "arm,armv8-timer");
    uint32_t ppis[12] = {1, 13, 0x104, 1, 14, 0x104, 1, 11, 0x104, 1, 10, 0x104};
    prop_u32s(&f, "interrupts", ppis, 12);
    prop(&f, "always-on", NULL, 0);
    end(&f);
    begin(&f, "gunyah-vm-config");
    prop_str(&f, "image-name", "henge-vmapple");
    prop_str(&f, "os-type", "linux");
    begin(&f, "memory");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    uint32_t base[2] = {(uint32_t)(BOOT_PA >> 32), (uint32_t)BOOT_PA};
    prop_u32s(&f, "base-address", base, 2);
    prop_u32(&f, "size-max", (uint32_t)size_max); /* one cell, as the probe's (the node above says none) */
    end(&f);
    begin(&f, "interrupts");
    prop_u32(&f, "config", GIC);
    end(&f);
    begin(&f, "vcpus");
    prop_str(&f, "affinity", "proxy");
    end(&f);
    begin(&f, "vdevices");
    prop_str(&f, "generate", "/hypervisor");
    begin(&f, "rm-rpc"); /* the guest's own channel to the resource manager: the shim's MEM_ACCEPTs */
    prop_str(&f, "vdevice-type", "rm-rpc");
    prop_str(&f, "generate", "/hypervisor/qcom,resource-mgr");
    prop_u32(&f, "label", 0);
    prop(&f, "console-dev", NULL, 0);
    prop_u32(&f, "message-size", 240);
    prop_u32(&f, "queue-depth", 8);
    end(&f);
    for (int id = 0; id < NIRQ; id++) {
        if (!irq_wanted(id) || (getenv("HV_GUNYAH_BELLS") && *getenv("HV_GUNYAH_BELLS") == '0')) continue;
        char n[32], path[48];
        snprintf(n, sizeof n, "bell-%x", id);
        snprintf(path, sizeof path, "/hypervisor/bell-%x", id);
        begin(&f, n);
        prop_str(&f, "vdevice-type", "doorbell");
        prop_str(&f, "generate", path);
        prop_u32(&f, "label", (uint32_t)id);
        prop(&f, "peer-default", NULL, 0);
        uint32_t irq[3] = {0, (uint32_t)(id - 32), irq_is_msi(id) ? 1u : 4u}; /* SPI, edge for MSIs, level otherwise */
        prop_u32s(&f, "interrupts", irq, 3);
        end(&f);
    }
    end(&f);
    end(&f);
    end(&f);
    be32(&f, 9);

    size_t hdr = 40, rsv = 16, total = hdr + rsv + f.n + f.ns;
    if (total > cap) return 0;
    uint32_t h[10] = {0xd00dfeed, (uint32_t)total, (uint32_t)(hdr + rsv), (uint32_t)(hdr + rsv + f.n), (uint32_t)hdr,
                      17, 16, 0, (uint32_t)f.ns, (uint32_t)f.n};
    for (int i = 0; i < 10; i++) out[4 * i] = h[i] >> 24, out[4 * i + 1] = h[i] >> 16, out[4 * i + 2] = h[i] >> 8, out[4 * i + 3] = h[i];
    memset(out + hdr, 0, rsv);
    memcpy(out + hdr + rsv, f.s, f.n);
    memcpy(out + hdr + rsv + f.n, f.str, f.ns);
    return total;
}

/* ---- interrupts ---- */

static void raise_line(int intid) {
    if (intid < 0 || intid >= NIRQ || irq_fd[intid] < 0) return;
    uint64_t one = 1;
    if (write(irq_fd[intid], &one, sizeof one) < 0) { /* the counter is full: it is pending anyway */ }
}

/* Henge's GIC entry points come here while Gunyah owns the GIC. */
static void gic_external(int intid, int level, int edge) {
    static int warned[NIRQ];
    if (intid < 32) return; /* PPIs (Henge's emulated timer): Gunyah has the real ones */
    if (intid >= NIRQ || irq_fd[intid] < 0) {
        if (intid < NIRQ && !warned[intid]++) fprintf(stderr, "gunyah: interrupt %d has no doorbell\n", intid);
        return;
    }
    if (edge) { raise_line(intid); return; }
    uint8_t was = irq_level[intid];
    irq_level[intid] = level != 0;
    if (level && !was) raise_line(intid);
}

/* ---- the paravirtualized-PAC rewrite ---- */

#define HVC0 0xd4000002u
#define CBNZ_X0_SELF 0xb5000000u
#define MOV_X0_0 0xd2800000u
#define NOP 0xd503201fu

static uint64_t pac_patched;

static void pac_scan(void) {
    for (int r = 0; r < M->nram; r++) {
        uint32_t *w = (uint32_t *)M->ram[r].host;
        size_t n = M->ram[r].size / 4;
        for (size_t i = 0; i + 1 < n; i++) {
            if (w[i] != HVC0 || w[i + 1] != CBNZ_X0_SELF) continue;
            /* The cbnz first: a CPU already spinning on it leaves at once, with x0 whatever it is. */
            __atomic_store_n(&w[i + 1], NOP, __ATOMIC_RELEASE);
            __atomic_store_n(&w[i], MOV_X0_0, __ATOMIC_RELEASE);
            __builtin___clear_cache((char *)&w[i], (char *)&w[i + 2]);
            if (pac_patched++ < 8)
                fprintf(stderr, "gunyah: PAC hypercall at pa %llx rewritten\n",
                        (unsigned long long)(M->ram[r].pa + i * 4));
        }
    }
}

static void *pac_thread(void *arg) {
    (void)arg;
    uint64_t reported = pac_patched;
    while (!stop_req) {
        /* The kernel's own calls were rewritten before the start (kpatch); this is for code loaded later (the
           auxiliary kernel collection), a pass over all of the Mac's RAM every 30 s. */
        for (int i = 0; i < 300 && !stop_req; i++) usleep(100000);
        if (stop_req) break;
        pac_scan();
        if (pac_patched != reported) {
            fprintf(stderr, "gunyah: %llu PAC hypercalls rewritten so far\n", (unsigned long long)pac_patched);
            reported = pac_patched;
        }
    }
    return NULL;
}

/* ---- devices ---- */

static void *device_thread(void *arg) {
    (void)arg;
    HvCpu *cpu0 = hv_vm_cpu(0);
    struct pollfd p = {.fd = dev_kick, .events = POLLIN};
    uint64_t n = 0;
    while (!stop_req) {
        if (poll(&p, 1, 1) > 0) { uint64_t v; if (read(dev_kick, &v, sizeof v) < 0) { } }
        if (M->tick) M->tick(M, cpu0);
        /* Lines a device still holds high: ring again, in case the guest finished with the last ring. */
        if ((++n & 1) == 0)
            for (int i = 32; i < NIRQ; i++)
                if (irq_level[i] && !irq_is_msi(i)) raise_line(i);
    }
    return NULL;
}

static void kick_devices(void) {
    uint64_t one = 1;
    if (write(dev_kick, &one, sizeof one) < 0) { }
}

/* ---- the kernel, made to run on this CPU (before the VM starts) ---- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* boot_args, as iBoot left it for the kernel (vm.c's display scan checks the same fields): its RAM offset, or -1. */
static int64_t find_boot_args(void) {
    uint64_t limit = B->ram_size < (1ull << 30) ? B->ram_size : 1ull << 30;
    for (uint64_t off = 0; off + 0x1000 <= limit; off += 0x4000) {
        const uint8_t *p = B->ram + off;
        uint16_t rev, ver;
        memcpy(&rev, p, 2), memcpy(&ver, p + 2, 2);
        if (rev < 2 || rev > 3 || ver < 1 || ver > 3) continue;
        uint64_t virt = rd64(p + 8), phys = rd64(p + 16), mem = rd64(p + 24);
        if (virt >> 40 != 0xfffffe || phys < VMAPPLE_MEM || phys >= VMAPPLE_MEM + B->ram_size || !mem || mem > B->ram_size) continue;
        return (int64_t)off;
    }
    return -1;
}

static struct { uint64_t pa, size; } xr[64]; /* the kernel collection's code, by physical address */
static uint64_t k_virt, k_phys; /* the kernel's static mapping: virtual k_virt is physical k_phys */
static uint64_t k_top;          /* physical end of the kernel's data (boot_args topOfKernelData) */
static int nxr;

static void add_exec(uint64_t pa, uint64_t size) {
    if (pa < VMAPPLE_MEM || pa >= VMAPPLE_MEM + B->ram_size) return;
    if (size > VMAPPLE_MEM + B->ram_size - pa) size = VMAPPLE_MEM + B->ram_size - pa;
    for (int i = 0; i < nxr; i++)
        if (pa < xr[i].pa + xr[i].size && pa + size > xr[i].pa) return; /* already (a fileset entry's part of a segment) */
    if (nxr < 64) xr[nxr].pa = pa, xr[nxr].size = size, nxr++;
}

/* A Mach-O header at hdr_pa whose __TEXT is at base_vm: its executable segments, and its fileset entries'. */
static void exec_segments(uint64_t hdr_pa, uint64_t base_vm, int depth) {
    const uint8_t *h = hv_phys_ptr(M, hdr_pa, 32);
    if (!h || rd32(h) != 0xfeedfacfu) return;
    uint32_t ncmds = rd32(h + 16), sizeofcmds = rd32(h + 20);
    const uint8_t *lc = hv_phys_ptr(M, hdr_pa + 32, sizeofcmds);
    if (!lc || sizeofcmds > (4u << 20)) return;
    for (uint32_t i = 0, o = 0; i < ncmds && o + 8 <= sizeofcmds; i++) {
        uint32_t cmd = rd32(lc + o), size = rd32(lc + o + 4);
        if (size < 8 || o + size > sizeofcmds) break;
        if (cmd == 0x19 && size >= 72) { /* LC_SEGMENT_64 */
            uint64_t vmaddr = rd64(lc + o + 24), vmsize = rd64(lc + o + 32);
            uint32_t initprot = rd32(lc + o + 60);
            if ((initprot & 4) && vmsize && vmaddr >= base_vm) add_exec(hdr_pa + (vmaddr - base_vm), vmsize);
        } else if (cmd == 0x80000035u && size >= 24 && depth == 0) { /* LC_FILESET_ENTRY */
            uint64_t vmaddr = rd64(lc + o + 8);
            if (vmaddr >= base_vm) exec_segments(hdr_pa + (vmaddr - base_vm), base_vm, 1);
        }
        o += size;
    }
}

#define MOVZ_X(rt) (0xd2800000u | (rt))
#define SCTLR_W 0xd5181000u /* MSR SCTLR_EL1, Xt */
#define MOVK_X(rt, imm16, hw) (0xf2800000u | (uint32_t)(hw) << 21 | (uint32_t)(imm16) << 5 | (rt))
#define MOVZ_XS(rt, imm16, hw) (0xd2800000u | (uint32_t)(hw) << 21 | (uint32_t)(imm16) << 5 | (rt))

static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3ffffff); }

/* The patches. Returns 0, or -1 when the kernel was not found (the hand-over does not happen). */
static int kpatch(void) {
    int64_t ba = find_boot_args();
    if (ba < 0) { fprintf(stderr, "gunyah: no boot_args in the Mac's RAM: this is not the jump into the kernel\n"); return -1; }
    const uint8_t *b = B->ram + ba;
    uint64_t virt = rd64(b + 8), phys = rd64(b + 16), top = rd64(b + 32), dtva = rd64(b + 96);
    k_virt = virt, k_phys = phys, k_top = top;
    uint32_t dtlen = rd32(b + 104);
    fprintf(stderr, "gunyah: boot_args at pa %#llx: kernel at virtual %#llx = physical %#llx, its data to %#llx, device tree "
                    "at %#llx (%u bytes)\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)ba), (unsigned long long)virt,
            (unsigned long long)phys, (unsigned long long)top, (unsigned long long)dtva, dtlen);
    /* The kernel collection's header: the first arm64 Mach-O (a fileset, or a plain kernel) from physBase on. */
    uint64_t hdr = 0;
    for (uint64_t pa = phys & ~0x3fffull; pa < phys + (256ull << 20) && pa + 32 <= VMAPPLE_MEM + B->ram_size; pa += 0x4000) {
        const uint8_t *h = hv_phys_ptr(M, pa, 32);
        if (h && rd32(h) == 0xfeedfacfu && rd32(h + 4) == 0x0100000cu && (rd32(h + 12) == 12 || rd32(h + 12) == 2)) { hdr = pa; break; }
    }
    if (!hdr) { fprintf(stderr, "gunyah: the kernel's Mach-O header was not found past %#llx\n", (unsigned long long)phys); return -1; }
    /* __TEXT (file offset 0) is where the header is: every segment's place follows from it. */
    uint64_t base_vm = 0;
    {
        const uint8_t *h = hv_phys_ptr(M, hdr, 32);
        uint32_t ncmds = rd32(h + 16), sizeofcmds = rd32(h + 20);
        const uint8_t *lc = hv_phys_ptr(M, hdr + 32, sizeofcmds);
        for (uint32_t i = 0, o = 0; lc && i < ncmds && o + 8 <= sizeofcmds; i++) {
            uint32_t cmd = rd32(lc + o), size = rd32(lc + o + 4);
            if (size < 8) break;
            if (cmd == 0x19 && size >= 72 && rd64(lc + o + 40) == 0 && rd64(lc + o + 48)) { base_vm = rd64(lc + o + 24); break; }
            o += size;
        }
    }
    if (!base_vm) { fprintf(stderr, "gunyah: the kernel's __TEXT was not found\n"); return -1; }
    exec_segments(hdr, base_vm, 0);
    uint64_t code = 0;
    for (int i = 0; i < nxr; i++) code += xr[i].size;
    fprintf(stderr, "gunyah: kernel collection at pa %#llx (%s, __TEXT at %#llx): %d code ranges, %llu MB\n", (unsigned long long)hdr,
            rd32(hv_phys_ptr(M, hdr, 32) + 12) == 12 ? "fileset" : "kernel", (unsigned long long)base_vm, nxr,
            (unsigned long long)(code >> 20));

    uint64_t impl_r = 0, impl_w = 0, midr = 0, midr_left = 0, pac = 0, sctlr = 0;
    const char *kp = getenv("HV_GUNYAH_KEEP_PAC");
    int keep_pac = kp && *kp == '1';
    for (int r = 0; r < nxr; r++) {
        uint32_t *w = hv_phys_ptr(M, xr[r].pa, xr[r].size);
        if (!w) continue;
        uint64_t n = xr[r].size / 4;
        /* The stub pool: the zeros at the code's end (page padding), less a margin. */
        uint64_t z = n;
        while (z > 0 && w[z - 1] == 0) z--;
        uint64_t pool = z + 16 < n ? z + 16 : n;
        for (uint64_t i = 0; i < z; i++) {
            uint32_t x = w[i];
            if (x == HVC0 && i + 1 < n && w[i + 1] == CBNZ_X0_SELF) {
                w[i] = MOV_X0_0, w[i + 1] = NOP, pac++;
                continue;
            }
            uint32_t top13 = x & 0xfff80000u, crn = (x >> 12) & 15, rt = x & 31;
            if (top13 == 0xd5380000u && (x & ~31u) == 0xd5380000u) { /* MRS Xt, MIDR_EL1 */
                if (pool + 3 > n || (int64_t)((pool - i) * 4) >= (128ll << 20)) { midr_left++; continue; }
                uint64_t site = xr[r].pa + i * 4, stub = xr[r].pa + pool * 4;
                w[pool] = MOVZ_XS(rt, (APPLE_MIDR >> 16) & 0xffff, 1);
                w[pool + 1] = MOVK_X(rt, APPLE_MIDR & 0xffff, 0);
                w[pool + 2] = branch(stub + 8, site + 4);
                w[i] = branch(site, stub);
                pool += 3, midr++;
                continue;
            }
            /* 0.3.85: the panic was ESR 0x72000002, a PAC failure (EC 0x1c, DA key) at AUTDA x2, x3: this CPU has FEAT_FPAC,
               so a failed AUT* faults at once, and the kernel's keys are what Apple's hypervisor would set through the
               PAC hypercalls (now answered "success" with nothing done). Every MSR SCTLR_EL1 goes through a stub that
               clears EnIA, EnIB, EnDA and EnDB first: all PAC and AUT instructions are then no-ops, signing and checking alike, so the
               kernel's own checks (XPAC and compare) pass. The Mac runs without pointer authentication. */
            if ((x & ~31u) == SCTLR_W && rt != 31 && !keep_pac) {
                if (pool + 5 > n || (int64_t)((pool - i) * 4) >= (128ll << 20)) { midr_left++; continue; }
                uint64_t site = xr[r].pa + i * 4, stub = xr[r].pa + pool * 4;
                w[pool] = 0x9260f400u | rt << 5 | rt;     /* and xt, xt, #~(EnIA|EnIB) */
                w[pool + 1] = 0x9264f800u | rt << 5 | rt; /* and xt, xt, #~EnDA */
                w[pool + 2] = 0x9272f800u | rt << 5 | rt; /* and xt, xt, #~EnDB */
                w[pool + 3] = x;                          /* msr sctlr_el1, xt */
                w[pool + 4] = branch(stub + 16, site + 4);
                w[i] = branch(site, stub);
                pool += 5, sctlr++;
                continue;
            }
            if ((top13 == 0xd5380000u || top13 == 0xd5180000u) && (crn == 15 || crn == 11)) { /* implementation defined */
                if (top13 == 0xd5380000u) w[i] = rt == 31 ? NOP : MOVZ_X(rt), impl_r++;
                else w[i] = NOP, impl_w++;
            }
        }
    }
    pac_patched += pac;
    fprintf(stderr, "gunyah: kernel patched for this CPU: %llu reads and %llu writes of Apple's own registers (now 0 and "
                    "ignored), %llu MIDR reads (now Apple's %#llx)%s, %llu PAC hypercalls (now success), %llu SCTLR writes (pointer "
                    "authentication off)\n",
            (unsigned long long)impl_r, (unsigned long long)impl_w, (unsigned long long)midr, (unsigned long long)APPLE_MIDR,
            midr_left ? " (some left: no room for their stubs)" : "", (unsigned long long)pac, (unsigned long long)sctlr);

    /* The timebase: the kernel's tree says the 24 MHz Henge's JIT counts; Gunyah gives the CPU's own counter. */
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    int changed = 0;
    if (dtva >= virt && dtlen > 8) {
        uint64_t dtpa = dtva - virt + phys;
        uint8_t *dt = hv_phys_ptr(M, dtpa, dtlen);
        if (dt) {
            /* Apple's device tree: a node is {u32 properties, u32 children}, then each property {char name[32],
               u32 length (bit 31: a placeholder), value padded to 4}, then its children. */
            uint8_t *p = dt, *end = dt + dtlen;
            uint32_t stack[64];
            int depth = 0;
            stack[0] = 1;
            while (depth >= 0 && p + 8 <= end) {
                if (!stack[depth]) { depth--; continue; }
                stack[depth]--;
                uint32_t np = rd32(p), nc = rd32(p + 4);
                p += 8;
                for (uint32_t i = 0; i < np && p + 36 <= end; i++) {
                    uint32_t len = rd32(p + 32) & 0x7fffffffu;
                    if (p + 36 + len > end) { p = end; break; }
                    if (!strncmp((const char *)p, "timebase-frequency", 32) && (len == 4 || len == 8)) {
                        uint32_t v = rd32(p + 36);
                        if (v != (uint32_t)f) {
                            uint32_t nv = (uint32_t)f;
                            memcpy(p + 36, &nv, 4);
                            if (len == 8) memset(p + 40, 0, 4);
                            changed++;
                        }
                    }
                    p += 36 + ((len + 3) & ~3u);
                }
                if (nc && depth < 63) stack[++depth] = nc;
            }
        }
    }
    fprintf(stderr, "gunyah: the kernel's timebase-frequency: %d set to the CPU's %llu Hz\n", changed, (unsigned long long)f);
    return 0;
}

/* ---- the Mac's RAM: all of it in 2 MB pages, or the hand-over does not happen ---- */

static uint64_t huge_mb(void *p, uint64_t len) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return 0;
    char line[256];
    int in = 0;
    uint64_t kb = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char *dash = strchr(line, '-'), *sp = strchr(line, ' ');
        if (dash && sp && dash < sp && sscanf(line, "%lx-%lx ", &lo, &hi) == 2) {
            in = lo < (uintptr_t)p + len && hi > (uintptr_t)p;
            continue;
        }
        unsigned long v;
        if (in && (sscanf(line, "ShmemPmdMapped: %lu kB", &v) == 1 || sscanf(line, "FilePmdMapped: %lu kB", &v) == 1 ||
                   sscanf(line, "AnonHugePages: %lu kB", &v) == 1))
            kb += v;
    }
    fclose(f);
    return kb >> 10;
}

static void put_file(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)!write(fd, v, strlen(v));
    close(fd);
}

static void classify(uint8_t *kind, uint64_t nchunks);
static int ram_ready(void) {
    uint64_t mb = B->ram_size >> 20, two = 2ull << 20;
    if ((uintptr_t)B->ram & (two - 1)) {
        fprintf(stderr, "gunyah: the Mac's RAM is not mapped on a 2 MB boundary: no 2 MB pages\n");
        return -1;
    }
    /* Present and pinned first: pagemap shows frames only for pages this process has mapped. 0.3.76 counted the
       mapping's 2 MB entries (smaps) instead: 844 MB of a reserve that was all 2 MB pages. */
    struct rlimit nolimit = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &nolimit);
    mlock(B->ram, B->ram_size);
    uint64_t nch = B->ram_size >> 21, h = 0;
    uint8_t *kind = calloc(nch ? nch : 1, 1);
    if (kind) {
        classify(kind, nch);
        for (uint64_t c = 0; c < nch; c++) h += kind[c] ? 2 : 0;
        free(kind);
    }
    fprintf(stderr, "gunyah: the Mac's RAM: %llu of %llu MB in 2 MB pages%s\n", (unsigned long long)h, (unsigned long long)mb,
            B->ram_reserved ? " (reserved at boot)" : "");
    if (h < mb && !B->ram_reserved) {
        /* What is small, collapsed into 2 MB pages now (its contents copied), memory compacted before each pass. */
        put_file("/sys/kernel/mm/transparent_hugepage/shmem_enabled", "advise");
        put_file("/sys/kernel/mm/transparent_hugepage/defrag", "madvise");
        madvise(B->ram, B->ram_size, MADV_HUGEPAGE);
        for (int pass = 0; pass < 3 && h < mb; pass++) {
            put_file("/proc/sys/vm/drop_caches", "3");
            put_file("/proc/sys/vm/compact_memory", "1");
            for (uint64_t o = 0; o < B->ram_size; o += two) madvise(B->ram + o, two, 25 /* MADV_COLLAPSE */);
            kind = calloc(nch ? nch : 1, 1);
            h = 0;
            if (kind) {
                classify(kind, nch);
                for (uint64_t c = 0; c < nch; c++) h += kind[c] ? 2 : 0;
                free(kind);
            }
            fprintf(stderr, "gunyah:   after collapsing: %llu MB in 2 MB pages\n", (unsigned long long)h);
        }
    }
    /* 0.3.76: what is not in 2 MB pages is shared in 8 MB pieces of 4 KB pages (the tablet took 8 MB of them;
       64 MB in one piece reset it). HV_GUNYAH_MIN_HUGE=percent: refuse below it (default 0). */
    const char *min = getenv("HV_GUNYAH_MIN_HUGE");
    uint64_t need = mb * (uint64_t)(min && *min ? atoi(min) : 0) / 100;
    if (h < need) {
        fprintf(stderr, "gunyah: only %llu of %llu MB in 2 MB pages, under HV_GUNYAH_MIN_HUGE: not handed over\n",
                (unsigned long long)h, (unsigned long long)mb);
        return -1;
    }
    return 0;
}

/* Which 2 MB pieces of the Mac's RAM are one 2 MB page each (1) or 4 KB pages (0): their first page's frame from
   /proc/self/pagemap (root sees frames), a transparent huge page there by /proc/kpageflags (KPF_THP), whole and
   aligned. Without either file every piece counts as 4 KB pages: shared 8 MB at a time, which is safe. */
static uint64_t pfn_of(int pm, const uint8_t *va) {
    uint64_t e = 0;
    if (pread(pm, &e, 8, (off_t)(((uintptr_t)va >> 12) * 8)) != 8 || !(e >> 63)) return 0;
    return e & ((1ull << 55) - 1);
}
static void classify(uint8_t *kind, uint64_t nchunks) {
    memset(kind, 0, nchunks);
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC), kf = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
    if (pm < 0 || kf < 0) {
        fprintf(stderr, "gunyah: page frames unreadable (%s): every piece shared as 4 KB pages\n", strerror(errno));
        if (pm >= 0) close(pm);
        if (kf >= 0) close(kf);
        return;
    }
    for (uint64_t c = 0; c < nchunks; c++) {
        const uint8_t *va = B->ram + (c << 21);
        uint64_t first = pfn_of(pm, va), last = pfn_of(pm, va + (2u << 20) - 4096), flags = 0;
        if (!first || (first & 511) || last != first + 511) continue;
        if (pread(kf, &flags, 8, (off_t)(first * 8)) == 8 && (flags >> 22 & 1)) kind[c] = 1;
    }
    close(pm);
    close(kf);
}

/* ---- the hand-over page ---- */

static void fill_state(const HvCpu *c) {
    uint64_t *s = handoff + 64;
    for (int i = 0; i < 31; i++) s[i] = c->x[i];
    /* SP: the JIT keeps the current one in sp and banks the other. */
    s[31] = c->spsel ? c->sp : c->sp_el1;  /* SP_EL1 */
    s[45] = c->spsel ? c->sp_el0 : c->sp;  /* SP_EL0 */
    s[32] = c->pc;
    s[33] = (c->nzcv & 0xf0000000ull) | (c->daif & 0x3c0ull) | (c->pstate_extra & ((1ull << 22) | (1ull << 23) | (1ull << 12))) |
            ((uint64_t)c->el << 2) | (c->spsel ? 1 : 0);
    s[34] = c->sctlr_el1 & ~(uint64_t)(0xC0000000u | 1u << 27 | 1u << 13), s[35] = c->ttbr0_el1, s[36] = c->ttbr1_el1, s[37] = c->tcr_el1, s[38] = c->mair_el1;
    s[39] = c->amair_el1, s[40] = c->vbar_el1, s[41] = c->contextidr_el1, s[42] = c->tpidr_el0, s[43] = c->tpidrro_el0;
    s[44] = c->tpidr_el1, s[46] = c->cpacr_el1, s[47] = c->cntkctl_el1, s[48] = c->fpcr, s[49] = c->fpsr, s[50] = c->mdscr_el1;
    fprintf(stderr, "gunyah: CPU 0 goes on at pc %#llx (EL%u, MMU %s), x0 %#llx x1 %#llx x2 %#llx x3 %#llx, SP_EL1 %#llx\n",
            (unsigned long long)c->pc, c->el, c->sctlr_el1 & 1 ? "on" : "off", (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)s[31]);
}

/* A line forced to storage: the tablet reset in 0.3.76 right after the vCPUs ran, and lines left in the page cache
   were lost with it. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    fsync(2);
}

static const char *phase_name(uint64_t p) {
    return p == 1 ? "started" : p == 2 ? "found its channel to the resource manager" : p == 3 ? "accepted the Mac's RAM"
         : p == 4 ? "loading CPU 0's state" : p == 0xdead ? "STOPPED" : "?";
}

/* A store of the shim's to the page's second half. */
static void shim_report(uint64_t off, uint64_t v) {
    unsigned i = (unsigned)(off / 8) & 511;
    shim_rep[i] = v;
    if (off == 0xff8) { /* +0x4ff8: the jump */
        mac_runs = 1;
        say("gunyah: the Mac runs on Gunyah now (CPU 0 at pc %#llx)\n", (unsigned long long)handoff[64 + 32]);
        return;
    }
    if (i == 8 && v < 640) say("gunyah: shim: accepting piece %llu (%#llx, %llu MB)\n", (unsigned long long)v,
                               (unsigned long long)handoff[129 + 3 * v], (unsigned long long)(handoff[130 + 3 * v] >> 20));
    if (i == 10 && v && shim_rep[16] % 16 == 0) say("gunyah: shim: the resource manager's answer: error %#llx%s\n",
                                                   (unsigned long long)v, v == 6 ? " (ARGUMENT_INVALID)" : v == 2 ? " (NORESOURCE)" : "");
    if (i == 16 && v % 16 == 0) say("gunyah: shim: still refused after %llu tries (one each 50 ms; 64 at most)\n", (unsigned long long)v);
    if (i == 0) {
        say("gunyah: shim: %s\n", phase_name(v));
        if (v == 2)
            say("gunyah: shim: the vCPU's MIDR %#llx, MPIDR %#llx (Henge's CPU 0: %#llx), ISAR1 %#llx, SCTLR %#llx; "
                            "PAC key read %s%s%llx\n", (unsigned long long)shim_rep[1], (unsigned long long)shim_rep[2],
                    (unsigned long long)hv_vm_cpu(0)->mpidr, (unsigned long long)shim_rep[3], (unsigned long long)shim_rep[4],
                    shim_rep[15] ? "ran: APIAKeyLo " : "TRAPPED", shim_rep[15] ? "0x" : " ", shim_rep[15] ? (unsigned long long)shim_rep[14] : 0ull);
        if (v == 0xdead) {
            if (shim_rep[11] || shim_rep[12])
                say("gunyah: shim: an exception: ESR %#llx at %#llx (FAR %#llx)\n", (unsigned long long)shim_rep[11],
                        (unsigned long long)shim_rep[12], (unsigned long long)shim_rep[13]);
            else if (!shim_rep[5]) say("gunyah: shim: no channel to the resource manager in the VM's tree\n");
            else say("gunyah: shim: piece %llu not accepted: MSGQ_SEND %#llx, the resource manager's error %#llx\n",
                         (unsigned long long)shim_rep[8], (unsigned long long)shim_rep[9], (unsigned long long)shim_rep[10]);
            hv_gunyah_stop();
        }
    }
}

/* ---- the Mac's serial port, read for its panics ----
   0.3.81: XNU ran on Gunyah and panicked at once, "Unexpected debugger trap while SP1 selected at pc ...", ESR
   0x02000000: an instruction this CPU (or Gunyah) does not take, where Henge's JIT did. Each pc a panic line names is
   looked up in the Mac's RAM and the instruction words around it are logged, decoded where they are system
   instructions, so the next patch knows what to rewrite. */
static char uart_line[512];
static int uart_n;
static uint64_t pcs_seen[8];
static int npcs;

static void decode_word(uint32_t w, char *out, size_t cap) {
    if ((w & 0xffc00000u) == 0xd5000000u) { /* system: MSR/MRS/SYS/SYSL, hints, barriers */
        unsigned l = w >> 21 & 1, op0 = w >> 19 & 3, op1 = w >> 16 & 7, crn = w >> 12 & 15, crm = w >> 8 & 15, op2 = w >> 5 & 7, rt = w & 31;
        if (op0 >= 2) snprintf(out, cap, "%s S%u_%u_C%u_C%u_%u, x%u", l ? "MRS (read)" : "MSR (write)", op0, op1, crn, crm, op2, rt);
        else if (op0 == 1) snprintf(out, cap, "%s #%u, C%u, C%u, #%u, x%u", l ? "SYSL" : "SYS", op1, crn, crm, op2, rt);
        else snprintf(out, cap, "hint/barrier/pstate (op1 %u CRn %u CRm %u op2 %u)", op1, crn, crm, op2);
    } else if ((w & 0xffe0001fu) == 0xd4000002u) snprintf(out, cap, "HVC #%u", w >> 5 & 0xffff);
    else if ((w & 0xffe0001fu) == 0xd4000003u) snprintf(out, cap, "SMC #%u", w >> 5 & 0xffff);
    else if ((w & 0xffe0001fu) == 0xd4200000u) snprintf(out, cap, "BRK #%#x", w >> 5 & 0xffff);
    else if (w == 0xe7ffdeffu || w == 0xe7ffdefeu) snprintf(out, cap, "XNU's TRAP_DEBUGGER (panic/debugger entry, on purpose)");
    else if ((w & 0xfffff000u) == 0x00201000u) snprintf(out, cap, "Apple AMX/GXF space (0x0020xxxx)");
    else snprintf(out, cap, "-");
}

static void panic_pc(uint64_t pc) {
    for (int i = 0; i < npcs; i++) if (pcs_seen[i] == pc) return;
    if (npcs == 8 || !k_virt || pc < k_virt) return;
    pcs_seen[npcs++] = pc;
    uint64_t pa = pc - k_virt + k_phys;
    const uint32_t *w = hv_phys_ptr(M, (pa & ~3ull) - 16, 36);
    if (!w) { say("gunyah: the panic's pc %#llx is pa %#llx, not in the Mac's RAM\n", (unsigned long long)pc, (unsigned long long)pa); return; }
    say("gunyah: the panic's pc %#llx (pa %#llx), the instructions around it:\n", (unsigned long long)pc, (unsigned long long)pa);
    for (int i = 0; i < 9; i++) {
        char d[96];
        decode_word(w[i], d, sizeof d);
        say("gunyah:   %s%#llx: %08x  %s\n", i == 4 ? "=> " : "   ", (unsigned long long)(pc - 16 + 4 * (uint64_t)i), w[i], d);
    }
}

/* 0.3.83: 0.3.82 showed the "undefined instruction" is 0xe7ffdeff, XNU's own TRAP_DEBUGGER: panic() called
   DebuggerTrapWithState(DBOP_PANIC = 3 in x0, message in x1, the panic's format in x2, its va_list in x3) this early,
   still on SP1, and the trap handler can only say "Unexpected debugger trap" and lose the real panic text. The first
   register dump's x0..x3 are read off the serial line and the panic's text is rebuilt from the Mac's RAM. */
static int kva_ok(uint64_t va, size_t n, const void **p) {
    if (!k_virt || va < k_virt) return 0;
    *p = hv_phys_ptr(M, va - k_virt + k_phys, n);
    return *p != NULL;
}

static int kstr(uint64_t va, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        const char *p;
        if (!kva_ok(va + n, 1, (const void **)&p)) break;
        if (!*p) break;
        out[n++] = (*p >= 32 && *p < 127) || *p == '\n' || *p == '\t' ? *p : '?';
    }
    out[n] = 0;
    return n > 0;
}

static void kformat(uint64_t fmt_va, uint64_t args_va, char *out, size_t cap) {
    char fmt[3072];
    size_t o = 0;
    if (!kstr(fmt_va, fmt, sizeof fmt)) { snprintf(out, cap, "(format unreadable)"); return; }
    for (const char *f = fmt; *f && o + 1 < cap; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[32] = "%";
        size_t s = 1;
        f++;
        while (*f && strchr("-+ #0123456789.", *f) && s < 20) spec[s++] = *f++;
        while (*f && strchr("hlqzjt", *f)) f++;
        if (!*f) break;
        if (*f == '%') { out[o++] = '%'; continue; }
        uint64_t a = 0;
        const uint64_t *ap;
        if (kva_ok(args_va, 8, (const void **)&ap)) a = *ap;
        args_va += 8;
        char tmp[256];
        switch (*f) {
        case 's': { char str[200]; if (!kstr(a, str, sizeof str)) snprintf(str, sizeof str, "(%#llx)", (unsigned long long)a);
                    spec[s++] = 's'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, str); break; }
        case 'd': case 'i': spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = 'd'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, (long long)a); break;
        case 'u': case 'x': case 'X': case 'o':
            spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = *f; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, (unsigned long long)a); break;
        case 'p': snprintf(tmp, sizeof tmp, "%#llx", (unsigned long long)a); break;
        case 'c': snprintf(tmp, sizeof tmp, "%c", (int)(a & 0xff)); break;
        default: snprintf(tmp, sizeof tmp, "<%%%c %#llx>", *f, (unsigned long long)a); break;
        }
        for (const char *t = tmp; *t && o + 1 < cap; t++) out[o++] = *t;
    }
    out[o] = 0;
}

static int panic_told;
static void panic_text(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    panic_told = 1;
    static char msg[256], text[6144];
    say("gunyah: the panic's call: op %llu%s, message %#llx, format %#llx, args %#llx\n", (unsigned long long)x0,
        x0 == 3 ? " (DBOP_PANIC)" : "", (unsigned long long)x1, (unsigned long long)x2, (unsigned long long)x3);
    if (x1 && kstr(x1, msg, sizeof msg)) say("gunyah: the panic's message: \"%s\"\n", msg);
    const uint64_t *vl;
    uint64_t args = x3 && kva_ok(x3, 8, (const void **)&vl) ? *vl : 0;
    if (x2) {
        kformat(x2, args, text, sizeof text);
        say("gunyah: THE PANIC: \"%s\"\n", text);
        /* 0.3.84: the panic's own text names the first fault's pc and lr (signed: strip PAC); look both up too */
        const char *s = strstr(text, "at pc 0x");
        if (s) panic_pc(strtoull(s + 8, NULL, 16));
        s = strstr(text, "lr 0x");
        if (s) {
            uint64_t lr = strtoull(s + 5, NULL, 16);
            panic_pc((lr & 0x0000ffffffffffffull) | 0xffff000000000000ull);
        }
    }
    if (args) {
        const uint64_t *a;
        if (kva_ok(args, 64, (const void **)&a))
            say("gunyah: the panic's first args: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
                (unsigned long long)a[4], (unsigned long long)a[5], (unsigned long long)a[6], (unsigned long long)a[7]);
    }
}

static int reg_of(const char *line, const char *key, uint64_t *v) {
    const char *s = strstr(line, key);
    if (!s) return 0;
    s += strlen(key);
    while (*s == ' ') s++;
    if (strncmp(s, "0x", 2)) return 0;
    *v = strtoull(s + 2, NULL, 16);
    return 1;
}

static void uart_char(char c) {
    if (c != '\n' && uart_n < (int)sizeof uart_line - 1) { uart_line[uart_n++] = c; return; }
    uart_line[uart_n] = 0;
    uart_n = 0;
    const char *keys[] = {"at pc 0x", "pc:  0x", "pc: 0x"};
    for (int k = 0; k < 3; k++) {
        const char *s = strstr(uart_line, keys[k]);
        if (s) panic_pc(strtoull(s + strlen(keys[k]), NULL, 16));
    }
    uint64_t r0, r1, r2, r3;
    if (!panic_told && npcs && reg_of(uart_line, "x0:", &r0) && reg_of(uart_line, "x1:", &r1) &&
        reg_of(uart_line, "x2:", &r2) && reg_of(uart_line, "x3:", &r3))
        panic_text(r0, r1, r2, r3);
}

/* ---- the vCPU ---- */

/* 0.3.77's hand-over: vCPU 0 never came back from GH_VCPU_RUN, and 20 s later the hypervisor's watchdog reset the
   tablet with four of its CPUs silent ("cpu alive mask 0f"): a vCPU that runs on and on keeps its CPU from Android.
   A signal every HV_GUNYAH_KICK_MS (2 by default; 0: none) brings each vCPU's GH_VCPU_RUN back, and the thread yields
   before going in again. The app's Gunyah test (--spin, --kick) says whether the driver lets signals through. */
static int kick_ms = 2;
/* 0.3.80: the Mac ran on Gunyah (0.3.79) and the tablet reset soon after with nothing more in the log. After the jump,
   each device page the Mac touches first (up to 96) and, every second for a minute, every vCPU's exit count and last
   access are forced to storage: the log's end then says how far the kernel got. */
static volatile uint64_t last_pa[HV_MAX_CPUS], last_write[HV_MAX_CPUS];
static uint64_t seen_pages[96];
static int nseen;
static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;
static void note_page(int id, uint64_t pa, int write) {
    last_pa[id] = pa, last_write[id] = write;
    if (!mac_runs) return;
    uint64_t pg = pa >> 12;
    pthread_mutex_lock(&seen_mu);
    int seen = 0;
    for (int i = 0; i < nseen; i++) seen |= seen_pages[i] == pg;
    int say_it = !seen && nseen < 96;
    if (say_it) seen_pages[nseen++] = pg;
    pthread_mutex_unlock(&seen_mu);
    if (say_it) say("gunyah: the Mac's vCPU %d first touches %#llx (%s)\n", id, (unsigned long long)pa, write ? "store" : "load");
}
/* 0.3.86: with PAC off (0.3.85) the kernel ran past its panic, made 143 exits (the serial port's set-up) and went quiet
   with no text: Gunyah gives no view of a vCPU's registers, so where it waits is read from its own log instead. XNU's
   msgbuf (struct msgbuf: magic 0x063061, size, write index, read index, buffer pointer) lives in the kernel's data and
   holds every printf, serial console or not. When vCPU 0's exits stop for 3 s, it is found and its last 3 KB logged. */
static void dump_msgbuf(void) {
    if (!k_phys || k_top <= k_phys) return;
    const uint8_t *k = hv_phys_ptr(M, k_phys, k_top - k_phys);
    if (!k) { say("gunyah: the kernel's data is not in the Mac's RAM (no log to read)\n"); return; }
    int found = 0;
    for (uint64_t o = 0; o + 24 <= k_top - k_phys && found < 4; o += 8) {
        if (rd32(k + o) != 0x063061u) continue;
        uint32_t size = rd32(k + o + 4), bufx = rd32(k + o + 8);
        uint64_t bufc = rd64(k + o + 16);
        if (size < 4096 || size > (64u << 20) || bufx >= size || bufc < k_virt) continue;
        const char *buf = hv_phys_ptr(M, bufc - k_virt + k_phys, size);
        if (!buf) continue;
        found++;
        uint32_t take = size < 3072 ? size : 3072;
        say("gunyah: the kernel's log (msgbuf at pa %#llx, %u bytes, written to %u), its last %u bytes:\n---\n",
            (unsigned long long)(k_phys + o), size, bufx, take);
        char out[3073];
        uint32_t m = 0;
        for (uint32_t i = 0; i < take; i++) {
            char c = buf[(bufx + size - take + i) % size];
            if (!c) continue;
            out[m++] = (c >= 32 && c < 127) || c == '\n' || c == '\t' ? c : '?';
        }
        out[m] = 0;
        say("%s\n---\n", out);
    }
    if (!found) say("gunyah: no kernel log (msgbuf) found in the kernel's data yet\n");
    /* 0.3.87: 0.3.86's log ended "failed to find the timer" (harmless: the JIT's boots print it too), then "MACH Reboot"
       a quarter millisecond later: that is the panic path's restart (kdp_machine_reboot_type), and a panic's text goes
       to the panic buffer and the console, not to msgbuf. Every "panic(cpu" in the Mac's RAM is logged with what
       follows it. */
    static const char key[] = "panic(cpu ";
    const uint8_t *ram = B->ram, *end = B->ram + B->ram_size, *p = ram;
    int hits = 0;
    while (hits < 6 && (p = memmem(p, (size_t)(end - p), key, sizeof key - 1)) != NULL) {
        if (p[sizeof key - 1] == '%') { p++; continue; } /* the kernel's own format string */
        char out[2049];
        size_t m = 0;
        for (const uint8_t *q = p; q < end && m < 2048 && *q; q++)
            out[m++] = (*q >= 32 && *q < 127) || *q == '\n' || *q == '\t' ? (char)*q : '?';
        out[m] = 0;
        say("gunyah: panic text in the Mac's RAM at pa %#llx:\n---\n%s\n---\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)(p - ram)), out);
        hits++;
        p += m > 0 ? m : 1;
    }
    if (!hits) say("gunyah: no \"panic(cpu\" text in the Mac's RAM\n");
}

static void *watch_thread(void *arg) {
    (void)arg;
    while (!stop_req && !mac_runs) usleep(10000);
    uint64_t last0 = ~0ull;
    int still = 0, dumps = 0;
    for (int s = 1; !stop_req && s <= 600; s++) {
        sleep(1);
        if (mmio_exits[0] == last0) still++;
        else still = 0, last0 = mmio_exits[0];
        if ((still == 3 && dumps < 3) || (still && still % 60 == 0 && dumps < 6)) dump_msgbuf(), dumps++;
        if (s > 60 && s % 10) continue;
        char line[512];
        int n = snprintf(line, sizeof line, "gunyah: %d s on Gunyah:", s);
        for (int i = 0; i < nvcpu && n < (int)sizeof line - 64; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " vcpu%d %llu exits (last %s %#llx)", i, (unsigned long long)mmio_exits[i],
                          last_write[i] ? "store" : "load", (unsigned long long)last_pa[i]);
        say("%s\n", line);
    }
    return NULL;
}
static uint64_t kicks_back[HV_MAX_CPUS];
static void *kick_thread(void *arg) {
    (void)arg;
    while (!stop_req) {
        usleep((useconds_t)kick_ms * 1000);
        for (int i = 0; i < nvcpu; i++)
            if (vcpu_thread_id[i]) pthread_kill(vcpu_thread_id[i], SIGUSR1);
    }
    return NULL;
}


static void on_sigusr(int s) { (void)s; }

static void *vcpu_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    struct gh_vcpu_run *r = run[id];
    uint64_t unknown = 0, gic_exits = 0, low = 0;
    time_t last_report = time(NULL);
    /* The JIT's CPU 0 thread blocked the signals this one needs (a stop breaks GH_VCPU_RUN with SIGUSR1), and bound
       itself to one core: each vCPU to the core the JIT gave that CPU. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    hv_vm_set_this_cpu(id);
    hv_host_cpu_thread();
    say("gunyah: vCPU %d enters GH_VCPU_RUN\n", id);
    while (!stop_req) {
        if (ioctl(vcpu_fd[id], GH_VCPU_RUN, 0) < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                if (kicks_back[id]++ == 0) say("gunyah: vCPU %d came back on a signal: the kicks work\n", id);
                sched_yield();
                continue;
            }
            fprintf(stderr, "gunyah: running vCPU %d failed: %s\n", id, strerror(errno));
            vm_reason = HV_EXIT_UNSUPPORTED;
            break;
        }
        if (r->exit_reason == GH_VCPU_EXIT_MMIO) {
            uint64_t pa = r->mmio.phys_addr;
            unsigned len = r->mmio.len > 8 ? 8 : r->mmio.len;
            if (mmio_exits[id]++ == 0) say("gunyah: vCPU %d runs (its first exit: %s %#llx)\n", id, r->mmio.is_write ? "a store to" : "a load from",
                                           (unsigned long long)pa);
            note_page(id, pa, r->mmio.is_write);
            /* Always the hand-over page's (vmapple has nothing there): the shim loads x0-x30 from it after it reports
               the jump, and 0.3.80 sent those 31 loads to Henge's devices, which answered 0: the kernel got all zeros. */
            if (pa >= HANDOFF_PA && pa < HANDOFF_PA + 0x5000) {
                uint64_t off = pa - HANDOFF_PA, v = 0;
                if (r->mmio.is_write) {
                    memcpy(&v, r->mmio.data, len);
                    if (off >= 0x4000) shim_report(off - 0x4000, v);
                } else {
                    if (off < 0x4000) v = handoff[off / 8];
                    memset(r->mmio.data, 0, sizeof r->mmio.data);
                    memcpy(r->mmio.data, &v, len);
                }
                continue;
            }
            if (pa < VMAPPLE_MEM && pa >= VMAPPLE_FIRMWARE && pa < VMAPPLE_CONFIG + 0x100000 && low++ < 16)
                fprintf(stderr, "gunyah: vCPU %d %s the %s at %#llx (Gunyah could not give it; answered from Henge's copy)\n", id,
                        r->mmio.is_write ? "writes" : "reads", pa < VMAPPLE_CONFIG ? "boot ROM" : "config window", (unsigned long long)pa);
            if (pa >= VMAPPLE_GIC_DIST && pa < VMAPPLE_GIC_MSI && gic_exits++ < 4)
                fprintf(stderr, "gunyah: vCPU %d's GIC access at %llx came here: Gunyah did not put its GIC at "
                                "vmapple's address\n", id, (unsigned long long)pa);
            /* Every vCPU goes through Henge's device dispatch, which takes the device lock itself. */
            if (r->mmio.is_write && pa == VMAPPLE_UART && mac_runs) uart_char((char)r->mmio.data[0]);
            if (r->mmio.is_write) {
                if (hv_phys_write(M, pa, r->mmio.data, len) != 0 && unknown++ < 16)
                    fprintf(stderr, "gunyah: vCPU %d write to nothing at %llx\n", id, (unsigned long long)pa);
                kick_devices();
            } else {
                memset(r->mmio.data, 0, sizeof r->mmio.data);
                if (hv_phys_read(M, pa, r->mmio.data, len) != 0 && unknown++ < 16)
                    fprintf(stderr, "gunyah: vCPU %d read of nothing at %llx\n", id, (unsigned long long)pa);
            }
        } else if (r->exit_reason == GH_VCPU_EXIT_STATUS) {
            uint32_t reason = 0;
            say("gunyah: vCPU %d: the VM's status changed\n", id);
            memcpy(&reason, r->status.reason, r->status.reason_size < 4 ? r->status.reason_size : 4);
            fprintf(stderr, "gunyah: the VM stopped (seen by vCPU %d): status %u (1 load failed, 2 exited, 3 crashed), "
                            "type %u, reason %#x\n", id, r->status.status, r->status.type, reason);
            vm_reason = r->status.status == 2 ? 0 : HV_EXIT_UNSUPPORTED;
            break;
        } else {
            fprintf(stderr, "gunyah: unknown exit %u on vCPU %d\n", r->exit_reason, id);
            vm_reason = HV_EXIT_UNSUPPORTED;
            break;
        }
        if (id == 0 && time(NULL) - last_report >= 30) {
            last_report = time(NULL);
            fprintf(stderr, "gunyah: device accesses so far:");
            for (int i = 0; i < nvcpu; i++) fprintf(stderr, " vcpu%d %llu", i, (unsigned long long)mmio_exits[i]);
            fputc('\n', stderr);
        }
    }
    hv_gunyah_stop(); /* one vCPU stopping (the VM exited or crashed) stops them all */
    return NULL;
}

void hv_gunyah_stop(void) {
    stop_req = 1;
    for (int i = 0; i < nvcpu; i++) {
        if (run[i]) run[i]->immediate_exit = 1;
        if (vcpu_thread_id[i] && !pthread_equal(vcpu_thread_id[i], pthread_self())) pthread_kill(vcpu_thread_id[i], SIGUSR1);
    }
}

/* A line of the log forced to storage before a step: starting a VM has reset the Xiaomi Pad 7 with nothing
   left to read, and the log's last line after a reset is then the step the tablet did not survive. */
static void mark(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("gunyah: next: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    fsync(2);
}

/* HV_GUNYAH_KMSG=FILE: the kernel's messages from the start of the run copied to FILE, each on storage when its
   write returns, so that after a reset the file ends with what the kernel said last (the app shows it). */
static void *kmsg_thread(void *arg) {
    int in = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
    int out = open((const char *)arg, O_WRONLY | O_TRUNC | O_CREAT | O_DSYNC | O_CLOEXEC, 0644);
    if (in < 0 || out < 0) {
        fprintf(stderr, "gunyah: the kernel's messages are not being kept (%s)\n", strerror(errno));
        return NULL;
    }
    lseek(in, 0, SEEK_END);
    char rec[4096];
    for (;;) {
        ssize_t n = read(in, rec, sizeof rec - 1); /* one record: "level,sequence,microseconds,flags;text\n" */
        if (n < 0) {
            if (errno == EPIPE || errno == EINTR) continue;
            return NULL;
        }
        rec[n] = 0;
        char *text = strchr(rec, ';'), *end = text ? strchr(text, '\n') : NULL;
        if (!text || !end) continue;
        unsigned long long us = 0;
        sscanf(rec, "%*u,%*u,%llu", &us);
        char line[4200];
        int len = snprintf(line, sizeof line, "[%5llu.%06llu] %.*s\n", us / 1000000, us % 1000000, (int)(end - text - 1), text + 1);
        (void)!write(out, line, (size_t)len);
    }
}

static int fail(const char *what) {
    fprintf(stderr, "gunyah: %s failed: %s (errno %d)\n", what, strerror(errno), errno);
    return -1;
}

/* Returns 0 / an HV_EXIT reason once the VM is over, or HV_GUNYAH_NOT_STARTED when nothing of Gunyah's was started
   (the caller lets Henge's CPUs go on). */
int hv_gunyah_run(void) {
    M = hv_vm_machine();
    B = hv_vm_board();
    nvcpu = hv_vm_ncpus();
    if (nvcpu < 1) nvcpu = 1;
    if (nvcpu > HV_MAX_CPUS) nvcpu = HV_MAX_CPUS;
    for (int i = 0; i < HV_MAX_CPUS; i++) vcpu_fd[i] = -1;
    for (int i = 0; i < NIRQ; i++) irq_fd[i] = -1;
    HvCpu *c0 = hv_vm_cpu(0);
    if (c0->el != 1 || (c0->sctlr_el1 & 1)) {
        fprintf(stderr, "gunyah: CPU 0 is at EL%u with its MMU %s: the shim hands over only at EL1 with it off\n", c0->el,
                c0->sctlr_el1 & 1 ? "on" : "off");
        return HV_GUNYAH_NOT_STARTED;
    }
    if (ram_ready() != 0) return HV_GUNYAH_NOT_STARTED;
    if (kpatch() != 0) return HV_GUNYAH_NOT_STARTED;
    int gs = open("/dev/gunyah_share", O_RDWR | O_CLOEXEC);
    if (gs < 0) {
        fprintf(stderr, "gunyah: /dev/gunyah_share: %s (the henge-gunyah Magisk module loads DroidVM's gunyah-host-share)\n", strerror(errno));
        return HV_GUNYAH_NOT_STARTED;
    }

    if (getenv("HV_GUNYAH_KMSG")) {
        pthread_t k;
        if (pthread_create(&k, NULL, kmsg_thread, getenv("HV_GUNYAH_KMSG")) == 0) pthread_detach(k);
        usleep(50000); /* reading before the first request */
    }
    mark("open /dev/gunyah");
    int dev = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
    if (dev < 0) { fail("opening /dev/gunyah (it needs root)"); return HV_GUNYAH_NOT_STARTED; }
    mark("create the VM");
    vm_fd = ioctl(dev, GH_CREATE_VM, 0);
    if (vm_fd < 0) { fail("creating the VM"); return HV_GUNYAH_NOT_STARTED; }

    /* The VM's own region: the shim, then the resource manager's device tree. */
    uint8_t *boot = mmap(NULL, BOOT_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (boot == MAP_FAILED) { fail("mapping the boot region"); return HV_GUNYAH_NOT_STARTED; }
    size_t shim_len = (size_t)(hv_gunyah_shim_end - hv_gunyah_shim_start);
    memcpy(boot, hv_gunyah_shim_start, shim_len);
    uint64_t params[2] = {HANDOFF_PA, BOOT_PA + DTB_OFF};
    memcpy(boot + (hv_gunyah_shim_params - hv_gunyah_shim_start), params, sizeof params);
    uint64_t ram_end = VMAPPLE_MEM + B->ram_size;
    uint64_t size_max = (ram_end - BOOT_PA + (256ull << 20) - 1) & ~((256ull << 20) - 1);
    if (size_max > 0xffffffffull) size_max = 0xfff00000ull; /* one cell: 4 GB less a megabyte at most */
    size_t dtb_len = build_dtb(boot + DTB_OFF, BOOT_SIZE - DTB_OFF, nvcpu, size_max);
    if (!dtb_len) { fprintf(stderr, "gunyah: the VM device tree is too big\n"); return HV_GUNYAH_NOT_STARTED; }
    fprintf(stderr, "gunyah: the VM: its region %#llx (2 MB, lent: shim %zu bytes, tree %zu bytes), up to %#llx for the Mac's "
                    "RAM at %#llx (%llu MB)\n", (unsigned long long)BOOT_PA, shim_len, dtb_len,
            (unsigned long long)(BOOT_PA + size_max), (unsigned long long)VMAPPLE_MEM, (unsigned long long)(B->ram_size >> 20));
    struct gh_userspace_memory_region r = {.label = 0, .flags = GH_MEM_R | GH_MEM_W | GH_MEM_X, .guest_phys_addr = BOOT_PA,
                                           .memory_size = BOOT_SIZE, .userspace_addr = (uintptr_t)boot};
    mark("lend the VM's region");
    if (ioctl(vm_fd, GH_VM_ANDROID_LEND_USER_MEM, &r) < 0) { fail("lending the VM's region"); return HV_GUNYAH_NOT_STARTED; }
    struct gh_vm_dtb_config dtb = {.guest_phys_addr = BOOT_PA + DTB_OFF, .size = BOOT_SIZE - DTB_OFF};
    mark("set the VM device tree");
    if (ioctl(vm_fd, GH_VM_SET_DTB_CONFIG, &dtb) < 0) { fail("setting the VM device tree"); return HV_GUNYAH_NOT_STARTED; }

    int bells = 0;
    mark("add the interrupt doorbells");
    int no_bells = getenv("HV_GUNYAH_BELLS") && *getenv("HV_GUNYAH_BELLS") == '0'; /* bring-up: none (no interrupts reach the Mac) */
    for (int id = 0; id < NIRQ && !no_bells; id++) {
        if (!irq_wanted(id)) continue;
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0) { fail("eventfd"); return HV_GUNYAH_NOT_STARTED; }
        struct gh_fn_irqfd_arg a = {.fd = (uint32_t)efd, .label = (uint32_t)id, .flags = irq_is_msi(id) ? 0 : GH_IRQFD_FLAGS_LEVEL};
        struct gh_fn_desc fn = {.type = GH_FN_IRQFD, .arg_size = sizeof a, .arg = (uintptr_t)&a};
        if (ioctl(vm_fd, GH_VM_ADD_FUNCTION, &fn) < 0) {
            fprintf(stderr, "gunyah: doorbell for interrupt %d: %s\n", id, strerror(errno));
            close(efd);
            continue;
        }
        irq_fd[id] = efd;
        bells++;
    }
    fprintf(stderr, "gunyah: %d interrupt doorbells\n", bells);
    for (int i = 0; i < nvcpu; i++) {
        uint32_t id = (uint32_t)i;
        struct gh_fn_desc fn = {.type = GH_FN_VCPU, .arg_size = sizeof id, .arg = (uintptr_t)&id};
        mark("add vCPU %d", i);
        vcpu_fd[i] = ioctl(vm_fd, GH_VM_ADD_FUNCTION, &fn);
        if (vcpu_fd[i] < 0) {
            fprintf(stderr, "gunyah: adding vCPU %d failed: %s\n", i, strerror(errno));
            return HV_GUNYAH_NOT_STARTED;
        }
    }
    struct rlimit nolimit = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &nolimit);
    mlock(B->ram, B->ram_size);

    mark("start the VM");
    if (ioctl(vm_fd, GH_VM_START, 0) < 0) { fail("starting the VM"); return HV_GUNYAH_NOT_STARTED; }
    /* From here Henge's CPUs cannot take the Mac back: a failure ends the run. */

    /* The Mac's RAM, shared into the running VM 256 MB a piece (DroidVM's way: a memparcel the guest accepts exists
       only once the VM does), each piece's handle for the shim. */
    int pieces = 0, big = 0;
    uint64_t nch = (B->ram_size + (2u << 20) - 1) >> 21, huge_bytes = 0;
    uint8_t *kind = calloc(nch, 1);
    if (!kind) return HV_EXIT_UNSUPPORTED;
    classify(kind, nch);
    for (uint64_t done = 0; done < B->ram_size && pieces < MAX_PIECES; pieces++) {
        /* A run of the same kind: 2 MB pages up to 256 MB a piece, 4 KB pages up to 8 MB. */
        uint64_t c = done >> 21;
        int k = kind[c];
        uint64_t cap = k ? SHARE_CHUNK : SMALL_CHUNK, n = 0;
        while (done + n < B->ram_size && n < cap && kind[(done + n) >> 21] == k) n += 2u << 20;
        if (done + n > B->ram_size) n = B->ram_size - done;
        if (k) huge_bytes += n, big++;
        struct ghsm_share_blob sb = {.vm_fd = vm_fd, .label = 0x4000u + (uint32_t)pieces, .flags = GH_MEM_R | GH_MEM_W | GH_MEM_X,
                                     .guest_phys_addr = VMAPPLE_MEM + done, .memory_size = n,
                                     .userspace_addr = (uintptr_t)B->ram + done};
        mark("share %llu MB of the Mac's RAM at %llx", (unsigned long long)(n >> 20), (unsigned long long)(VMAPPLE_MEM + done));
        if (ioctl(gs, GHSM_SHARE_BLOB, &sb) < 0) {
            fprintf(stderr, "gunyah: sharing the Mac's RAM at %#llx: %s (after %d pieces)\n", (unsigned long long)(VMAPPLE_MEM + done),
                    strerror(errno), pieces);
            return HV_EXIT_UNSUPPORTED;
        }
        handoff[128 + 3 * pieces] = sb.mem_handle, handoff[129 + 3 * pieces] = VMAPPLE_MEM + done, handoff[130 + 3 * pieces] = n;
        done += n;
    }
    free(kind);
    if (pieces >= MAX_PIECES && handoff[129 + 3 * (pieces - 1)] + handoff[130 + 3 * (pieces - 1)] < VMAPPLE_MEM + B->ram_size) {
        fprintf(stderr, "gunyah: the Mac's RAM needs more than %d pieces (too much of it in 4 KB pages): reserve it at boot\n", MAX_PIECES);
        return HV_EXIT_UNSUPPORTED;
    }
    handoff[0] = 0x444e414845474e48ull; /* "HNGEHAND" */
    handoff[1] = (uint64_t)pieces;
    fill_state(c0);
    fprintf(stderr, "gunyah: the Mac's RAM shared as %d pieces: %llu MB in 2 MB pages (%d pieces), %llu MB in 4 KB pages (%d pieces "
                    "of up to 8 MB); the shim accepts them\n", pieces, (unsigned long long)(huge_bytes >> 20), big,
            (unsigned long long)((B->ram_size - huge_bytes) >> 20), pieces - big);

    long sz = ioctl(vcpu_fd[0], GH_VCPU_MMAP_SIZE, 0);
    if (sz < 0) sz = ioctl(dev, GH_VCPU_MMAP_SIZE, 0);
    if (sz <= 0) { fail("asking the vCPU's run area size"); return HV_EXIT_UNSUPPORTED; }
    for (int i = 0; i < nvcpu; i++) {
        run[i] = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd[i], 0);
        if (run[i] == MAP_FAILED) { run[i] = NULL; fail("mapping a vCPU's run area"); return HV_EXIT_UNSUPPORTED; }
    }

    dev_kick = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    hv_vm_stop_hook = hv_gunyah_stop;
    hv_gic_external = gic_external; /* from here Henge's devices ring doorbells instead of its own GIC */
    struct sigaction sa = {.sa_handler = on_sigusr}; /* no SA_RESTART: a stop breaks GH_VCPU_RUN */
    sigaction(SIGUSR1, &sa, NULL);
    mark("run the vCPUs (the shim, then the Mac's kernel)");

    pthread_t t;
    pthread_create(&t, NULL, device_thread, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, pac_thread, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, watch_thread, NULL);
    pthread_detach(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    for (int i = 0; i < nvcpu; i++) pthread_create(&vcpu_thread_id[i], &attr, vcpu_thread, (void *)(intptr_t)i);
    if (getenv("HV_GUNYAH_KICK_MS")) kick_ms = atoi(getenv("HV_GUNYAH_KICK_MS"));
    if (kick_ms > 0) {
        pthread_create(&t, NULL, kick_thread, NULL);
        pthread_detach(t);
    }
    for (int i = 0; i < nvcpu; i++) pthread_join(vcpu_thread_id[i], NULL);
    for (int i = 0; i < nvcpu; i++) vcpu_thread_id[i] = 0;
    stop_req = 1;
    usleep(20000);
    if (!mac_runs) fprintf(stderr, "gunyah: the shim did not reach the Mac (its last phase: %s)\n", phase_name(shim_rep[0]));
    fprintf(stderr, "gunyah: vCPU 0 came back on signals %llu times\n", (unsigned long long)kicks_back[0]);
    return vm_reason < 0 ? 0 : vm_reason;
}
