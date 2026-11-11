/* The virtual Mac's CPUs on Gunyah (see gunyah.h). Linux/Android only.

   How the vmapple machine is laid onto a Gunyah VM:
   - Memory: every RAM range the machine has (boot ROM at 0x100000, the config window, guest RAM at 0x70000000)
     is shared with the VM at the same address. Henge keeps its own mapping, so its devices DMA as before.
   - Boot: Gunyah starts an unauthenticated VM at the base of the memory region that holds its device tree, with
     x0 = the tree. That region (BOOT_PA) starts with a five-instruction trampoline to the boot ROM, so AVPBooter
     starts as on Apple's hypervisor: EL1, MMU off, x0-x3 zero.
   - The device tree here is only for Gunyah's resource manager (gunyah-vm-config: vCPUs, the GIC, doorbells);
     the Mac never reads it (iBoot builds the kernel's device tree from the config device, as always).
   - GIC and timer: Gunyah's virtual GICv3, placed at vmapple's addresses, and the CPU's own virtual timer.
     Henge's emulated GIC is left in place but unused; its MMIO never exits here unless Gunyah put its GIC
     elsewhere, which this logs.
   - Device interrupts: one Gunyah doorbell (irqfd) per vmapple SPI, raised from Henge's GIC entry points through
     hv_gic_external. Level lines are re-raised while the device still holds them.
   - MMIO: each exit goes to hv_phys_read/hv_phys_write, Henge's device dispatch, under the same device lock.
   - Devices: a thread runs the machine's tick (virtio queues, USB, network, display) every millisecond and
     right after any MMIO write.
   - Paravirtualized PAC: vmapple's XNU asks Apple's hypervisor to program PAC keys (hvc #0 with x0 =
     0xc100000x, then `cbnz x0, .`). Gunyah answers those itself with NOT_SUPPORTED, and the guest would spin
     there forever. A scanner rewrites each `hvc #0; cbnz x0, .` pair it finds in guest memory to
     `mov x0, #0; nop` (and makes the CPU see it): the keys stay at their reset values, which every process then
     shares, which is what Henge's JIT does as well.
   - vCPUs: one per Henge CPU, each run by its own host thread (the VM's vCPUs are "proxy" scheduled: a vCPU runs
     only while a thread sits in GH_VCPU_RUN for it). The resource manager's tree lists cpu@0..N with reg = i, the
     same Aff0 values vmapple's config gives XNU, and Gunyah itself answers the guest's PSCI (method hvc): a
     secondary's GH_VCPU_RUN returns to guest code once the kernel's CPU_ON powers it on. */
#include "gunyah.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#define BOOT_PA 0x00800000ull   /* trampoline + the VM's device tree: unused space in vmapple's map */
#define BOOT_SIZE 0x10000ull
#define DTB_OFF 0x1000ull
#define SPI_LOW_FIRST 32        /* UART, RTC, GPIO, AES, graphics: INTIDs 33..63 */
#define SPI_LOW_LAST 67         /* and PCIe INTx 64..67 */
#define NIRQ 160

extern void (*hv_vm_stop_hook)(void);

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

/* ---- a minimal flattened device tree writer ---- */

typedef struct { uint8_t s[16384]; size_t n; char str[2048]; size_t ns; } Fdt;

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
    memcpy(f->s + f->n, v, len);
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

