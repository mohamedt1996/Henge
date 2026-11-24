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
#include <sys/stat.h>
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
#include "virtio.h"

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
        struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write, resume_action; } mmio; /* resume: 0 done, 1 fault */
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
#define SPI_LOW_LAST 95         /* and PCIe INTx 64..95: slot s rings 64 + s (0.3.97: was 64..67, slots 4 and up had none) */
#define NIRQ 160
#define APPLE_MIDR 0x611f0220ull /* what Henge's JIT answers (core/sysreg.c MIDR) */

extern void (*hv_vm_stop_hook)(void);
void hv_host_cpu_thread(void); /* the core this thread's CPU has (android/runner.c; a no-op elsewhere) */
extern const uint8_t hv_gunyah_shim_start[], hv_gunyah_shim_end[], hv_gunyah_shim_params[];
extern const uint8_t hv_gunyah_fix_start[], hv_gunyah_fix_end[];
extern const uint8_t hv_gunyah_fixtb_start[], hv_gunyah_fixtb_end[]; /* the same, with the timer's traps */

static HvMachine *M;
static HvVmapple *B;
static int vm_fd = -1, dev_kick = -1, nvcpu;
static int vcpu_fd[HV_MAX_CPUS];
static struct gh_vcpu_run *run[HV_MAX_CPUS];
static volatile int stop_req, vm_reason = -1;
static pthread_t vcpu_thread_id[HV_MAX_CPUS];
/* 0.3.100: a vCPU thread's id is signalled only while the thread lives (vcpu_alive, under kill_mu). The kicker and a
   stopping vCPU signalled threads the main thread had already joined: pthread_kill on a freed thread, the engine's
   "Segmentation fault" right after "the VM stopped" (0.3.99's first run). */
static pthread_mutex_t kill_mu = PTHREAD_MUTEX_INITIALIZER;
static int vcpu_alive[HV_MAX_CPUS];
static void kick_vcpu(int i) {
    pthread_mutex_lock(&kill_mu);
    if (vcpu_alive[i] && !pthread_equal(vcpu_thread_id[i], pthread_self())) pthread_kill(vcpu_thread_id[i], SIGUSR1);
    pthread_mutex_unlock(&kill_mu);
}
static volatile uint64_t mmio_exits[HV_MAX_CPUS];
static int irq_fd[NIRQ];
static volatile uint8_t irq_level[NIRQ];
static volatile uint64_t rings[NIRQ];  /* doorbell rings per interrupt (0.3.98: the silence report shows them) */
static volatile uint64_t dev_ticks;    /* device-thread passes (each runs the devices' completions) */
static uint64_t now_ns(void);
static volatile uint64_t dev_gap_max; /* the device thread's longest pause since the last 30 s line */
static volatile uint64_t uart_last_ns; /* when the kernel last wrote the serial port */
static volatile uint64_t fault_pa;     /* 0.3.99: the next read here by vCPU 0 is answered with an abort (see watch_thread) */
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
    /* 0.3.94: the IPA ranges whose faults Gunyah hands to Henge (VMMIO). Without this property the resource manager
       uses its legacy default, [0, 1 GB): every vmapple device below 0x40000000 reached Henge, but the PCIe ECAM
       (0x40000000) and PCIe MMIO (0x50000000) windows got external aborts, and XNU's PCI configuration panicked on
       its first config read past device 0. Now everything below the Mac's RAM and the VM's own region. */
    uint32_t vmmio[4] = {0, 0, (uint32_t)(BOOT_PA >> 32), (uint32_t)BOOT_PA};
    prop_u32s(&f, "vmmio-ranges", vmmio, 4);
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
        prop(&f, "source-can-clear", NULL, 0); /* 0.3.99: Henge's send capability may clear and mask it too (dbfind) */
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
    rings[intid]++;
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

/* `spread`: 0.3.117. The pass every 30 s read all 3 GB of the Mac's RAM in one go, a second or so of one core and of
   the memory bus and the caches the Mac's CPUs share with it, in the middle of whatever the Mac was doing. It now
   takes 16 MB at a time with a rest between (about 190 pieces: the same RAM in the same half minute). */