/* The VM's configuration for Gunyah's resource manager, in the shape crosvm gives it. */
static size_t build_dtb(uint8_t *out, size_t cap, int ncpus) {
    static Fdt f;
    memset(&f, 0, sizeof f);
    const uint32_t GIC = 1;
    begin(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "linux,dummy-virt");
    prop_u32(&f, "interrupt-parent", GIC);
    for (int i = 0; i < M->nram; i++) {
        char n[48];
        snprintf(n, sizeof n, "memory@%llx", (unsigned long long)M->ram[i].pa);
        begin(&f, n);
        prop_str(&f, "device_type", "memory");
        uint32_t reg[4] = {(uint32_t)(M->ram[i].pa >> 32), (uint32_t)M->ram[i].pa, (uint32_t)(M->ram[i].size >> 32),
                           (uint32_t)M->ram[i].size};
        prop_u32s(&f, "reg", reg, 4);
        end(&f);
    }
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
    end(&f);
    begin(&f, "interrupts");
    prop_u32(&f, "config", GIC);
    end(&f);
    begin(&f, "vcpus");
    prop_str(&f, "affinity", "proxy");
    end(&f);
    begin(&f, "vdevices");
    prop_str(&f, "generate", "/hypervisor");
    for (int id = 0; id < NIRQ; id++) {
        if (!irq_wanted(id)) continue;
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
    uint64_t reported = 0;
    while (!stop_req) {
        pac_scan();
        if (pac_patched != reported) {
            fprintf(stderr, "gunyah: %llu PAC hypercalls rewritten so far\n", (unsigned long long)pac_patched);
            reported = pac_patched;
        }
        usleep(pac_patched ? 2000000 : 300000);
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

/* ---- the vCPU ---- */

static void on_sigusr(int s) { (void)s; }

static void *vcpu_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    struct gh_vcpu_run *r = run[id];
    uint64_t unknown = 0, gic_exits = 0;
    time_t last_report = time(NULL);
    while (!stop_req) {
        if (ioctl(vcpu_fd[id], GH_VCPU_RUN, 0) < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            fprintf(stderr, "gunyah: running vCPU %d failed: %s\n", id, strerror(errno));
            vm_reason = HV_EXIT_UNSUPPORTED;
            break;
        }
        if (r->exit_reason == GH_VCPU_EXIT_MMIO) {
            uint64_t pa = r->mmio.phys_addr;
            unsigned len = r->mmio.len > 8 ? 8 : r->mmio.len;
            mmio_exits[id]++;
            if (pa >= VMAPPLE_GIC_DIST && pa < VMAPPLE_GIC_MSI && gic_exits++ < 4)
                fprintf(stderr, "gunyah: vCPU %d's GIC access at %llx came here: Gunyah did not put its GIC at "
                                "vmapple's address\n", id, (unsigned long long)pa);
            /* Every vCPU goes through Henge's device dispatch, which takes the device lock itself. */
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

static int fail(const char *what) {
    fprintf(stderr, "gunyah: %s failed: %s (errno %d)\n", what, strerror(errno), errno);
    return -1;
}

int hv_gunyah_run(void) {
    M = hv_vm_machine();
    B = hv_vm_board();
    nvcpu = hv_vm_ncpus();
    if (nvcpu < 1) nvcpu = 1;
    if (nvcpu > HV_MAX_CPUS) nvcpu = HV_MAX_CPUS;
    for (int i = 0; i < HV_MAX_CPUS; i++) vcpu_fd[i] = -1;
    for (int i = 0; i < NIRQ; i++) irq_fd[i] = -1;

    mark("open /dev/gunyah");
    int dev = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
    if (dev < 0) return fail("opening /dev/gunyah (it needs root)");
    mark("create the VM");
    vm_fd = ioctl(dev, GH_CREATE_VM, 0);
    if (vm_fd < 0) return fail("creating the VM");

    /* The boot region: trampoline to the ROM, then the resource manager's device tree. */
    uint8_t *boot = mmap(NULL, BOOT_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (boot == MAP_FAILED) return fail("mapping the boot region");
    const uint32_t tramp[] = {
        0xd2a00010u | ((uint32_t)(VMAPPLE_FIRMWARE >> 16) << 5), /* movz x16, #(ROM >> 16), lsl #16 */
        0xaa1f03e0u, 0xaa1f03e1u, 0xaa1f03e2u, 0xaa1f03e3u,       /* mov x0..x3, xzr */
        0xd61f0200u,                                              /* br x16 */
    };
    memcpy(boot, tramp, sizeof tramp);
    size_t dtb_len = build_dtb(boot + DTB_OFF, BOOT_SIZE - DTB_OFF, nvcpu);
    if (!dtb_len) { fprintf(stderr, "gunyah: the VM device tree is too big\n"); return -1; }

    uint32_t label = 0;
    struct gh_userspace_memory_region r = {.label = label++, .flags = GH_MEM_R | GH_MEM_W | GH_MEM_X,
                                           .guest_phys_addr = BOOT_PA, .memory_size = BOOT_SIZE,
                                           .userspace_addr = (uintptr_t)boot};
    mark("share the boot region");
    if (ioctl(vm_fd, GH_VM_SET_USER_MEM_REGION, &r) < 0) return fail("sharing the boot region");
    /* Gunyah pins every page and keeps one kcalloc'd array entry per 4 KB page (plus one parcel entry per run),
       so one 3 GB region needs a 6 MB+ physically contiguous kernel allocation and fails with ENOMEM. Large
       regions are shared in chunks, halved on ENOMEM down to 8 MB. Root may also need a higher memlock limit. */
    struct rlimit nolimit = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &nolimit) < 0) fprintf(stderr, "gunyah: raising the memlock limit: %s\n", strerror(errno));
    for (int i = 0; i < M->nram; i++) {
        HvRam *ram = &M->ram[i];
        fprintf(stderr, "gunyah: memory %llx-%llx (%llu MB)\n", (unsigned long long)ram->pa,
                (unsigned long long)(ram->pa + ram->size), (unsigned long long)(ram->size >> 20));
        /* No transparent huge pages in what Gunyah is given. This asked for them (fewer, longer runs in the
           parcel), and the tablet panicked when the VM started. Gunyah's firmware of the Snapdragon 8 Gen 3
           generation (the Mi Pad 7's 7+ Gen 3 is of it, Linux 6.1) is reported not to take 2 MB pages made by
           THP collapse: the hypervisor fails and resets the device (xiehezhi/Xiaomi14-8Gen3-Gunyah-Fix, on
           DroidVM's "Prepare Lend mTHP"). Plain 4 KB pages, as HV_GUNYAH_THP=1 does not. */
        madvise(ram->host, ram->size, getenv("HV_GUNYAH_THP") ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
        uint64_t chunk = 256ull << 20, done = 0;
        int pieces = 0;
        while (done < ram->size) {
            uint64_t n = ram->size - done < chunk ? ram->size - done : chunk;
            r = (struct gh_userspace_memory_region){.label = label, .flags = GH_MEM_R | GH_MEM_W | GH_MEM_X,
                                                    .guest_phys_addr = ram->pa + done, .memory_size = n,
                                                    .userspace_addr = (uintptr_t)ram->host + done};
            mark("share %llu MB of memory at %llx", (unsigned long long)(n >> 20), (unsigned long long)(ram->pa + done));
            if (ioctl(vm_fd, GH_VM_SET_USER_MEM_REGION, &r) == 0) {
                label++, pieces++, done += n;
                continue;
            }
            if (errno != ENOMEM || chunk <= (8ull << 20)) {
                fprintf(stderr, "gunyah: shared %llu of %llu MB before this; lower the RAM setting if memory is short\n",
                        (unsigned long long)(done >> 20), (unsigned long long)(ram->size >> 20));
                return fail("sharing guest memory");
            }
            chunk >>= 1;
        }
        if (pieces > 1) fprintf(stderr, "gunyah:   shared as %d pieces (%llu MB each at the end)\n", pieces,
                                (unsigned long long)(chunk >> 20));
    }
    struct gh_vm_dtb_config dtb = {.guest_phys_addr = BOOT_PA + DTB_OFF, .size = (dtb_len + 0xfff) & ~0xfffull};
    mark("set the VM device tree");
    if (ioctl(vm_fd, GH_VM_SET_DTB_CONFIG, &dtb) < 0) return fail("setting the VM device tree");

    int bells = 0;
    mark("add the interrupt doorbells");
    for (int id = 0; id < NIRQ; id++) {
        if (!irq_wanted(id)) continue;
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0) return fail("eventfd");
        struct gh_fn_irqfd_arg a = {.fd = (uint32_t)efd, .label = (uint32_t)id,
                                    .flags = irq_is_msi(id) ? 0 : GH_IRQFD_FLAGS_LEVEL};
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
            if (i == 0) return fail("adding the vCPU");
            /* The VM was configured for nvcpu; fewer vCPUs would leave XNU waiting on CPUs that never come. */
            fprintf(stderr, "gunyah: adding vCPU %d failed: %s; set the CPU count to %d\n", i, strerror(errno), i);
            return -1;
        }
    }
    mark("start the VM");
    if (ioctl(vm_fd, GH_VM_START, 0) < 0) return fail("starting the VM");
    long sz = ioctl(vcpu_fd[0], GH_VCPU_MMAP_SIZE, 0);
    if (sz < 0) sz = ioctl(dev, GH_VCPU_MMAP_SIZE, 0);
    if (sz <= 0) return fail("asking the vCPU's run area size");
    for (int i = 0; i < nvcpu; i++) {
        run[i] = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd[i], 0);
        if (run[i] == MAP_FAILED) { run[i] = NULL; return fail("mapping a vCPU's run area"); }
    }
    fprintf(stderr, "gunyah: %d vCPU%s\n", nvcpu, nvcpu == 1 ? "" : "s");

    dev_kick = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    hv_vm_stop_hook = hv_gunyah_stop;
    hv_gic_external = gic_external; /* from here Henge's devices ring doorbells instead of its own GIC */
    struct sigaction sa = {.sa_handler = on_sigusr}; /* no SA_RESTART: a stop breaks GH_VCPU_RUN */
    sigaction(SIGUSR1, &sa, NULL);
    mark("run the vCPUs (the VM started; the boot ROM runs on the CPU itself)");

    pthread_t t;
    pthread_create(&t, NULL, device_thread, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, pac_thread, NULL);
    pthread_detach(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    for (int i = 0; i < nvcpu; i++) pthread_create(&vcpu_thread_id[i], &attr, vcpu_thread, (void *)(intptr_t)i);
    for (int i = 0; i < nvcpu; i++) pthread_join(vcpu_thread_id[i], NULL);
    for (int i = 0; i < nvcpu; i++) vcpu_thread_id[i] = 0;
    stop_req = 1;
    usleep(20000);
    return vm_reason < 0 ? 0 : vm_reason;
}