static void pac_scan(int spread) {
    for (int r = 0; r < M->nram; r++) {
        uint32_t *w = (uint32_t *)M->ram[r].host;
        size_t n = M->ram[r].size / 4;
        for (size_t i = 0; i + 1 < n; i++) {
            if (!(i & 0xfffff) && stop_req) return;
            if (spread && i && !(i & 0x3fffff)) usleep(150000);
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

/* 0.3.98: 0.3.97 died of SIGSEGV with nothing said: the engine's handler (vm.c on_ill) names the place, but a thread
   that overflowed its stack has nowhere to run it. Every thread here gets a signal stack of its own. */
static void thread_altstack(void) {
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

static void *pac_thread(void *arg) {
    (void)arg;
    thread_altstack();
    uint64_t reported = pac_patched;
    while (!stop_req) {
        /* The kernel's own calls were rewritten before the start (kpatch); this is for code loaded later (the
           auxiliary kernel collection), a pass over all of the Mac's RAM every 30 s. */
        /* 0.3.117: the first pass 30 s in, at once (the auxiliary collection loads during the boot); each later one
           spread over its half minute. */
        static int passes;
        if (!passes)
            for (int i = 0; i < 300 && !stop_req; i++) usleep(100000);
        if (stop_req) break;
        pac_scan(passes++ > 0);
        if (pac_patched != reported) {
            fprintf(stderr, "gunyah: %llu PAC hypercalls rewritten so far\n", (unsigned long long)pac_patched);
            reported = pac_patched;
        }
    }
    return NULL;
}

/* ---- the Mac's clock at 24 MHz (0.3.119) ----
   Every Apple Silicon Mac's timebase is 24 MHz, and macOS's sound server counts on it: in each of the 307 overload
   reports of four runs coreaudiod takes one IO cycle of 1024 frames at 48 kHz for 511,999 ticks (24 MHz; 409,600 at
   the tablet's 19.2), whatever the kernel was told. Its cycles and the sound driver's timestamps (which follow
   mach_timebase_info) then disagree by a quarter: a re-anchor and an overload about once a second, the cuts in
   Chrome, and what Safari's GPU process dies logging. On the JIT the Mac's counter is 24 MHz and sound is right.

   HV_GUNYAH_TIMEBASE=24 (Settings > Mac's clock: 24 MHz), where the counter is 19.2 MHz (24 = 19.2 x 5/4, as the JIT
   scales it): the kernel's reads of the counter go through stubs that answer c + c/4, its writes of the timers'
   compare and count-down values through stubs that take a fifth off, CNTFRQ reads answer 24 MHz, and the device
   tree keeps its 24 MHz. Programs read the counter themselves where the kernel's commpage says they may (a byte at
   +0x90: 0 none, 1 to 3 how), unscaled; that byte is set to 0 once the kernel has made the page, and
   mach_absolute_time asks the kernel instead. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int g_tb24;        /* the counter is scaled for the Mac */
static uint64_t g_ct_seen[6], g_ct_done, g_ct_left;
static struct { uint64_t pa, end; int fix; } g_ts[72]; /* the timer's stubs and the fix-ups, for the stall report */
static int g_nts;

static int commpage_like(const uint8_t *c, int ncpu) {
    if (c[0x24] != 14 || c[0x25] != 14 || c[0x37] != 14) return 0; /* user and kernel page shifts: 16 KB */
    if (c[0x22] != ncpu || c[0x35] != ncpu) return 0;              /* CPUs configured, physical */
    uint64_t mem;
    memcpy(&mem, c + 0x38, 8);
    return mem >= (1ull << 30) && mem <= (64ull << 30) && c[0x90] <= 3;
}

static void *commpage_thread(void *arg) {
    (void)arg;
    thread_altstack();
    while (!stop_req && !mac_runs) usleep(10000);
    uint8_t *at[4];
    int nat = 0, said = 0, set = 0;
    for (int pass = 0; pass < 1200 && !stop_req; pass++) { /* two minutes */
        if (!nat || pass % 50 == 0) {
            nat = 0;
            for (int r = 0; r < M->nram && nat < 4; r++) {
                uint8_t *p = (uint8_t *)M->ram[r].host;
                for (uint64_t o = 0; o + 0x4000 <= M->ram[r].size && nat < 4; o += 0x4000)
                    if (commpage_like(p + o, nvcpu)) {
                        at[nat++] = p + o;
                        if (said < 4) {
                            char hex[2 * 0xa0 + 1];
                            for (int i = 0; i < 0xa0; i++) snprintf(hex + 2 * i, 3, "%02x", p[o + (uint64_t)i]);
                            say("gunyah: a commpage at pa %#llx (programs' own counter reads: %u; its first 160 bytes %s)\n",
                                (unsigned long long)(M->ram[r].pa + o), p[o + 0x90], hex);
                            said++;
                        }
                    }
            }
        }
        if (g_tb24)
            for (int i = 0; i < nat; i++)
                if (at[i][0x90]) {
                    __atomic_store_n(&at[i][0x90], 0, __ATOMIC_RELEASE);
                    if (set++ < 4) say("gunyah: that commpage now sends programs to the kernel for the time (the scaled counter)\n");
                }
        if (!g_tb24 && nat) break; /* only said, where the counter is not scaled */
        if (pass == 300 && !nat)
            say(g_tb24 ? "gunyah: NO COMMPAGE FOUND after 30 s: programs in the Mac read the unscaled counter, their time runs a fifth "
                         "slow against the kernel's. Settings > Mac's clock: Exact\n"
                       : "gunyah: no commpage found after 30 s (its layout is not the one looked for)\n");
        usleep(100000);
    }
    return NULL;
}

/* ---- devices ---- */

static void *device_thread(void *arg) {
    (void)arg;
    thread_altstack();
    HvCpu *cpu0 = hv_vm_cpu(0);
    struct pollfd p = {.fd = dev_kick, .events = POLLIN};
    uint64_t n = 0, pass_at = now_ns();
    while (!stop_req) {
        if (poll(&p, 1, 1) > 0) { uint64_t v; if (read(dev_kick, &v, sizeof v) < 0) { } }
        {
            uint64_t t = now_ns();
            if (t - pass_at > dev_gap_max) dev_gap_max = t - pass_at;
            pass_at = t;
        }
        if (M->tick) M->tick(M, cpu0);
        dev_ticks++;
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
static uint64_t fb_pa;            /* the Mac's screen: boot_args' Video at first, initialize_screen's once the kernel says */
static uint32_t fb_w, fb_h, fb_rb;
#define RV_MARK 0x4f00u          /* hand-over page offset the reset vector's stub stores to (x16 MPIDR, then +8 its read) */
static uint64_t rv_rhd, rv_sites; /* where ResetHandlerData.cpu_data_entries is; reset vectors patched */
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

/* 0.3.95: Gunyah hands Henge only faults inside the VM's VMMIO ranges, and this tablet's resource manager keeps its
   default [0, 1 GB) whatever the VM's tree says (0.3.94's vmmio-ranges changed nothing: ECAM 0x40008000 and MMIO
   0x50000000 still fault in the shim). vmapple's PCIe ECAM (0x40000000) and MMIO window (0x50000000) sit above it.
   So under Gunyah the bus moves below 1 GB, into the free space after vmapple's last device: ECAM at 0x31000000
   (16 MB: buses 0-15; Henge has bus 0 only), MMIO at 0x32000000 (224 MB; Henge's BARs are 16-64 KB). Henge's PCIe
   model answers there too (hv_pcie_alias), and the kernel's tree is told: in each pci node, the old addresses (as
   the arm-io offsets 0x30000000/0x40000000 or as 0x40000000/0x50000000), their sizes and bus-range become the new. */
#define PCIE_ECAM_NEW 0x31000000ull
#define PCIE_ECAM_NEW_SIZE 0x01000000ull
#define PCIE_MMIO_NEW 0x32000000ull
#define PCIE_MMIO_NEW_SIZE 0x0e000000ull
static int pcie_moved;

static void pcie_node(const char *name, uint8_t *node, uint8_t *end) {
    uint32_t np = rd32(node);
    uint8_t *q = node + 8;
    int abs = -1; /* 1: the addresses are absolute, 0: offsets from arm-io's 0x10000000 */
    for (int pass = 0; pass < 2; pass++) {
        q = node + 8;
        for (uint32_t i = 0; i < np && q + 36 <= end; i++) {
            char pn[33];
            snprintf(pn, sizeof pn, "%.32s", (const char *)q);
            uint32_t len = rd32(q + 32) & 0x7fffffffu;
            uint8_t *v = q + 36;
            if (v + len > end) return;
            if (pass == 0) {
                char hex[3 * 64 + 1];
                size_t h = 0;
                for (uint32_t k = 0; k < len && k < 64; k++) h += (size_t)snprintf(hex + h, sizeof hex - h, "%02x ", v[k]);
                hex[h] = 0;
                int printable = len > 0 && len < 64;
                for (uint32_t k = 0; k + 1 < len && printable; k++) printable = v[k] >= 32 && v[k] < 127;
                fprintf(stderr, "gunyah: the kernel's %s: %s (%u bytes): %s%s%.*s%s\n", name, pn, len, printable ? "\"" : "",
                        printable ? "" : hex, printable ? (int)len : 0, printable ? (const char *)v : "", printable ? "\"" : "");
                for (uint32_t k = 0; k + 4 <= len && (!strcmp(pn, "reg") || !strcmp(pn, "ranges")); k += 4) {
                    uint32_t x = rd32(v + k);
                    if (!strcmp(pn, "reg") && x == 0x30000000u) abs = 0;            /* ECAM as an arm-io offset */
                    else if (abs < 0 && (x == 0x40000000u || x == 0x50000000u)) abs = 1;
                }
            } else if (abs >= 0 && (!strcmp(pn, "reg") || !strcmp(pn, "ranges") || !strcmp(pn, "bus-range"))) {
                for (uint32_t k = 0; k + 4 <= len; k += 4) {
                    uint32_t x = rd32(v + k), nx = x;
                    const uint32_t rel = (uint32_t)VMAPPLE_GIC_DIST; /* arm-io's base, 0x10000000 */
                    int reg = !strcmp(pn, "reg");
                    if (!strcmp(pn, "bus-range")) { if (k == 4 && x == 255) nx = 15; }
                    else if (x == 0x50000000u) nx = (uint32_t)PCIE_MMIO_NEW;                     /* MMIO, absolute */
                    else if (reg && x == 0x30000000u) nx = (uint32_t)PCIE_ECAM_NEW - rel;         /* ECAM, offset */
                    else if (reg && x == 0x40000000u) nx = abs ? (uint32_t)PCIE_ECAM_NEW : (uint32_t)PCIE_MMIO_NEW - rel;
                    else if (!reg && x == 0x40000000u) nx = (uint32_t)PCIE_MMIO_NEW - rel;       /* ranges: MMIO, offset */
                    else if (reg && x == 0x10000000u) nx = (uint32_t)PCIE_ECAM_NEW_SIZE;
                    else if (x == 0x1fff0000u) nx = (uint32_t)PCIE_MMIO_NEW_SIZE;
                    if (nx != x) {
                        memcpy(v + k, &nx, 4);
                        pcie_moved++;
                        fprintf(stderr, "gunyah: the kernel's %s %s +%u: %#x -> %#x\n", name, pn, k, x, nx);
                    }
                }
            }
            q += 36 + ((len + 3) & ~3u);
        }
        if (pass == 0 && abs < 0) fprintf(stderr, "gunyah: the kernel's %s: its PCIe addresses not recognized, left as they are\n", name);
    }
}

/* The patches. Returns 0, or -1 when the kernel was not found (the hand-over does not happen). */
static int kpatch(void) {
    int64_t ba = find_boot_args();
    if (ba < 0) { fprintf(stderr, "gunyah: no boot_args in the Mac's RAM: this is not the jump into the kernel\n"); return -1; }
    const uint8_t *b = B->ram + ba;
    uint64_t virt = rd64(b + 8), phys = rd64(b + 16), top = rd64(b + 32), dtva = rd64(b + 96);
    k_virt = virt, k_phys = phys, k_top = top;
    { /* 0.3.102: boot_args' Video (base, display, rowBytes, width, height, depth at +40): the screen iBoot set up */
        uint64_t vb = rd64(b + 40), vr = rd64(b + 56), vw = rd64(b + 64), vh = rd64(b + 72);
        if (vb >= VMAPPLE_MEM && vw && vh && vw <= 8192 && vh <= 8192 && vr >= vw * 4)
            fb_pa = vb, fb_w = (uint32_t)vw, fb_h = (uint32_t)vh, fb_rb = (uint32_t)vr;
    }
    uint32_t dtlen = rd32(b + 104);
    /* 0.3.89: the kernel's own console on the serial port (this log), panics included. Until now a panic this early had
       nowhere to go (no panic buffer yet, serial console off, video console not drawn) and showed only as "MACH Reboot".
       boot_args' CommandLine (offset 108) is edited before the kernel parses it. HV_GUNYAH_BOOT_ARGS replaces what is
       added ("" adds nothing). */
    {
        char *cmd = (char *)B->ram + ba + 108;
        const char *add = getenv("HV_GUNYAH_BOOT_ARGS");
        if (!add) add = "serial=3 debug=0x8 keepsyms=1 -v";
        size_t len = strnlen(cmd, 600);
        if (*add && !strstr(cmd, "serial=") && len + 1 + strlen(add) < 600) {
            if (len) cmd[len++] = ' ';
            strcpy(cmd + len, add);
        }
        fprintf(stderr, "gunyah: the kernel's boot-args: \"%.600s\"\n", cmd);
    }
    fprintf(stderr, "gunyah: boot_args at pa %#llx: kernel at virtual %#llx = physical %#llx, its data to %#llx, device tree "
                    "at %#llx (%u bytes)\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)ba), (unsigned long long)virt,
            (unsigned long long)phys, (unsigned long long)top, (unsigned long long)dtva, dtlen);
    /* The kernel collection's header: an arm64 Mach-O (a fileset, or a plain kernel) linked in the kernel's half of
       the address space. 0.3.104: the first Mach-O from physBase on was a
       user program's (__TEXT at 0x100000000; the reserved RAM keeps the last run's pages, and recoveryOS's ramdisk
       holds binaries), so nothing was patched and the kernel died on its first pointer authentication. Now only a
       kernel's header is taken, looked for from 64 MB below physBase to its data's end. */
    uint64_t hdr = 0, base_vm = 0, lo = phys > VMAPPLE_MEM + (64ull << 20) ? (phys - (64ull << 20)) & ~0x3fffull : VMAPPLE_MEM;
    uint64_t hi = top > phys ? top : phys + (256ull << 20);
    int seen = 0;
    for (int pass = 0; pass < 2 && !hdr; pass++)
        for (uint64_t pa = pass ? lo : phys & ~0x3fffull; pa < (pass ? phys : hi) && pa + 32 <= VMAPPLE_MEM + B->ram_size; pa += 0x4000) {
            const uint8_t *h = hv_phys_ptr(M, pa, 32);
            if (!h || rd32(h) != 0xfeedfacfu || rd32(h + 4) != 0x0100000cu || (rd32(h + 12) != 12 && rd32(h + 12) != 2)) continue;
            uint32_t ncmds = rd32(h + 16), sizeofcmds = rd32(h + 20);
            const uint8_t *lc = sizeofcmds < (4u << 20) ? hv_phys_ptr(M, pa + 32, sizeofcmds) : NULL;
            uint64_t bv = 0;
            for (uint32_t i = 0, o = 0; lc && i < ncmds && o + 8 <= sizeofcmds; i++) {
                uint32_t cmd = rd32(lc + o), size = rd32(lc + o + 4);
                if (size < 8) break;
                if (cmd == 0x19 && size >= 72 && rd64(lc + o + 40) == 0 && rd64(lc + o + 48)) { bv = rd64(lc + o + 24); break; }
                o += size;
            }
            /* The load commands hold the kernel's unslid addresses (0.3.105's run: the fileset at pa 0x7497c000, __TEXT
               at 0xfffffe0007004000, 64 MB past physBase, whose page is zeros), so the test is the kind: a fileset, or
               a plain kernel, linked in the kernel's half of the address space; a user program never is. */
            int fits = bv >= 0xfffffe0000000000ull && (rd32(h + 12) == 12 || rd32(h + 12) == 2);
            if (seen++ < 6)
                fprintf(stderr, "gunyah: a Mach-O at pa %#llx (%s, __TEXT at %#llx)%s\n", (unsigned long long)pa,
                        rd32(h + 12) == 12 ? "fileset" : "executable", (unsigned long long)bv,
                        fits ? ": the kernel's" : ": not the kernel's (a user program's, or a fileset entry's), passed over");
            if (fits) { hdr = pa, base_vm = bv; break; }
        }
    if (!hdr) {
        const uint8_t *h = hv_phys_ptr(M, phys & ~0x3fffull, 32);
        fprintf(stderr, "gunyah: the kernel's Mach-O header was not found around %#llx (%d other Mach-Os); at physBase: "
                        "%08x %08x %08x %08x: the hand-over is off, Henge's CPUs go on\n", (unsigned long long)phys, seen,
                h ? rd32(h) : 0, h ? rd32(h + 4) : 0, h ? rd32(h + 8) : 0, h ? rd32(h + 12) : 0);
        return -1;
    }
    exec_segments(hdr, base_vm, 0);
    uint64_t code = 0;
    for (int i = 0; i < nxr; i++) code += xr[i].size;
    fprintf(stderr, "gunyah: kernel collection at pa %#llx (%s, __TEXT at %#llx): %d code ranges, %llu MB\n", (unsigned long long)hdr,
            rd32(hv_phys_ptr(M, hdr, 32) + 12) == 12 ? "fileset" : "kernel", (unsigned long long)base_vm, nxr,
            (unsigned long long)(code >> 20));
    if (code < (1ull << 20)) {
        fprintf(stderr, "gunyah: under 1 MB of kernel code found: nothing could be patched; the hand-over is off, Henge's CPUs go on\n");
        return -1;
    }

    uint64_t impl_r = 0, impl_w = 0, midr = 0, midr_left = 0, pac = 0, sctlr = 0;
    uint64_t vec[8], pools[64], zs[64];
    int nvec = 0;
    const char *kp = getenv("HV_GUNYAH_KEEP_PAC");
    int keep_pac = kp && *kp == '1';
    {
        uint64_t hz;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        const char *t = getenv("HV_GUNYAH_TIMEBASE");
        g_tb24 = t && !strcmp(t, "24") && hz == 19200000;
        if (t && !strcmp(t, "24") && !g_tb24)
            fprintf(stderr, "gunyah: the Mac's clock at 24 MHz is for a 19.2 MHz counter; this one is %llu Hz: its own rate is told\n",
                    (unsigned long long)hz);
    }
    for (int r = 0; r < nxr; r++) {
        uint32_t *w = hv_phys_ptr(M, xr[r].pa, xr[r].size);
        pools[r] = xr[r].size / 4, zs[r] = 0;
        if (!w) continue;
        uint64_t n = xr[r].size / 4;
        /* The stub pool: the zeros at the code's end (page padding), less a margin. */
        uint64_t z = n;
        while (z > 0 && w[z - 1] == 0) z--;
        uint64_t pool = z + 16 < n ? z + 16 : n;
        zs[r] = z;
        for (uint64_t i = 0; i < z; i++) {
            uint32_t x = w[i];
            if ((x & ~31u) == 0xd518c000u && nvec < 8) { /* MSR VBAR_EL1, Xt: where its ADRP (+ADD) points */
                unsigned t = x & 31;
                for (uint64_t j = i; j > 0 && j + 12 > i; j--) {
                    uint32_t a = w[j - 1];
                    if ((a & 0x9f00001fu) != (0x90000000u | t)) continue;
                    int64_t imm = (int64_t)((((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3));
                    if (imm & (1ll << 20)) imm -= 1ll << 21;
                    uint64_t tgt = ((xr[r].pa + (j - 1) * 4) & ~0xfffull) + (uint64_t)(imm << 12);
                    for (uint64_t k = j; k < i; k++) /* ADD Xt, Xt, #imm12 */
                        if ((w[k] & 0xffc003ffu) == (0x91000000u | t << 5 | t)) tgt += (w[k] >> 10) & 0xfff;
                    int dup = 0;
                    for (int v = 0; v < nvec; v++) dup |= vec[v] == tgt;
                    if (!dup && !(tgt & 0x7ff)) vec[nvec++] = tgt;
                    break;
                }
            }
            if (x == HVC0 && i + 1 < n && w[i + 1] == CBNZ_X0_SELF) {
                w[i] = MOV_X0_0, w[i + 1] = NOP, pac++;
                continue;
            }
            /* 0.3.96: XNU's reset_vector (msr OSLAR_EL1, xzr; msr DAIFSet, #0xf; ... adrp x19, ResetHandlerData ...
               ldr x1, [x19, #CPU_DATA_ENTRIES]), where PSCI CPU_ON starts a secondary with its MMU and caches off. A stub
               reports the CPU's arrival and what it reads there (see rv_report); x16 and x17 are free in that code. */
            if (x == 0xd510109fu && i + 12 < z && w[i + 1] == 0xd5034fdfu && rv_sites < 4) {
                uint64_t rhd = 0;
                int ldr_off = -1;
                for (uint64_t k = i + 2; k < i + 12; k++) {
                    uint32_t a = w[k];
                    if ((a & 0x9f00001fu) == 0x90000013u && !rhd) { /* adrp x19 */
                        int64_t imm = (int64_t)((((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3));
                        if (imm & (1ll << 20)) imm -= 1ll << 21;
                        rhd = ((xr[r].pa + k * 4) & ~0xfffull) + (uint64_t)(imm << 12);
                        if ((w[k + 1] & 0xffc003ffu) == 0x91000273u) rhd += (w[k + 1] >> 10) & 0xfff; /* add x19, x19 */
                    } else if (rhd && (a & 0xffc003ffu) == 0xf9400261u) { /* ldr x1, [x19, #imm] */
                        ldr_off = (int)((a >> 10) & 0xfff) * 8;
                        break;
                    }
                }
                if (rhd && ldr_off >= 0 && pool + 10 <= n && (int64_t)((pool - i) * 4) < (120ll << 20)) {
                    uint64_t site = xr[r].pa + i * 4, stub = xr[r].pa + pool * 4, mark = HANDOFF_PA + RV_MARK;
                    uint64_t tgt = rhd + (uint64_t)ldr_off, at5 = stub + 20;
                    int64_t pg = (int64_t)((tgt & ~0xfffull) - (at5 & ~0xfffull)) >> 12;
                    w[pool] = x;                                                    /* msr oslar_el1, xzr */
                    w[pool + 1] = 0xd53800b0u;                                      /* mrs x16, mpidr_el1 */
                    w[pool + 2] = MOVZ_XS(17, (mark >> 16) & 0xffff, 1);
                    w[pool + 3] = MOVK_X(17, mark & 0xffff, 0);
                    w[pool + 4] = 0xf9000230u;                                      /* str x16, [x17] */
                    w[pool + 5] = 0x90000010u | (uint32_t)(pg & 3) << 29 | (uint32_t)((pg >> 2) & 0x7ffff) << 5; /* adrp x16 */
                    w[pool + 6] = 0x91000210u | (uint32_t)(tgt & 0xfff) << 10;      /* add x16, x16, #lo12 */
                    w[pool + 7] = 0xf9400210u;                                      /* ldr x16, [x16] (caches off) */
                    w[pool + 8] = 0xf9000630u;                                      /* str x16, [x17, #8] */
                    w[pool + 9] = branch(stub + 36, site + 4);
                    w[i] = branch(site, stub);
                    pool += 10;
                    rv_rhd = tgt, rv_sites++;
                    fprintf(stderr, "gunyah: the kernel's reset vector at pa %#llx reports each CPU it starts (its CPU table "
                                    "pointer at pa %#llx)\n", (unsigned long long)site, (unsigned long long)tgt);
                }
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
        pools[r] = pool;
    }
    pac_patched += pac;
    /* 0.3.92: device accesses Gunyah cannot emulate (writeback LDR/STR: ESR ISV 0, a synchronous external abort back
       to the kernel; XNU's pe_init_fiq has `str w9, [x8, #0x80]!` on the GIC). Each kernel exception table's EL1
       synchronous entries (SP0 at +0, SPx at +0x200) first go through host/gunyah_fixup.S, which carries such an access
       out as a plain one and goes on. */
    int vfixed = 0, tb_ok = nvec > 0;
    {
        /* The Mac's clock at 24 MHz: the fix-up that also carries out the timer's instructions (240 bytes more). */
        const uint8_t *fix = g_tb24 ? hv_gunyah_fixtb_start : hv_gunyah_fix_start;
        size_t flen = g_tb24 ? (size_t)(hv_gunyah_fixtb_end - hv_gunyah_fixtb_start) : (size_t)(hv_gunyah_fix_end - hv_gunyah_fix_start);
        size_t fw = flen / 4;
        for (int v = 0; v < nvec; v++) {
            for (int e = 0; e < 2; e++) {
                uint64_t ent = vec[v] + (e ? 0x200 : 0);
                uint32_t *ew = hv_phys_ptr(M, ent, 4);
                if (!ew) { tb_ok = 0; continue; }
                uint32_t first = *ew;
                /* The displaced instruction runs elsewhere: an unconditional B (XNU's BRANCH_TO_KVA_VECTOR) is
                   re-aimed from there; nothing else PC-relative (ADR/ADRP, BL, conditional branches, literals). */
                int64_t btgt = -1;
                if ((first & 0xfc000000u) == 0x14000000u) {
                    int64_t off = (int64_t)(first & 0x3ffffff);
                    if (off & (1ll << 25)) off -= 1ll << 26;
                    btgt = (int64_t)ent + off * 4;
                } else if ((first & 0x1f000000u) == 0x10000000u || (first & 0x7c000000u) == 0x14000000u ||
                    (first & 0x7e000000u) == 0x34000000u || (first & 0x7e000000u) == 0x36000000u ||
                    (first & 0xff000010u) == 0x54000000u || (first & 0x3b000000u) == 0x18000000u) {
                    fprintf(stderr, "gunyah: exception vector %#llx starts PC-relative (%08x): not fixed up\n", (unsigned long long)ent, first);
                    tb_ok = 0;
                    continue;
                }
                int done = 0;
                for (int r = 0; r < nxr && !done; r++) {
                    uint64_t room = xr[r].size / 4 - pools[r];
                    uint64_t at = xr[r].pa + pools[r] * 4;
                    if (room < fw + 4 || (at > ent ? at - ent : ent - at) >= (120ull << 20)) continue;
                    uint32_t *dst = hv_phys_ptr(M, at, flen);
                    if (!dst) continue;
                    memcpy(dst, fix, flen);
                    if (btgt >= 0) {
                        uint64_t d = (uint64_t)btgt > at ? (uint64_t)btgt - at : at - (uint64_t)btgt;
                        if (d >= (120ull << 20)) continue;
                        dst[fw - 2] = branch(at + (fw - 2) * 4, (uint64_t)btgt);
                        dst[fw - 1] = NOP;
                    } else {
                        dst[fw - 2] = first;
                        dst[fw - 1] = branch(at + (fw - 1) * 4, ent + 4);
                    }
                    *ew = branch(ent, at);
                    pools[r] += fw + 1;
                    vfixed++, done = 1;
                    fprintf(stderr, "gunyah: fix-up of exception vector %#llx: %zu bytes at pa %#llx (code range %d)\n",
                            (unsigned long long)ent, flen, (unsigned long long)at, r);
                    if (g_nts < 72) g_ts[g_nts].pa = at, g_ts[g_nts].end = at + flen, g_ts[g_nts].fix = 1, g_nts++;
                    /* Page padding: fewer zeros than a page at the range's end. More are the kernel's own data. */
                    if (e == 0 && xr[r].size - zs[r] * 4 > 16384) tb_ok = 0;
                }
                if (!done) tb_ok = 0;
                if (!done) fprintf(stderr, "gunyah: no room near exception vector %#llx for its fix-up\n", (unsigned long long)ent);
            }
        }
        fprintf(stderr, "gunyah: %d exception tables found, %d synchronous entries now carry out device accesses Gunyah cannot\n", nvec, vfixed);
    }
    /* 0.3.121: the generic timer, for the Mac's clock at 24 MHz. 0.3.119 and 0.3.120 gave each timer instruction a
       stub in the zeros at a code range's end; the first range has 11.5 KB of them, nearly all taken by three fix-ups,
       and the only other zeros (390 KB at the end of range 7) are the kernel's own data, not padding: 0.3.120's stall
       report found stack frames and tables where the stubs had been, and 10218 saved copies of one stub's address.
       So no stubs: each instruction becomes the permanently undefined word 0x00007a00 | op << 5 | Rt, which the
       fix-up of the synchronous vectors (host/gunyah_fixup_tb.S) carries out. Only if every table's SP0 entry has its
       fix-up in page padding; else nothing is rewritten and the Mac gets the counter's own rate. */
    if (g_tb24 && !tb_ok) {
        fprintf(stderr, "gunyah: the Mac's clock at 24 MHz needs every exception table's fix-up in the code's page padding; "
                        "it is not there: the Mac gets the counter's own rate (as Settings > Mac's clock: Exact)\n");
        g_tb24 = 0;
    }
    /* 0.3.121's start reached launchd, which died of SIGILL: a trap rewrites ESR_EL1, and XNU's fleh_dispatch64
       (osfmk/arm64/locore.s) calls recount_leave_user, which reads the time, between an exception from a program and
       the handler's reading ESR_EL1 and FAR_EL1; the program's system call was then taken for an undefined
       instruction. That one call (mov x21, x0; mov x22, x1; cmp; b.gt; stp fp, lr; mov fp, sp; BL; ...; mov x0, x21)
       now goes through a wrapper in the page padding that keeps the two registers over it. Not found once, or no
       room: nothing is rewritten. */
    if (g_tb24) {
        int found = 0, fr = -1;
        uint64_t fi = 0;
        for (int r = 0; r < nxr; r++) {
            uint32_t *w = hv_phys_ptr(M, xr[r].pa, xr[r].size);
            if (!w || zs[r] < 16) continue;
            for (uint64_t i = 0; i + 20 < zs[r]; i++) {
                if (w[i] != 0xaa0003f5u || w[i + 1] != 0xaa0103f6u) continue; /* mov x21, x0; mov x22, x1 */
                int bl = -1, nbl = 0, back = 0;
                for (int k = 2; k < 14 && !back; k++) {
                    if ((w[i + k] & 0xfc000000u) == 0x94000000u) bl = k, nbl++;
                    else if (w[i + k] == 0xaa1503e0u) back = k; /* mov x0, x21 */
                }
                if (nbl != 1 || !back || bl > back) continue;
                /* 0.3.123 found six such places. fleh_dispatch64's ends in the jump to the handler: br x22, or
                   braa x22, sp where the kernel is built with pointer authentication. Each candidate is logged. */
                int disp = 0;
                for (int k = back + 1; k < back + 5 && !disp; k++)
                    disp = w[i + k] == 0xd61f02c0u || w[i + k] == 0xd71f0adfu;
                fprintf(stderr, "gunyah: a call between mov x21, x0; mov x22, x1 and mov x0, x21 at pa %#llx%s: %08x %08x %08x "
                                "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        (unsigned long long)(xr[r].pa + i * 4), disp ? " (then the jump through x22: fleh_dispatch64)" : "",
                        w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7], w[i + 8], w[i + 9], w[i + 10],
                        w[i + 11], w[i + 12], w[i + 13], w[i + 14], w[i + 15]);
                if (!disp) continue;
                found++, fr = r, fi = i + (uint64_t)bl;
            }
        }
        uint32_t *w = found == 1 ? hv_phys_ptr(M, xr[fr].pa, xr[fr].size) : NULL;
        if (!w || xr[fr].size - zs[fr] * 4 > 16384 || pools[fr] + 16 > xr[fr].size / 4) {
            fprintf(stderr, "gunyah: the Mac's clock at 24 MHz: the kernel's call of recount_leave_user was found %d times%s: the "
                            "Mac gets the counter's own rate (as Settings > Mac's clock: Exact)\n", found,
                    found == 1 ? ", and its code range has no page padding left for the wrapper" : " (once is needed)");
            g_tb24 = 0;
        } else {
            uint64_t site = xr[fr].pa + fi * 4, stub = xr[fr].pa + pools[fr] * 4;
            int64_t off = (int64_t)(w[fi] & 0x3ffffff);
            if (off & (1ll << 25)) off -= 1ll << 26;
            uint64_t target = site + (uint64_t)(off * 4);
            uint32_t *c = &w[pools[fr]];
            /* stp x29, x30, [sp, #-48]!; ESR and FAR to [sp, #16], ELR and SPSR to [sp, #32] (fleh_synchronous reads
               all four after the call; assembled with clang and copied); bl; the four back; ldp x29, x30; ret */
            static const uint32_t wr[16] = {0xa9bd7bfdu, 0xd5385210u, 0xd5386011u, 0xa90147f0u, 0xd5384030u, 0xd5384011u,
                                            0xa90247f0u, 0 /* bl */,   0xa94247f0u, 0xd5184030u, 0xd5184011u, 0xa94147f0u,
                                            0xd5185210u, 0xd5186011u, 0xa8c37bfdu, 0xd65f03c0u};
            memcpy(c, wr, sizeof wr);
            c[7] = 0x94000000u | (uint32_t)(((int64_t)(target - (stub + 28)) >> 2) & 0x3ffffff); /* bl recount_leave_user */
            w[fi] = 0x94000000u | (uint32_t)(((int64_t)(stub - site) >> 2) & 0x3ffffff);
            pools[fr] += 16;
            fprintf(stderr, "gunyah: the kernel's call of recount_leave_user (pa %#llx, to %#llx) keeps ESR, FAR, ELR and SPSR now "
                            "(wrapper at pa %#llx; around the call: %08x %08x %08x %08x | %08x %08x %08x %08x)\n",
                    (unsigned long long)site, (unsigned long long)target, (unsigned long long)stub, w[fi - 4], w[fi - 3],
                    w[fi - 2], w[fi - 1], w[fi + 1], w[fi + 2], w[fi + 3], w[fi + 4]);
        }
    }
    for (int r = 0; r < nxr; r++) {
        uint32_t *w = hv_phys_ptr(M, xr[r].pa, xr[r].size);
        if (!w || !zs[r]) continue;
        for (uint64_t i = 0; i < zs[r]; i++) {
            static const uint32_t ops[13] = {0xd53be040u, 0xd53be020u, 0xd53be0c0u, 0xd53be0a0u, 0xd53be340u, 0xd53be240u,
                                             0xd53be300u, 0xd53be200u, 0xd53be000u, 0xd51be340u, 0xd51be240u, 0xd51be300u,
                                             0xd51be200u};
            static const uint8_t kinds[13] = {1, 1, 1, 1, 1, 1, 2, 2, 5, 3, 3, 4, 4};
            uint32_t x = w[i], rt = x & 31;
            if ((x & 0xffdff000u) != 0xd51be000u) continue; /* MRS/MSR of an op0 3, op1 3, CRn 14 register */
            int op = 0;
            while (op < 13 && ops[op] != (x & ~31u)) op++;
            if (op == 13) continue;
            g_ct_seen[kinds[op]]++;
            if (!g_tb24) continue;
            if (rt == 31) { g_ct_left++; continue; } /* xzr: none seen; it would read or write the unscaled value */
            w[i] = 0x00007a00u | (uint32_t)op << 5 | rt;
            g_ct_done++;
        }
    }
    fprintf(stderr, "gunyah: kernel patched for this CPU: %llu reads and %llu writes of Apple's own registers (now 0 and "
                    "ignored), %llu MIDR reads (now Apple's %#llx)%s, %llu PAC hypercalls (now success), %llu SCTLR writes (pointer "
                    "authentication off)\n",
            (unsigned long long)impl_r, (unsigned long long)impl_w, (unsigned long long)midr, (unsigned long long)APPLE_MIDR,
            midr_left ? " (some left: no room for their stubs)" : "", (unsigned long long)pac, (unsigned long long)sctlr);

    fprintf(stderr, "gunyah: the kernel's generic timer: %llu reads of a counter or compare value, %llu of a count-down, %llu "
                    "writes of a compare value, %llu of a count-down, %llu reads of CNTFRQ; %s%s\n",
            (unsigned long long)g_ct_seen[1], (unsigned long long)g_ct_seen[2], (unsigned long long)g_ct_seen[3],
            (unsigned long long)g_ct_seen[4], (unsigned long long)g_ct_seen[5],
            g_tb24 ? "each is now a trap the exception fix-up carries out: the Mac's clock is 24 MHz (the counter's 19.2 MHz times 5/4)" : "as they are",
            g_ct_left ? " (but for some that name the zero register, left as they were)" : "");
    /* The timebase: the kernel's tree says the 24 MHz Henge's JIT counts; Gunyah gives the CPU's own counter. */
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    /* 0.3.113: XNU takes its microseconds as ticks / (rate / 1000000), a whole number: at the Mi Pad 7's 19.2 MHz that
       is 19, so a second counted 1,010,526 microseconds and the microsecond clock stepped back up to 10.5 ms at each
       second (0.3.111's log: "[53225.1001268]", "took -8650 us", "Spent too much time (18446744073709)s"). The kernel
       is told the nearest whole MHz instead (19): its clocks agree with each other and run 1% fast against the
       tablet's. HV_GUNYAH_TIMEBASE=exact (Settings > Mac's clock: Exact) tells it the counter's own rate, as before. */
    uint64_t counter_hz = f;
    const char *tbm = getenv("HV_GUNYAH_TIMEBASE");
    /* 0.3.116: exact unless asked (HV_GUNYAH_TIMEBASE=whole). The sound device is paced by the tablet's clock, so at
       19 MHz every 0.26 s buffer came back 2.7 ms "late" by the Mac's: coreaudiod re-anchored its clock each fourth one
       (10.7 ms, "host time is not consecutive") and reported an overload, once a second, for the whole of 0.3.115's run. */
    int tb_exact = !(tbm && !strcmp(tbm, "whole"));
    if (g_tb24) f = 24000000; /* 0.3.119: the kernel's counter reads are scaled to it */
    else if (!tb_exact && f % 1000000 && (f + 500000) / 1000000 > 0) f = (f + 500000) / 1000000 * 1000000;
    int changed = 0;
    if (dtva >= virt && dtlen > 8) {
        uint64_t dtpa = dtva - virt + phys;
        uint8_t *dt = hv_phys_ptr(M, dtpa, dtlen);
        if (dt) {
            /* Apple's device tree: a node is {u32 properties, u32 children}, then each property {char name[32],
               u32 length (bit 31: a placeholder), value padded to 4}, then its children. */
            uint8_t *p = dt, *end = dt + dtlen;
            uint32_t stack[64];
            int gics = 0, pcies = 0;
            int depth = 0;
            stack[0] = 1;
            while (depth >= 0 && p + 8 <= end) {
                if (!stack[depth]) { depth--; continue; }
                stack[depth]--;
                uint32_t np = rd32(p), nc = rd32(p + 4);
                uint8_t *pnode = p;
                p += 8;
                char nname[33] = "";
                uint8_t *nreg = NULL;
                uint32_t nreglen = 0;
                for (uint32_t i = 0; i < np && p + 36 <= end; i++) {
                    uint32_t len = rd32(p + 32) & 0x7fffffffu;
                    if (p + 36 + len > end) { p = end; break; }
                    if (!strncmp((const char *)p, "name", 32)) snprintf(nname, sizeof nname, "%.*s", (int)(len < 32 ? len : 32), (const char *)p + 36);
                    if (!strncmp((const char *)p, "reg", 32)) nreg = p + 36, nreglen = len;
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
                /* 0.3.91: the kernel's GIC (/arm-io/gic reg: GICD offset, size, GICR offset, size, from the SoC base
                   0x10000000), against where Gunyah has it: the shim found the redistributors at 0x10010000, 128 KB
                   apart (GICR_TYPER there 0, then vCPU 1 at 0x10030000, vCPU 2 at 0x10050000). The kernel's FAR,
                   0x80 past its SGI frame, faults on Gunyah while the shim's IGROUPR0 there works. */
                if (strstr(nname, "pci") && pcies++ < 4) pcie_node(nname, pnode, p);
                if (!strcmp(nname, "gic") && nreg && nreglen >= 32 && gics++ < 2) {
                    uint64_t g[4];
                    memcpy(g, nreg, 32);
                    fprintf(stderr, "gunyah: the kernel's GIC: distributor at +%#llx (%#llx bytes), redistributors at +%#llx "
                                    "(%#llx bytes)%s\n", (unsigned long long)g[0], (unsigned long long)g[1], (unsigned long long)g[2],
                            (unsigned long long)g[3], nreglen > 32 ? " and more" : "");
                    uint64_t want_r = VMAPPLE_GIC_REDIST - VMAPPLE_GIC_DIST, want_s = 0x20000ull * (uint64_t)hv_vm_ncpus();
                    if (g[2] != want_r || g[3] < want_s) {
                        g[2] = want_r;
                        if (g[3] < want_s) g[3] = want_s;
                        memcpy(nreg, g, 32);
                        fprintf(stderr, "gunyah: the kernel's GIC redistributors set to Gunyah's: +%#llx, %#llx bytes\n",
                                (unsigned long long)g[2], (unsigned long long)g[3]);
                    }
                }
                if (nc && depth < 63) stack[++depth] = nc;
            }
        }
    }
    if (g_tb24)
        fprintf(stderr, "gunyah: the kernel's timebase-frequency: %d changed, 24000000 Hz (the CPU's counter, %llu Hz, reaches the "
                        "kernel scaled by 5/4)\n", changed, (unsigned long long)counter_hz);
    else if (f == counter_hz)
        fprintf(stderr, "gunyah: the kernel's timebase-frequency: %d set to the CPU's %llu Hz%s\n", changed, (unsigned long long)f,
                f % 1000000 ? " (not a whole MHz: the Mac's microsecond clock steps back each second; Settings > Mac's clock)" : "");
    else
        fprintf(stderr, "gunyah: the kernel's timebase-frequency: %d set to %llu Hz, the whole MHz nearest the CPU's %llu Hz "
                        "(the Mac's clock runs %.2f%% %s, its sound device with the tablet's: Settings > Mac's clock)\n", changed,
                (unsigned long long)f, (unsigned long long)counter_hz,
                100.0 * (counter_hz > f ? (double)(counter_hz - f) / (double)f : (double)(f - counter_hz) / (double)f),
                counter_hz > f ? "fast" : "slow");
    if (pcie_moved && B->pcie) {
        int e = hv_pcie_alias(B->pcie, PCIE_ECAM_NEW, PCIE_ECAM_NEW_SIZE, PCIE_MMIO_NEW, PCIE_MMIO_NEW_SIZE);
        fprintf(stderr, "gunyah: PCIe moved below Gunyah's 1 GB VMMIO limit: ECAM %#llx, MMIO %#llx (%d values in the tree)%s\n",
                (unsigned long long)PCIE_ECAM_NEW, (unsigned long long)PCIE_MMIO_NEW, pcie_moved, e < 0 ? "; Henge's alias FAILED" : "");
    }
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
    if (i >= 32 && i < 72 && (i & 1)) { /* 0.3.89: the shim's look at Gunyah's GIC before the kernel's */
        static const char *probe[20] = {"GICD_CTLR", "GICD_TYPER", "GICR_TYPER (vCPU 0, 0x10010008)", "GICR_WAKER",
                                        "GICR_IGROUPR0 (SGI frame, 0x10020080) read", "GICR_IGROUPR0 write 0x81ffffff",
                                        "GICR_IGROUPR0 read back", "GICR_ISENABLER0 (0x10020100) read",
                                        "GICR_TYPER at 0x10030008 (vCPU 1 if the stride is 128K)",
                                        "GICR_TYPER at 0x10050008 (vCPU 1 if the stride is 256K)", "ICC_SRE_EL1",
                                        "ICC_BPR0_EL1 write 0", "ICC_PMR_EL1 write 0xff", "ICC_CTLR_EL1",
                                        "ICC_IGRPEN0_EL1 write 1", "ICC_IGRPEN0_EL1",
                                        "PCIe ECAM 0x40000000 (bus 0 device 0) read", "PCIe ECAM 0x40008000 (device 1) read",
                                        "PCIe MMIO 0x50000000 read", "PCIe ECAM moved, 0x31000000 (bus 0 device 0) read"};
        unsigned n = (i - 32) / 2;
        if (v >> 63)
            say("gunyah: shim: GIC probe %s: FAULTS (ESR %#llx)\n", probe[n], (unsigned long long)(v & ~(1ull << 63)));
        else
            say("gunyah: shim: GIC probe %s: %#llx\n", probe[n], (unsigned long long)shim_rep[i - 1]);
        return;
    }
    if (i == 10 && v && shim_rep[16] % 16 == 0) say("gunyah: shim: the resource manager's answer: error %#llx%s\n",
                                                   (unsigned long long)v, v == 6 ? " (ARGUMENT_INVALID)" : v == 2 ? " (NORESOURCE)" : "");
    if (i == 17) {
        char e[64] = "";
        if (shim_rep[18]) snprintf(e, sizeof e, " (doorbell_mask's first error %#llx)", (unsigned long long)shim_rep[18]);
        say("gunyah: shim: %llu interrupt doorbells now clear themselves once delivered%s (first capability %#llx)\n",
            (unsigned long long)v, e, (unsigned long long)shim_rep[19]);
    }
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
    else if ((w & 0x3b000000u) == 0x39000000u) { /* LDR/STR, unsigned offset: Gunyah can emulate (ISV=1) */
        unsigned sz = w >> 30, opc = w >> 22 & 3, rn = w >> 5 & 31, rt = w & 31, imm = (w >> 10 & 0xfff) << sz;
        snprintf(out, cap, "%s %c%u, [x%u, #%#x] (%u bytes)", opc ? "LDR" : "STR", sz == 3 ? 'x' : 'w', rt, rn, imm, 1u << sz);
    } else if ((w & 0x3b200400u) == 0x38000400u) { /* pre/post-indexed: writeback, ISV=0 */
        unsigned sz = w >> 30, opc = w >> 22 & 3, rn = w >> 5 & 31, rt = w & 31;
        snprintf(out, cap, "%s %c%u, [x%u] %s-indexed (%u bytes, WRITEBACK: no syndrome for a hypervisor)", opc ? "LDR" : "STR",
                 sz == 3 ? 'x' : 'w', rt, rn, w >> 11 & 1 ? "pre" : "post", 1u << sz);
    } else if ((w & 0x3b200c00u) == 0x38200800u) {
        unsigned sz = w >> 30, opc = w >> 22 & 3, rn = w >> 5 & 31, rt = w & 31, rm = w >> 16 & 31;
        snprintf(out, cap, "%s %c%u, [x%u, x%u] (%u bytes)", opc ? "LDR" : "STR", sz == 3 ? 'x' : 'w', rt, rn, rm, 1u << sz);
    } else if ((w & 0x3a000000u) == 0x28000000u)
        snprintf(out, cap, "%s pair, [x%u] (LDP/STP: no syndrome for a hypervisor)", w >> 22 & 1 ? "LDP" : "STP", w >> 5 & 31);
    else if ((w & 0x7f800000u) == 0x12800000u) snprintf(out, cap, "MOVN w/x%u, #%#x, lsl #%u", w & 31, w >> 5 & 0xffff, (w >> 21 & 3) * 16);
    else if ((w & 0x7f800000u) == 0x11000000u) snprintf(out, cap, "ADD x%u, x%u, #%#x%s", w & 31, w >> 5 & 31, w >> 10 & 0xfff, w >> 22 & 1 ? ", lsl #12" : "");
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

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void uart_char(char c) {
    uart_last_ns = now_ns();
    if (c != '\n' && uart_n < (int)sizeof uart_line - 1) { uart_line[uart_n++] = c; return; }
    uart_line[uart_n] = 0;
    uart_n = 0;
    const char *keys[] = {"at pc 0x", "pc:  0x", "pc: 0x"};
    for (int k = 0; k < 3; k++) {
        const char *s = strstr(uart_line, keys[k]);
        if (s) panic_pc(strtoull(s + strlen(keys[k]), NULL, 16));
    }
    /* 0.3.91: a kernel exception's panic names its saved state ("state=0x..."): struct arm_saved_state, header 8
       bytes, then x0-x28, fp, lr, sp, pc, cpsr, far, esr. */
    const char *st = strstr(uart_line, "state=0x");
    static int states;
    if (st && states < 2) {
        uint64_t va = strtoull(st + 8, NULL, 16);
        const uint64_t *ss;
        if (kva_ok(va, 296, (const void **)&ss)) {
            states++;
            const uint64_t *r = ss + 1;
            say("gunyah: the faulting state: pc %#llx lr %#llx sp %#llx far %#llx esr %#llx cpsr %#llx\n", (unsigned long long)r[32],
                (unsigned long long)r[30], (unsigned long long)r[31], (unsigned long long)r[34], (unsigned long long)r[35],
                (unsigned long long)(r[33] & 0xffffffffu));
            for (int i = 0; i < 29; i += 4)
                say("gunyah:   x%-2d %#018llx x%-2d %#018llx x%-2d %#018llx x%-2d %#018llx\n", i, (unsigned long long)r[i], i + 1,
                    (unsigned long long)(i + 1 < 29 ? r[i + 1] : 0), i + 2, (unsigned long long)(i + 2 < 29 ? r[i + 2] : 0), i + 3,
                    (unsigned long long)(i + 3 < 29 ? r[i + 3] : 0));
            panic_pc(r[32]);
            panic_pc((r[30] & 0x0000ffffffffffffull) | 0xffff000000000000ull);
        }
    }
    /* 0.3.93: a panic's backtrace (its "lr: 0x... fp: 0x..." lines): the frame after the exception's (the return
       address that shows up twice) called the code that faulted. Its call and the called function are logged. */
    static uint64_t bt[40];
    static int nbt, bt_done;
    const char *lrp = strstr(uart_line, "lr: 0x");
    if (lrp && strstr(uart_line, "fp: 0x") && nbt < 40) bt[nbt++] = strtoull(lrp + 6, NULL, 16);
    if (!bt_done && nbt > 2 && strstr(uart_line, "Kernel Extensions in backtrace")) {
        bt_done = 1;
        for (int k = 1; k < nbt - 1; k++) {
            int rep = 0;
            for (int j = 0; j < k; j++) rep |= bt[j] == bt[k];
            if (!rep) continue;
            uint64_t ret = bt[k + 1];
            say("gunyah: the faulting code was called from %#llx (the frame after the exception's):\n", (unsigned long long)ret);
            panic_pc(ret - 4);
            const uint32_t *cw;
            if (kva_ok(ret - 4, 4, (const void **)&cw) && (*cw & 0xfc000000u) == 0x94000000u) {
                int64_t off = (int64_t)(*cw & 0x3ffffff);
                if (off & (1ll << 25)) off -= 1ll << 26;
                uint64_t fn = ret - 4 + (uint64_t)(off * 4);
                const uint32_t *fw;
                if (kva_ok(fn, 4 * 32, (const void **)&fw)) {
                    say("gunyah: the function it called, at %#llx:\n", (unsigned long long)fn);
                    for (int i = 0; i < 32; i++) {
                        char d[128];
                        decode_word(fw[i], d, sizeof d);
                        say("gunyah:      %#llx: %08x  %s\n", (unsigned long long)(fn + 4 * (uint64_t)i), fw[i], d);
                    }
                }
            }
            break;
        }
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
/* 0.3.88: 0.3.87 found no "panic(cpu" text anywhere in the Mac's RAM, yet "MACH Reboot" is the panic path's restart:
   this early the panic log has no buffer and its text goes only to the video console, drawn as pixels into the
   framebuffer XNU named in "initialize_screen". That framebuffer is written to a picture the user can send. */

static void dump_screen(void) {
    if (!fb_pa || !fb_w || !fb_h || fb_w > 8192 || fb_h > 8192 || fb_rb < fb_w * 4) return;
    const uint8_t *fb = hv_phys_ptr(M, fb_pa, (uint64_t)fb_rb * fb_h);
    if (!fb) { say("gunyah: the Mac's screen at pa %#llx is not in its RAM\n", (unsigned long long)fb_pa); return; }
    const char *dirs[] = {"/storage/emulated/0/Henge", "/sdcard/Henge", "/data/local/tmp"};
    for (int d = 0; d < 3; d++) {
        char path[128];
        snprintf(path, sizeof path, "%s/gunyah-screen.bmp", dirs[d]);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        uint32_t img = fb_w * 4 * fb_h, hdr[13] = {0};
        uint8_t bm[2] = {'B', 'M'};
        hdr[0] = 54 + img;                /* file size */
        hdr[2] = 54;                      /* pixels' offset */
        hdr[3] = 40;                      /* BITMAPINFOHEADER */
        hdr[4] = fb_w;
        hdr[5] = (uint32_t)-(int32_t)fb_h; /* top-down */
        hdr[6] = 1 | 32u << 16;           /* planes, bits */
        hdr[8] = img;
        fwrite(bm, 1, 2, f);
        fwrite(hdr, 4, 13, f);
        for (uint32_t y = 0; y < fb_h; y++) fwrite(fb + (uint64_t)y * fb_rb, 4, fb_w, f);
        fclose(f);
        chmod(path, 0644);
        static int told;
        if (!told++)
            say("gunyah: the Mac's screen (%ux%u at pa %#llx) is in %s (written again every 30 s): send it\n", fb_w, fb_h,
                (unsigned long long)fb_pa, path);
        return;
    }
    say("gunyah: could not write the Mac's screen to a picture\n");
}

/* 0.3.120: where a stalled kernel is, as far as its memory tells. Gunyah shows no vCPU register; but a kernel that
   faults over and over, or was interrupted, leaves return addresses in its data (saved ELR and LR, the stacks). Every
   8-byte value in the kernel's data that is the kernel address of a timer stub or of a fix-up is counted. */
static void stall_stubs(void) {
    if (!g_nts || !k_phys || k_top <= k_phys) return;
    const uint8_t *k = hv_phys_ptr(M, k_phys, k_top - k_phys);
    if (!k) return;
    struct { uint64_t va; unsigned n; } hit[24];
    int nh = 0;
    uint64_t all = 0;
    for (uint64_t o = 0; o + 8 <= k_top - k_phys; o += 8) {
        uint64_t v = rd64(k + o);
        if (v < k_virt || (v & 3)) continue;
        uint64_t pa = v - k_virt + k_phys;
        int in = 0;
        for (int i = 0; i < g_nts && !in; i++) in = pa >= g_ts[i].pa && pa < g_ts[i].end;
        if (!in) continue;
        all++;
        int j = 0;
        while (j < nh && hit[j].va != v) j++;
        if (j < nh) hit[j].n++;
        else if (nh < 24) hit[nh].va = v, hit[nh].n = 1, nh++;
    }
    say("gunyah: stall: %llu values in the kernel's data point into the exception fix-ups%s\n", (unsigned long long)all,
        all ? ":" : " (no fault or interrupt was taken inside one)");
    for (int j = 0; j < nh; j++) {
        uint64_t pa = hit[j].va - k_virt + k_phys;
        int fix = 0;
        for (int i = 0; i < g_nts; i++) if (pa >= g_ts[i].pa && pa < g_ts[i].end) fix = g_ts[i].fix;
        const uint32_t *c = hv_phys_ptr(M, pa & ~3ull, 32);
        if (!c) continue;
        say("gunyah: stall:   %#llx (pa %#llx, %s) %u times; the code there: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            (unsigned long long)hit[j].va, (unsigned long long)pa, fix ? "a fix-up" : "a stub", hit[j].n, c[0], c[1], c[2], c[3],
            c[4], c[5], c[6], c[7]);
    }
}

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
        /* The screen's place: the last initialize_screen anywhere in the buffer (0.3.101's tail held only timer spam). */
        for (uint32_t i = 0; i + 64 < size; i++) {
            if (buf[i] != 'i' || memcmp(buf + i, "initialize_screen: b=", 21)) continue;
            char t[96];
            memcpy(t, buf + i, sizeof t - 1);
            t[sizeof t - 1] = 0;
            unsigned long long b, w, h, rb;
            if (sscanf(t, "initialize_screen: b=%llx, w=%llx, h=%llx, r=%llx", &b, &w, &h, &rb) == 4 && w && h)
                fb_pa = b, fb_w = (uint32_t)w, fb_h = (uint32_t)h, fb_rb = (uint32_t)rb;
        }
    }
    if (!found) say("gunyah: no kernel log (msgbuf) found in the kernel's data yet\n");
    stall_stubs();
    /* 0.3.87: 0.3.86's log ended "failed to find the timer" (harmless: the JIT's boots print it too), then "MACH Reboot"
       a quarter millisecond later: that is the panic path's restart (kdp_machine_reboot_type), and a panic's text goes
       to the panic buffer and the console, not to msgbuf. Every "panic(cpu" in the Mac's RAM is logged with what
       follows it. */
    static const char key[] = "panic(cpu ";
    const uint8_t *ram = B->ram, *end = B->ram + B->ram_size, *p = ram;
    int hits = 0, old_reports = 0;
    while (hits < 6 && old_reports < 64 && (p = memmem(p, (size_t)(end - p), key, sizeof key - 1)) != NULL) {
        if (p[sizeof key - 1] == '%') { p++; continue; } /* the kernel's own format string */
        char out[2049];
        size_t m = 0;
        for (const uint8_t *q = p; q < end && m < 2048 && *q; q++)
            out[m++] = (*q >= 32 && *q < 127) || *q == '\n' || *q == '\t' ? (char)*q : '?';
        out[m] = 0;
        p += m > 0 ? m : 1;
        /* 0.3.109: a report kept on disk (its JSON escapes, "\n\t" as two characters) is a past panic recoveryOS read
           in, not this boot's: counted, not printed (0.3.107 and 0.3.108 logged the same old one six times a pass). */
        if (strstr(out, "\\n\\t") || strstr(out, "\\n")) { old_reports++; continue; }
        say("gunyah: panic text in the Mac's RAM at pa %#llx:\n---\n%s\n---\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)(p - m - ram)), out);
        hits++;
    }
    if (!hits) say("gunyah: no panic of this boot in the Mac's RAM%s\n", old_reports ? " (only past panic reports read from disk)" : "");
    const uint8_t *d = ram;
    while ((d = memmem(d, (size_t)(end - d), "Debugger called: <", 18)) != NULL && d[18] == '%') d++;
    if (d) {
        char out[257];
        size_t m = 0;
        for (const uint8_t *q = d; q < end && m < 256 && *q >= 32 && *q < 127; q++) out[m++] = (char)*q;
        out[m] = 0;
        say("gunyah: in the Mac's RAM at pa %#llx: %s\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)(d - ram)), out);
    }
    dump_screen();
}

/* ---- 0.3.98: what the kernel is doing when its serial output stops ----
   0.3.97 stopped printing right after PCI configuration while CPU 0 went on reading two device registers (0x32001000
   and 0x32019000, offset 0x1000 of two virtio devices' BAR: the ISR) 150,000 times a second. The addresses each vCPU
   reads and writes most are counted with the last value; after 5 s of silence on the serial port (and every 60 s
   of it) the watch thread names them (device, BAR offset), shows every PCI device's state (virtio status, ISR, its
   INTx line, its queues: what the driver made available against what the device took and handed back), the
   doorbell rings per line, how often the device thread ran (it hands back completions), and the kernel's own log
   from its memory, which holds what the serial port had not printed yet. */
#define HOT 6
static struct { uint64_t pa, n, v; int w; } hot[HV_MAX_CPUS][HOT];
static void hot_note(int id, uint64_t pa, uint64_t v, int w) {
    int k, min = 0;
    for (k = 0; k < HOT; k++) {
        if (hot[id][k].pa == pa && hot[id][k].n) { hot[id][k].n++, hot[id][k].v = v, hot[id][k].w = w; return; }
        if (hot[id][k].n < hot[id][min].n) min = k;
    }
    hot[id][min].pa = pa, hot[id][min].n = 1, hot[id][min].v = v, hot[id][min].w = w;
}

/* Henge's virtio devices: 1af4:104x, and Apple's own IDs for the disks (106b:1a00, virtio_blk.c's apple_variant). */
static int pci_is_virtio(const HvPciDev *d) {
    uint16_t ven = (uint16_t)(d->config[0] | d->config[1] << 8);
    return ven == 0x1af4 || ven == 0x106b;
}

static const char *pci_name(const HvPciDev *d, char *buf, size_t n) {
    uint16_t ven = (uint16_t)(d->config[0] | d->config[1] << 8), dev = (uint16_t)(d->config[2] | d->config[3] << 8);
    const char *what = "?";
    if (ven == 0x1af4) {
        switch (dev - 0x1040) {
        case 1: what = "virtio-net"; break;
        case 2: what = "virtio-blk"; break;
        case 18: what = "virtio-input"; break;
        case 25: what = "virtio-snd"; break;
        case 5: what = "virtio-balloon"; break;
        default: what = "virtio"; break;
        }
    } else if (ven == 0x106b) what = "virtio-blk (Apple's IDs)";
    else if ((d->config[11] == 0x0c && d->config[10] == 0x03)) what = "xhci";
    snprintf(buf, n, "slot %d %s %04x:%04x", d->slot, what, ven, dev);
    return buf;
}

static const HvPciDev *pci_at(uint64_t pa, int *bar, uint64_t *off) {
    HvPcie *p = B->pcie;
    if (!p) return NULL;
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        const HvPciDev *d = p->dev[s];
        if (!d) continue;
        for (int b = 0; b < 6; b++) {
            if (!d->bar_size[b]) continue;
            uint64_t a = hv_pci_bar_addr((HvPciDev *)d, b);
            if (a && pa >= a && pa < a + d->bar_size[b]) { *bar = b, *off = pa - a; return d; }
        }
    }
    return NULL;
}

static void silence_report(double quiet_s) {
    say("gunyah: the kernel has printed nothing for %.0f s while its CPUs run: what they are doing\n", quiet_s);
    for (int id = 0; id < nvcpu; id++) {
        for (int k = 0; k < HOT; k++) {
            if (!hot[id][k].n) continue;
            int bar = -1;
            uint64_t off = 0;
            char nm[64];
            const HvPciDev *d = pci_at(hot[id][k].pa, &bar, &off);
            const char *reg = "";
            if (d && pci_is_virtio(d))
                reg = off < 0x1000 ? " (common config)" : off < 0x2000 ? " (ISR)" : off < 0x3000 ? " (device config)" : " (notify)";
            say("gunyah:   vCPU %d %s %#llx %llu times (last %#llx)%s%s%s BAR%d+%#llx%s\n", id, hot[id][k].w ? "stores to" : "loads from",
                (unsigned long long)hot[id][k].pa, (unsigned long long)hot[id][k].n, (unsigned long long)hot[id][k].v,
                d ? ": " : "", d ? pci_name(d, nm, sizeof nm) : "", d ? "" : "", bar, (unsigned long long)off, reg);
            hot[id][k].n = 0;
        }
    }
    HvPcie *p = B->pcie;
    for (int s = 0; p && s < HV_PCI_SLOTS; s++) {
        const HvPciDev *d = p->dev[s];
        if (!d) continue;
        char nm[64], q[512];
        int line = p->intx_base + d->slot, n = 0;
        uint16_t cmd = (uint16_t)(d->config[4] | d->config[5] << 8);
        q[0] = 0;
        if (pci_is_virtio(d)) {
            const HvVirtio *v = d->opaque;
            n += snprintf(q + n, sizeof q - (size_t)n, " status %#x isr %#x", v->status, v->isr);
            for (int i = 0; i < v->nq && n < (int)sizeof q - 80; i++) {
                const HvVirtq *vq = &v->q[i];
                if (!vq->enabled) continue;
                const uint16_t *av = hv_phys_ptr(M, vq->avail, 4), *us = hv_phys_ptr(M, vq->used, 4);
                n += snprintf(q + n, sizeof q - (size_t)n, " | q%d size %u: driver made %u available, device took %u, handed back %u", i,
                              vq->size, av ? av[1] : 0, vq->last_avail, us ? us[1] : 0);
            }
        }
        say("gunyah:   %s: BAR %#llx, command %#x, INTx %d -> interrupt %d (%s, %llu rings)%s\n", pci_name(d, nm, sizeof nm),
            (unsigned long long)hv_pci_bar_addr((HvPciDev *)d, 4) ? (unsigned long long)hv_pci_bar_addr((HvPciDev *)d, 4)
                                                                    : (unsigned long long)hv_pci_bar_addr((HvPciDev *)d, 0),
            cmd, d->intx, line, line < NIRQ && irq_level[line] ? "high" : "low", line < NIRQ ? (unsigned long long)rings[line] : 0ull, q);
    }
    char r[512];
    int n = snprintf(r, sizeof r, "gunyah:   doorbell rings:");
    for (int i = 32; i < NIRQ && n < (int)sizeof r - 24; i++)
        if (rings[i]) n += snprintf(r + n, sizeof r - (size_t)n, " %d:%llu%s", i, (unsigned long long)rings[i], irq_level[i] ? "^" : "");
    say("%s; the device thread ran %llu times\n", r, (unsigned long long)dev_ticks);
    dump_msgbuf();
}

/* ---- 0.3.102: the kernel's log, followed ----
   0.3.101 reached launchd ("BaseSystem environment starting", boot task mount-phase-1), then the serial port went quiet
   and the CPUs idled while the kernel's own log (msgbuf) kept growing; its tail was all CoreAnalyticsHub's timer line.
   Once the serial port has been quiet 3 s, each new line of msgbuf is copied here ("mac:"), but for lines seen often
   (the timer's), each counted instead: one line every 30 s says how many. */
static uint64_t mb_pa;      /* msgbuf's header in the Mac's RAM */
static uint32_t mb_seen;    /* its write index as far as copied */
static int mb_started;
static void mb_find(void) {
    if (mb_pa || !k_phys || k_top <= k_phys) return;
    const uint8_t *k = hv_phys_ptr(M, k_phys, k_top - k_phys);
    if (!k) return;
    for (uint64_t o = 0; o + 24 <= k_top - k_phys; o += 8) {
        if (rd32(k + o) != 0x063061u) continue;
        uint32_t size = rd32(k + o + 4), bufx = rd32(k + o + 8);
        uint64_t bufc = rd64(k + o + 16);
        if (size < 4096 || size > (64u << 20) || bufx >= size || bufc < k_virt) continue;
        mb_pa = k_phys + o;
        return;
    }
}
/* 0.3.110: whether the kernel's last word before the VM exited was a restart. XNU's halt_all_cpus prints "MACH Reboot"
   for a restart (a chosen startup disk, a panic's restart) and "CPU halted" for a shut down; Gunyah ends the VM on the
   guest's PSCI SYSTEM_RESET and SYSTEM_OFF alike (status 2, "exited"), so the log says which it was. 1: restart. */
static int kernel_said_reboot(void) {
    mb_find();
    if (!mb_pa) return 0;
    const uint8_t *h = hv_phys_ptr(M, mb_pa, 24);
    if (!h) return 0;
    uint32_t size = rd32(h + 4), bufx = rd32(h + 8);
    uint64_t bufc = rd64(h + 16);
    const char *buf = bufc >= k_virt ? hv_phys_ptr(M, bufc - k_virt + k_phys, size) : NULL;
    if (!buf || bufx >= size) return 0;
    /* the most recent of each, by distance back from the write index (the buffer is a ring) */
    uint64_t reboot_age = UINT64_MAX, halt_age = UINT64_MAX;
    for (uint32_t i = 0; i + 11 <= size; i++) {
        if (buf[i] == 'M' && !memcmp(buf + i, "MACH Reboot", 11)) {
            uint64_t age = (uint64_t)((bufx + size - i) % size);
            if (age < reboot_age) reboot_age = age;
        } else if (buf[i] == 'C' && i + 10 <= size && !memcmp(buf + i, "CPU halted", 10)) {
            uint64_t age = (uint64_t)((bufx + size - i) % size);
            if (age < halt_age) halt_age = age;
        }
    }
    say("gunyah: the kernel's log ends in %s\n", reboot_age < halt_age ? "\"MACH Reboot\" (a restart)"
        : halt_age != UINT64_MAX ? "\"CPU halted\" (a shut down)" : "neither \"MACH Reboot\" nor \"CPU halted\"");
    return reboot_age < halt_age;
}

static void mb_follow(int force_start) {
    static uint32_t spam_n;
    static time_t spam_t;
    mb_find();
    if (!mb_pa) return;
    const uint8_t *h = hv_phys_ptr(M, mb_pa, 24);
    if (!h) return;
    uint32_t size = rd32(h + 4), bufx = rd32(h + 8);
    uint64_t bufc = rd64(h + 16);
    const char *buf = bufc >= k_virt ? hv_phys_ptr(M, bufc - k_virt + k_phys, size) : NULL;
    if (!buf || bufx >= size) return;
    if (!mb_started) {
        if (!force_start) { mb_seen = bufx; return; }
        mb_started = 1;
        say("gunyah: the serial port is quiet: the kernel's own log follows (\"mac:\")\n");
    }
    uint32_t n = (bufx + size - mb_seen) % size, lines = 0, i;
    static char line[512]; /* a line half-written when read is finished next time */
    static int m;
    for (i = 0; i < n && lines < 120; i++) {
        char c = buf[(mb_seen + i) % size];
        if (c && c != '\n' && m < (int)sizeof line - 1) { line[m++] = (c >= 32 && c < 127) || c == '\t' ? c : '?'; continue; }
        if (c != '\n') continue;
        line[m] = 0;
        m = 0;
        if (strstr(line, "handleNagTimerExpiry")) { spam_n++; continue; }
        if (line[0]) say("mac: %s\n", line), lines++;
    }
    mb_seen = (mb_seen + i) % size;
    if (spam_n && time(NULL) - spam_t >= 30) {
        say("mac: (CoreAnalyticsHub's timer line %u more times)\n", spam_n);
        spam_n = 0, spam_t = time(NULL);
    }
}

static void *watch_thread(void *arg) {
    (void)arg;
    thread_altstack();
    while (!stop_req && !mac_runs) usleep(10000);
    uint64_t last0 = ~0ull;
    int still = 0, dumps = 0, quiet_reports = 0, in_quiet = 0, probed = 0;
    double quiet_at = 0;
    for (int s = 1; !stop_req && s <= 86400; s++) {
        sleep(1);
        if (mmio_exits[0] == last0) still++;
        else still = 0, last0 = mmio_exits[0];
        if ((still == 3 && dumps < 3) || (still && still % 60 == 0 && dumps < 6)) dump_msgbuf(), dumps++;
        double quiet = uart_last_ns ? (double)(now_ns() - uart_last_ns) / 1e9 : 0;
        mb_follow(quiet >= 3);
        /* 0.3.102: what the Mac shows, every 30 s (gunyah-screen.bmp). 0.3.117: for the first 5 minutes, then every 10
           (a 3 MB file to the tablet's storage each time, beside the Mac's own disk work). */
        if (s % 30 == 0 && (s <= 300 || s % 600 == 0)) dump_screen();
        if (quiet >= 5 && !still) {
            if ((!in_quiet || quiet - quiet_at >= 60) && quiet_reports < 4) silence_report(quiet), quiet_reports++, quiet_at = quiet;
            /* 0.3.99: still stuck 45 s on: one of vCPU 0's polling reads gets an abort, so the kernel panics with a
               backtrace of the loop (HV_GUNYAH_NO_PROBE=1: never). */
            if (quiet >= 45 && !probed && !getenv("HV_GUNYAH_NO_PROBE")) {
                int best = -1;
                for (int k = 0; k < HOT; k++)
                    if (!hot[0][k].w && hot[0][k].n > 1000 && (best < 0 || hot[0][k].n > hot[0][best].n)) best = k;
                if (best >= 0) fault_pa = hot[0][best].pa, probed = 1;
            }
            in_quiet = 1;
        } else if (quiet < 5) in_quiet = 0;
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
/* ---- caches: Gunyah maps the Mac's RAM without stage-2 forced write-back (no S2FWB), so a vCPU running with its MMU
   off (a secondary from PSCI CPU_ON, through reset_vector, start_cpu and common_start) reads memory past the caches.
   XNU cleans only cpu_data and CpuDataEntries before CPU_ON; ResetHandlerData, const_boot_args and cpu_ttep were written
   with the caches on and may still sit dirty in a core's cache: the new CPU reads stale values, takes TTBR1 from the
   stale cpu_ttep and dies before its first device access (0.3.95: "cpu 1 failed to boot", vcpu1 0 exits). Apple's
   hypervisor forces write-back, so XNU never needed more. The host cleans those lines to memory: DC by virtual address
   from EL0 (Linux sets SCTLR_EL1.UCI) works on the physical line, in every core's cache. ---- */

static unsigned dline(void) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4u << ((ctr >> 16) & 15);
}

static void dc_range(const uint8_t *p, size_t len, int inval) {
    unsigned l = dline();
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(l - 1), e = (uintptr_t)p + len;
    if (inval)
        for (; a < e; a += l) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    else
        for (; a < e; a += l) __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* All of the Mac's RAM the host has pages for (mincore), cleaned and invalidated: what the JIT and the patches wrote,
   for the boot CPU's own start with its MMU off too. */
struct clean_part { uint8_t *p; size_t len; uint64_t done; };
static void *clean_worker(void *arg) {
    struct clean_part *c = arg;
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), win = 2u << 20;
    unsigned char *vec = malloc(win / pg + 1);
    if (!vec) return NULL;
    for (size_t o = 0; o < c->len; o += win) {
        size_t n = c->len - o < win ? c->len - o : win;
        if (mincore(c->p + o, n, vec) != 0) continue;
        for (size_t i = 0; i < (n + pg - 1) / pg; i++)
            if (vec[i] & 1) dc_range(c->p + o + i * pg, pg, 1), c->done += pg;
    }
    free(vec);
    return NULL;
}

static long clean_all_ram_quiet(int quiet);
static void clean_all_ram(void) { clean_all_ram_quiet(0); }
static long clean_all_ram_quiet(int quiet) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    enum { W = 4 };
    struct clean_part part[W];
    pthread_t t[W];
    size_t step = (B->ram_size / W + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);
    int nt = 0;
    for (int i = 0; i < W; i++) {
        size_t o = (size_t)i * step;
        if (o >= B->ram_size) break;
        part[i] = (struct clean_part){B->ram + o, B->ram_size - o < step ? B->ram_size - o : step, 0};
        if (pthread_create(&t[nt], NULL, clean_worker, &part[i]) == 0) nt++;
        else clean_worker(&part[i]);
    }
    uint64_t done = 0;
    for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);
    for (int i = 0; i < W && (size_t)i * step < B->ram_size; i++) done += part[i].done;
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
    if (!quiet)
        fprintf(stderr, "gunyah: %llu MB of the Mac's RAM cleaned from the caches to memory in %ld ms (Gunyah has no forced "
                        "write-back: a CPU with its MMU off reads memory, not caches)\n", (unsigned long long)(done >> 20), ms);
    return ms;
}

/* The kernel's image and data (physBase to topOfKernelData), cleaned again and again while the Mac runs: every 50 ms
   for the first 3 minutes (secondaries start, each reading what CPU 0 wrote long before), then every second (a CPU
   coming back from a powered-down idle comes through the reset vector the same way). */
static void *clean_thread(void *arg) {
    (void)arg;
    thread_altstack();
    uint64_t lo = k_phys & ~0x3fffull, hi = k_top;
    if (hi <= lo || hi - lo > (1ull << 30)) return NULL;
    const uint8_t *p = hv_phys_ptr(M, lo, hi - lo);
    if (!p) return NULL;
    struct timespec t0, a, b;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int n = 0; !stop_req; n++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        dc_range(p, hi - lo, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long us = (long)((b.tv_sec - a.tv_sec) * 1000000 + (b.tv_nsec - a.tv_nsec) / 1000);
        if (n == 0)
            fprintf(stderr, "gunyah: the kernel's %llu MB (pa %#llx-%#llx) is cleaned to memory all along (%ld us a pass)\n",
                    (unsigned long long)((hi - lo) >> 20), (unsigned long long)lo, (unsigned long long)hi, us);
        long ms = b.tv_sec - t0.tv_sec < 180 ? 50 : 1000;
        if (ms < us / 100) ms = us / 100; /* never more than 10% of a core */
        for (long s = 0; s < ms && !stop_req; s += 10) usleep(10000);
    }
    return NULL;
}

/* The reset vector's report (kpatch): which = 0, the CPU's MPIDR as it arrives; 8, the CPU table pointer as it read it
   with its caches off, set against what memory (through the host's cached view) holds. */
static int rv_cleaned[HV_MAX_CPUS]; /* this CPU start's RAM clean is done (first report seen) */
static void rv_report(int id, uint64_t which, uint64_t v) {
    static int n;
    n++;
    if (which == 0) {
        /* 0.3.100: XNU's flush_dcache is only a DSB on Apple platforms (__ARM_COHERENT_CACHE__: Apple's cores, and
           Apple's hypervisor with forced write-back, keep caches-off reads coherent), so cpu_start's "flush" of the
           new CPU's cpu_data and CpuDataEntries cleans nothing. Those were written just before CPU_ON, after the last
           periodic clean: the CPU, caches off, read stale reset-handler and stack fields and hung on reset_vector's
           "b ." (0.3.99: CPUs 1 and 3 never came up, CPU 2 did in one run, none in the other; "cpu 1 failed to boot"
           once XNU checked, at the root mount). This stop is the CPU's first store, before it reads any of that: the
           whole of the Mac's RAM is cleaned to memory now, and the CPU goes on only after. */
        long ms = clean_all_ram_quiet(1);
        rv_cleaned[id] = 1;
        if (n <= 8)
            fprintf(stderr, "gunyah: vCPU %d reached the kernel's reset vector (MPIDR %#llx): CPU_ON started it; the Mac's RAM "
                            "cleaned to memory for it (%ld ms)\n", id, (unsigned long long)v, ms);
        return;
    }
    if (!rv_cleaned[id]) { /* its first report did not arrive: clean now, still before it reads its CPU data */
        long ms = clean_all_ram_quiet(1);
        fprintf(stderr, "gunyah: vCPU %d is at the kernel's reset vector (its first report missing); the Mac's RAM cleaned "
                        "to memory for it (%ld ms)\n", id, ms);
    }
    rv_cleaned[id] = 0;
    if (n > 16) return;
    const uint8_t *p = rv_rhd ? hv_phys_ptr(M, rv_rhd, 8) : NULL;
    uint64_t mem = p ? rd64(p) : 0;
    fprintf(stderr, "gunyah: vCPU %d, caches off, reads the kernel's CPU table pointer as %#llx; the caches hold %#llx%s\n", id,
            (unsigned long long)v, (unsigned long long)mem,
            v == mem ? " (the same)" : " (STALE: memory was behind the caches)");
}

static uint64_t kicks_back[HV_MAX_CPUS];
static uint64_t saved_exits[HV_MAX_CPUS]; /* exits that came back with a signal (0.3.101: once lost) */
/* 0.3.103: a vCPU is kicked only when it has been inside one GH_VCPU_RUN for 20 ms or more (a long run, or a WFI
   wait). The driver takes a load's answer from the run area when the run starts and drops it if a signal is already
   pending then (it returns -ERESTARTSYS before running the vCPU, and the next run resumes the load with 0): a kick
   landing between an exit's handling and the next run turned device reads into zeros (an ISR read 0: a disk
   completion missed, APFS waiting for good, and after 60 s with no root device the kernel's "About to reboot into
   Recovery!", 0.3.102). A vCPU busy with device accesses is left alone now; it comes back to the host on each of
   them anyway. */
static volatile uint64_t run_since[HV_MAX_CPUS]; /* when this vCPU's current GH_VCPU_RUN began (0: not in one) */
static uint64_t kicks_sent[HV_MAX_CPUS];
/* 0.3.127: "the sound stops when the mouse stops moving". What the engine can see of an idle Mac, every 30 s: how
   long a vCPU stayed inside one GH_VCPU_RUN at most (a WFI wait that nothing ended), how late this 2 ms kicker and the
   1 ms device thread ran at most (both keep to the slow cores since 0.3.125), and the kicks sent. */
static volatile uint64_t stay_max[HV_MAX_CPUS], stay_long[HV_MAX_CPUS], kick_late_max;
static void *kick_thread(void *arg) {
    (void)arg;
    thread_altstack();
    uint64_t said_at = now_ns(), kicks_was = 0;
    while (!stop_req) {
        uint64_t before = now_ns();
        usleep((useconds_t)kick_ms * 1000);
        uint64_t now = now_ns();
        if (now - before > kick_late_max) kick_late_max = now - before;
        if (mac_runs && now - said_at >= 30000000000ull) {
            char line[400];
            int n = 0;
            uint64_t kicks = 0;
            for (int i = 0; i < nvcpu && n < (int)sizeof line - 48; i++) {
                n += snprintf(line + n, sizeof line - (size_t)n, " vcpu%d %.0f ms (%llu of 100 ms or more)", i, stay_max[i] / 1e6,
                              (unsigned long long)stay_long[i]);
                stay_max[i] = 0, stay_long[i] = 0, kicks += kicks_sent[i];
            }
            say("gunyah: last 30 s: the longest a CPU stayed in the Mac without coming back:%s; %llu kicks; the kicker's longest "
                "sleep %.1f ms (2 asked), the device thread's longest pause %.1f ms (1 asked)\n", line,
                (unsigned long long)(kicks - kicks_was), kick_late_max / 1e6, dev_gap_max / 1e6);
            kicks_was = kicks, kick_late_max = 0, dev_gap_max = 0, said_at = now;
        }
        for (int i = 0; i < nvcpu; i++) {
            uint64_t t = run_since[i];
            if (t && now > t && now - t >= 20000000ull) kick_vcpu(i), kicks_sent[i]++;
        }
    }
    return NULL;
}


static void on_sigusr(int s) { (void)s; }

static void *vcpu_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    struct gh_vcpu_run *r = run[id];
    thread_altstack();
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
        /* 0.3.101: the driver ends GH_VCPU_RUN with -ERESTARTSYS whenever a signal is pending, also when it has just
           filled in an MMIO exit (exit_reason MMIO, its state "MMIO read/write"); the next run then resumes that access as
           handled. Each kick (every 2 ms) that landed so lost a device access: a store dropped, a load answered with
           whatever data was left in the run area. 0.3.100 lost vCPU 1's first report at the reset vector that way (its
           first exit seen was the second store), so its RAM was not cleaned and it hung: "cpu 1 failed to boot" again.
           exit_reason is cleared before each run, and an interrupted run that left an exit behind is handled as one. */
        r->exit_reason = GH_VCPU_EXIT_UNKNOWN;
        uint64_t in_at = now_ns();
        run_since[id] = in_at;
        int rc = ioctl(vcpu_fd[id], GH_VCPU_RUN, 0);
        run_since[id] = 0;
        {
            uint64_t stayed = now_ns() - in_at;
            if (stayed > stay_max[id]) stay_max[id] = stayed;
            if (stayed >= 100000000ull) stay_long[id]++;
        }
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                if (kicks_back[id]++ == 0) say("gunyah: vCPU %d came back on a signal: the kicks work\n", id);
                if (r->exit_reason == GH_VCPU_EXIT_MMIO || r->exit_reason == GH_VCPU_EXIT_STATUS) {
                    if (saved_exits[id]++ < 3)
                        say("gunyah: vCPU %d: an exit came back with a signal (%s %#llx): handled, not lost\n", id,
                            r->exit_reason == GH_VCPU_EXIT_STATUS ? "status" : r->mmio.is_write ? "store to" : "load from",
                            (unsigned long long)r->mmio.phys_addr);
                } else {
                    sched_yield();
                    continue;
                }
            } else {
                fprintf(stderr, "gunyah: running vCPU %d failed: %s\n", id, strerror(errno));
                vm_reason = HV_EXIT_UNSUPPORTED;
                break;
            }
        }
        if (r->exit_reason == GH_VCPU_EXIT_MMIO) {
            uint64_t pa = r->mmio.phys_addr;
            unsigned len = r->mmio.len > 8 ? 8 : r->mmio.len;
            r->mmio.resume_action = 0;
            if (id == 0 && fault_pa && pa == fault_pa && !r->mmio.is_write) {
                fault_pa = 0;
                r->mmio.resume_action = 1; /* GH_VCPU_RESUME_FAULT: Gunyah gives the kernel a data abort there */
                say("gunyah: vCPU 0's read at %#llx is answered with an abort: the kernel's panic names the code that loops\n",
                    (unsigned long long)pa);
                continue;
            }
            if (mmio_exits[id]++ == 0) say("gunyah: vCPU %d runs (its first exit: %s %#llx)\n", id, r->mmio.is_write ? "a store to" : "a load from",
                                           (unsigned long long)pa);
            note_page(id, pa, r->mmio.is_write);
            if (r->mmio.is_write) { uint64_t wv = 0; memcpy(&wv, r->mmio.data, len); hot_note(id, pa, wv, 1); }
            /* Always the hand-over page's (vmapple has nothing there): the shim loads x0-x30 from it after it reports
               the jump, and 0.3.80 sent those 31 loads to Henge's devices, which answered 0: the kernel got all zeros. */
            if (pa >= HANDOFF_PA && pa < HANDOFF_PA + 0x5000) {
                uint64_t off = pa - HANDOFF_PA, v = 0;
                if (r->mmio.is_write) {
                    memcpy(&v, r->mmio.data, len);
                    if (off == RV_MARK || off == RV_MARK + 8) rv_report(id, off - RV_MARK, v);
                    else if (off >= 0x4000) shim_report(off - 0x4000, v);
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
                uint64_t rv = 0;
                memcpy(&rv, r->mmio.data, len);
                hot_note(id, pa, rv, 0);
            }
        } else if (r->exit_reason == GH_VCPU_EXIT_STATUS) {
            uint32_t reason = 0;
            say("gunyah: vCPU %d: the VM's status changed\n", id);
            memcpy(&reason, r->status.reason, r->status.reason_size < 4 ? r->status.reason_size : 4);
            fprintf(stderr, "gunyah: the VM stopped (seen by vCPU %d): status %u (1 load failed, 2 exited, 3 crashed), "
                            "type %u, reason %#x\n", id, r->status.status, r->status.type, reason);
            vm_reason = r->status.status != 2 ? HV_EXIT_UNSUPPORTED : kernel_said_reboot() ? HV_GUNYAH_RESTART : 0;
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
    pthread_mutex_lock(&kill_mu);
    vcpu_alive[id] = 0;
    pthread_mutex_unlock(&kill_mu);
    return NULL;
}

void hv_gunyah_stop(void) {
    stop_req = 1;
    for (int i = 0; i < nvcpu; i++) {
        if (run[i]) run[i]->immediate_exit = 1;
        kick_vcpu(i);
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

    clean_all_ram();
    /* 0.3.96: the helpers are joined before this returns: detached, they went on reading the Mac's RAM (the PAC scan,
       the stall dump) and Henge's devices after the machine was taken down, the "Segmentation fault" after a stop. */
    pthread_t helper[8];
    int nhelper = 0;
    void *(*helpers[])(void *) = {device_thread, pac_thread, watch_thread, clean_thread, commpage_thread};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    hv_machine_wake_hook = kick_devices; /* a finished disk request reaches the device thread at once, not at its next 1 ms */
    /* 0.3.128 (HV_GUNYAH_MLOCK=1): 0.3.127's last two minutes, the tablet short of memory (450 to 590 MB available,
       2.2 GB swapped in and 2.3 out per 30 s): "the engine took 908 page faults, 4399 of them waiting for swap or
       storage", each with a CPU of the Mac's or a device behind it. The engine's own pages (some 80 MB beside the
       Mac's RAM, which is pinned already) stay in memory from here: locked as they are touched, not all at once. */
    {
        const char *ml = getenv("HV_GUNYAH_MLOCK");
        if (ml && *ml == '1') {
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif
            int rc = mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT);
            fprintf(stderr, "gunyah: the engine's own memory is kept out of swap: %s\n", rc == 0 ? "locked as it is touched" : strerror(errno));
        }
    }
    for (unsigned h = 0; h < sizeof helpers / sizeof *helpers; h++)
        if (pthread_create(&helper[nhelper], &attr, helpers[h], NULL) == 0) nhelper++;
    pthread_mutex_lock(&kill_mu); /* the ids are written before any kick reads them */
    for (int i = 0; i < nvcpu; i++)
        if (pthread_create(&vcpu_thread_id[i], &attr, vcpu_thread, (void *)(intptr_t)i) == 0) vcpu_alive[i] = 1;
    pthread_mutex_unlock(&kill_mu);
    if (getenv("HV_GUNYAH_KICK_MS")) kick_ms = atoi(getenv("HV_GUNYAH_KICK_MS"));
    if (kick_ms > 0 && pthread_create(&helper[nhelper], &attr, kick_thread, NULL) == 0) nhelper++;
    for (int i = 0; i < nvcpu; i++) pthread_join(vcpu_thread_id[i], NULL);
    stop_req = 1;
    for (int i = 0; i < nhelper; i++) pthread_join(helper[i], NULL);
    for (int i = 0; i < nvcpu; i++) vcpu_thread_id[i] = 0;
    hv_machine_wake_hook = NULL;
    hv_gic_external = NULL;
    hv_vm_stop_hook = NULL;
    if (!mac_runs) fprintf(stderr, "gunyah: the shim did not reach the Mac (its last phase: %s)\n", phase_name(shim_rep[0]));
    fprintf(stderr, "gunyah: vCPU 0 came back on signals %llu times (kicks sent: %llu); exits that came back with a signal "
                    "(once lost):", (unsigned long long)kicks_back[0], (unsigned long long)kicks_sent[0]);
    for (int i = 0; i < nvcpu; i++) fprintf(stderr, " vcpu%d %llu", i, (unsigned long long)saved_exits[i]);
    fputc('\n', stderr);
    /* 0.3.111: HV_GUNYAH_RESTART is negative too; 0.3.110 turned it into 0 here ("stopped: other"), so the app never
       started the Mac again after a chosen startup disk. */
    return vm_reason == HV_GUNYAH_RESTART ? HV_GUNYAH_RESTART : vm_reason < 0 ? 0 : vm_reason;
}
