/* gunyah-probe: what this tablet's Gunyah hypervisor lets an app-made VM do, for deciding whether HengeVM
   can run the guest on it instead of the JIT. Run as root (the app's device check does, through su).

   It creates a VM through /dev/gunyah (the Android GH_* interface, include/uapi/linux/gunyah.h), boots a
   tiny EL1 program (gunyah_probe_guest.S) and reports, through MMIO writes it catches here:
     - MIDR, and whether 16K pages exist (macOS cannot run without them)
     - what the hypervisor answers to vmapple's PAC hypercall (XNU stops at boot unless it gets 0) and to
       SMCCC/PSCI queries: the GH_* interface forwards only MMIO to the VMM, so every HVC is Gunyah's own
     - any exception the guest takes instead (ESR, ELR)
   Each step prints ok or the error, so a failure shows exactly where the tablet stops it.

   With no arguments that VM is 2 MB with one CPU. Starting the Mac on Gunyah resets the Xiaomi Pad 7 and leaves
   no record, so the arguments walk from that small VM towards the engine's (host/gunyah.c), a step a run:
     --mem MB        guest memory, shared with Gunyah in 256 MB pieces as the engine shares it
     --touch         the guest writes every page of it and reads it back
     --cpus N        N vCPUs, the others started by the guest through PSCI, each run by a thread here
     --bells         the engine's interrupt doorbells (INTIDs 33-67 and 96-159), made and left alone
     --layout engine the engine's addresses: the boot region at 8 MB, a ROM-sized region at 1 MB, memory at
                     0x70000000, the interrupt controller at 0x10000000
     --layout zero   the same, with nothing below the VM's base address: one 2 MB region at 0 (the start code,
                     the tree, and the ROM's place at 1 MB inside it), base address 0. Qualcomm's resource
                     manager keeps what lies below the base address for devices.
     --image-mb N    the first region N MB, not 2 (probe layout)
     --dtb-room KB   the device tree's region, KB long (default: 1 MB, or what the first region has after the
                     tree). The resource manager writes the tree back where it lies with its own nodes added,
                     and with too little room it writes past what it mapped (crosvm gives 2 MB).
     --lend          all memory lent to the VM (GH_VM_ANDROID_LEND_USER_MEM), not shared with it
     --lend-image    only the first region lent (the resource manager of 2023 takes a VM's first region only
                     as its own); the rest shared, so Henge still reads and writes it
     --ram HOW       how memory beyond the first region is made known to the resource manager, which places
                     only the first region by itself (at the base address) and does not hear the addresses
                     the kernel was given:
                       shm      (default) each piece a `shm` device in the tree: its label and its address
                       resmem   each piece a /reserved-memory node: its label and its address
                       private  one piece, labelled 0 and lent: the resource manager adds such a piece right
                                after the first region (which is then labelled 1)
                       plain    said nowhere, as before 0.3.40
     --poke ADDR     last, the guest stores to ADDR (hex): whether a store where the Mac has a device (its PCI
                     windows are above 1 GB) comes out to Henge, or is an exception in the guest
     --mmu16k        last, the guest turns its MMU on with 16 KB pages, as macOS has it, and reads memory
                     through them: Gunyah's ID register says the CPU has none, and may only be saying so
     --base ADDR     the base address (hex): where the first region is, and what the tree says of it
     --gic ADDR      the interrupt controller's address (hex; the redistributors 64 KB after it)
     --size-max MB   the tree's limit of the VM's own memory above the base address
     --ram-at ADDR   the memory beyond the first region at ADDR (hex), not where the layout has it
     --ram-rw        the memory beyond the first region given readable and writable, not executable
     --shm-alloc     with --ram shm: no address in the tree; the resource manager is to choose one
     --peek          after the guest ran, this program reads a byte of the first region: whether memory lent
                     to the VM is still the host's to read (Henge's devices read and write the Mac's memory).
                     Read in a child process, so a refusal is a signal to the child; it may also reset the tablet.
     --tree-only F   no VM: the tree this run would give, written to F
     --kmsg FILE     the kernel's messages from now on copied to FILE, each forced to storage as it comes:
                     what the kernel said last before a reset
     --seconds S     how long the guest may run (6)
     --until STEP    stop, and take the VM down, after `open`, `create`, `memory`, `tree`, `vcpu` or `start`:
                     the steps before the guest runs, one at a time
     --crumb FILE    before each step, a line appended to FILE and forced to storage (fsync): after a reset
                     the file's last line is the step the tablet did not survive
   The last line printed is `RESULT: PASS` or `RESULT: FAIL ...`.

   What the Xiaomi Pad 7 answered to 0.3.40 (its resource manager is gunyah-6de79709c, and its messages are those
   of Qualcomm's source of late 2023): the tree is taken; a first region that is shared is refused at the start
   and one that is lent starts, at 0x80000000; base address 0 is refused; 64 MB more, lent, is refused whichever
   way the tree names it. The guest's ID register shows no 16 KB pages.

   And to 0.3.41: 16 KB pages work in the guest, whatever the ID register says. A lent first region works up to
   256 MB, as long as the kernel takes it as one piece; any second piece of memory, lent or shared, named in the
   tree or not, is refused at the start. Base addresses 0x70000000 and 0x80000000 start, 0, 2 MB and 0x40000000
   do not; the interrupt controller may be at 0x10000000. Stores below 1 GB with no memory there come out to
   the VMM; at 0x40000000 and 0x50000000 the guest takes an external abort. (More than one CPU was refused,
   but with the probe layout's redistributor region, which has room for one.)

   What the resource manager wants of the tree (Qualcomm's source, vm_config_parser.c and vm_creation.c; the
   tablet answered "memory invalid" to VM_INIT, which in the 2023 source is also what a tree it cannot accept
   gives):
     - one memory node, named exactly `memory`, under the root. Any other node with device_type = "memory"
       (`memory@80000000` is another) fails the whole tree. The resource manager replaces it with what it
       really gave the VM.
     - a `/chosen` node: its changes to the tree include one there, and are refused whole without it.
     - no `/hypervisor` node: that is its own to make. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gunyah_probe_blobs.h"

/* Android common kernel include/uapi/linux/gunyah.h (android14-6.1 and later). */
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
#define GH_MEM_RWX 7
enum { GH_VCPU_EXIT_UNKNOWN, GH_VCPU_EXIT_MMIO, GH_VCPU_EXIT_STATUS };
struct gh_vcpu_run {
    uint8_t immediate_exit, padding[7];
    uint32_t exit_reason;
    union {
        struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
        struct { uint32_t status; uint16_t type, pad; uint32_t reason_size; uint8_t reason[8]; } status;
    };
};

#define MAX_CPUS 8
#define NIRQ 160
/* The engine's doorbells (host/gunyah.c, irq_wanted): the low SPIs and the MSI frame's. */
static int bell_wanted(int id) { return (id >= 33 && id <= 67) || (id >= 96 && id < 160); }
static int bell_is_msi(int id) { return id >= 96; }

/* ---- the layouts ---- */
typedef struct {
    const char *name;
    uint64_t image_pa, image_size, dtb_off; /* the region the guest starts in, and the VM's tree inside it */
    uint64_t rom_pa, rom_size;              /* a region of zeros where the engine has the boot ROM (0: none) */
    uint64_t ram_pa;                        /* more memory, --mem's */
    uint64_t gic_dist, gic_redist, redist_each;
    uint64_t report;                        /* a page with nothing behind it: the guest's stores there exit */
    uint32_t ppi_flags;
    int nodes;                              /* the tree's memory: 0 image and RAM as one, 1 ROM and RAM, 2 image, RAM */
} Layout;
static const Layout SMALL = {"probe", 0x80000000ull, 0x200000, 0x100000, 0, 0, 0x80200000ull,
                             0x3fff0000ull, 0x3ffd0000ull, 0, 0x10000000ull, 0x108, 0};
static const Layout ENGINE = {"engine", 0x00800000ull, 0x10000, 0x1000, 0x00100000ull, 0x100000, 0x70000000ull,
                              0x10000000ull, 0x10010000ull, 0x20000, 0x20010000ull, 0x104, 1};
static const Layout ZERO = {"zero", 0, 0x200000, 0x1000, 0, 0, 0x70000000ull,
                            0x10000000ull, 0x10010000ull, 0x20000, 0x20010000ull, 0x104, 2};

/* Memory beyond the first region, a piece a parcel: what the tree must tell the resource manager of. */
typedef struct { uint32_t label; uint64_t pa, size; } Piece;
#define MAX_PIECES 96
static Piece pieces[MAX_PIECES];
static int npieces;
enum { RAM_SHM, RAM_RESMEM, RAM_PRIVATE, RAM_PLAIN };
static int ram_how = RAM_SHM;
static uint64_t size_max_mb;
static int shm_alloc, ram_rw;

/* ---- crumbs: a line forced to storage before each step ---- */
static const char *crumb_path;
static void crumb(const char *fmt, ...) {
    if (!crumb_path) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    int fd = open(crumb_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)!write(fd, line, (size_t)n);
    fsync(fd);
    close(fd);
}

/* --kmsg: a child that copies the kernel's messages, from now on, to a file opened so each write is on storage
   when it returns. It dies with this process. */
#include <sys/prctl.h>
static void kmsg_start(const char *path) {
    if (!path) return;
    pid_t pid = fork();
    if (pid != 0) return;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    int in = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
    int out = open(path, O_WRONLY | O_APPEND | O_CREAT | O_DSYNC | O_CLOEXEC, 0644);
    if (in < 0 || out < 0) _exit(0);
    lseek(in, 0, SEEK_END);
    char rec[4096];
    for (;;) {
        ssize_t n = read(in, rec, sizeof rec - 1); /* one record: "level,sequence,microseconds,flags;text\n" */
        if (n < 0) {
            if (errno == EPIPE) continue; /* records were overwritten before they were read */
            _exit(0);
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

static int step(const char *what, long r) {
    if (r < 0) {
        printf("%-22s FAILED: %s (errno %d)\n", what, strerror(errno), errno);
        crumb("failed: %s: %s", what, strerror(errno));
    } else {
        printf("%-22s ok\n", what);
        crumb("ok: %s", what);
    }
    return r < 0;
}

/* ---- the VM's tree for Gunyah's resource manager, in the shape crosvm gives it ---- */
typedef struct { uint8_t s[60000]; size_t n; char str[2048]; size_t ns; } Fdt;
static void be32(Fdt *f, uint32_t v) { f->s[f->n++] = v >> 24, f->s[f->n++] = v >> 16, f->s[f->n++] = v >> 8, f->s[f->n++] = v; }
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
    uint8_t b[16 * 4];
    for (int i = 0; i < count; i++) b[4 * i] = v[i] >> 24, b[4 * i + 1] = v[i] >> 16, b[4 * i + 2] = v[i] >> 8, b[4 * i + 3] = v[i];
    prop(f, name, b, (size_t)count * 4);
}
static void prop_u32(Fdt *f, const char *name, uint32_t v) { prop_u32s(f, name, &v, 1); }
static void prop_reg(Fdt *f, const char *name, uint64_t pa, uint64_t size) {
    uint32_t reg[4] = {(uint32_t)(pa >> 32), (uint32_t)pa, (uint32_t)(size >> 32), (uint32_t)size};
    prop_u32s(f, name, reg, 4);
}

static size_t build_dtb(uint8_t *out, size_t cap, const Layout *L, int ncpus, int bells) {
    static Fdt f;
    memset(&f, 0, sizeof f);
    const uint32_t GIC = 1;
    begin(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "linux,dummy-virt");
    prop_u32(&f, "interrupt-parent", GIC);
    /* The one memory node, named `memory` and nothing more: the resource manager replaces it. */
    begin(&f, "memory");
    prop_str(&f, "device_type", "memory");
    prop_reg(&f, "reg", L->image_pa, L->image_size);
    end(&f);
    begin(&f, "chosen"); /* the resource manager adds to it (a seed), and must find it */
    end(&f);
    if (ram_how == RAM_RESMEM && npieces) {
        begin(&f, "reserved-memory");
        prop_u32(&f, "#address-cells", 2);
        prop_u32(&f, "#size-cells", 2);
        prop(&f, "ranges", NULL, 0);
        for (int i = 0; i < npieces; i++) {
            char n[48];
            snprintf(n, sizeof n, "henge@%llx", (unsigned long long)pieces[i].pa);
            begin(&f, n);
            prop_reg(&f, "reg", pieces[i].pa, pieces[i].size);
            prop_u32(&f, "qcom,label", pieces[i].label);
            end(&f);
        }
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
    char gn[32];
    snprintf(gn, sizeof gn, "intc@%llx", (unsigned long long)L->gic_dist);
    begin(&f, gn);
    prop_str(&f, "compatible", "arm,gic-v3");
    prop_u32(&f, "#interrupt-cells", 3);
    prop(&f, "interrupt-controller", NULL, 0);
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    uint32_t redist = L->redist_each ? (uint32_t)L->redist_each * (uint32_t)ncpus : 0x20000;
    uint32_t gic[8] = {0, (uint32_t)L->gic_dist, 0, 0x10000, 0, (uint32_t)L->gic_redist, 0, redist};
    prop_u32s(&f, "reg", gic, 8);
    prop_u32(&f, "phandle", GIC);
    end(&f);
    begin(&f, "timer");
    prop_str(&f, "compatible", "arm,armv8-timer");
    uint32_t ppis[12] = {1, 13, L->ppi_flags, 1, 14, L->ppi_flags, 1, 11, L->ppi_flags, 1, 10, L->ppi_flags};
    prop_u32s(&f, "interrupts", ppis, 12);
    prop(&f, "always-on", NULL, 0);
    end(&f);
    begin(&f, "gunyah-vm-config");
    prop_str(&f, "image-name", "henge-probe");
    prop_str(&f, "os-type", "linux");
    begin(&f, "memory");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    uint32_t base[2] = {(uint32_t)(L->image_pa >> 32), (uint32_t)L->image_pa};
    prop_u32s(&f, "base-address", base, 2);
    if (size_max_mb) { /* read with the cells of the node above, which says none: one cell */
        prop_u32(&f, "size-max", (uint32_t)(size_max_mb << 20));
    }
    end(&f);
    begin(&f, "interrupts");
    prop_u32(&f, "config", GIC);
    end(&f);
    begin(&f, "vcpus");
    prop_str(&f, "affinity", "proxy");
    end(&f);
    begin(&f, "vdevices");
    prop_str(&f, "generate", "/hypervisor");
    for (int i = 0; ram_how == RAM_SHM && i < npieces; i++) {
        /* Shared memory at an address of its own: the parcel of this label, placed at `base`. */
        char n[32], path[48];
        snprintf(n, sizeof n, "shm-%x", pieces[i].label);
        snprintf(path, sizeof path, "/hypervisor/shm-%x", pieces[i].label);
        begin(&f, n);
        prop_str(&f, "vdevice-type", "shm");
        prop_str(&f, "generate", path);
        prop(&f, "peer-default", NULL, 0);
        prop_u32(&f, "#address-cells", 2);
        prop_u32(&f, "#size-cells", 2);
        begin(&f, "memory");
        prop_u32(&f, "qcom,label", pieces[i].label);
        prop_u32(&f, "label", pieces[i].label);
        uint32_t at[2] = {(uint32_t)(pieces[i].pa >> 32), (uint32_t)pieces[i].pa};
        if (shm_alloc) prop(&f, "allocate-base", NULL, 0);
        else prop_u32s(&f, "base", at, 2);
        end(&f);
        end(&f);
    }
    for (int id = 0; bells && id < NIRQ; id++) {
        if (!bell_wanted(id)) continue;
        char n[32], path[48];
        snprintf(n, sizeof n, "bell-%x", id);
        snprintf(path, sizeof path, "/hypervisor/bell-%x", id);
        begin(&f, n);
        prop_str(&f, "vdevice-type", "doorbell");
        prop_str(&f, "generate", path);
        prop_u32(&f, "label", (uint32_t)id);
        prop(&f, "peer-default", NULL, 0);
        uint32_t irq[3] = {0, (uint32_t)(id - 32), bell_is_msi(id) ? 1u : 4u};
        prop_u32s(&f, "interrupts", irq, 3);
        end(&f);
    }
    end(&f);
    end(&f);
    end(&f);
    be32(&f, 9);
    size_t off_rsv = 40, off_st = off_rsv + 16, off_str = off_st + f.n, total = off_str + f.ns;
    if (total > cap) return 0;
    static uint8_t hdr[40];
    uint32_t v[10] = {0xd00dfeed, (uint32_t)total, (uint32_t)off_st, (uint32_t)off_str, (uint32_t)off_rsv, 17, 16, 0,
                      (uint32_t)f.ns, (uint32_t)f.n};
    for (int i = 0; i < 10; i++) hdr[4 * i] = v[i] >> 24, hdr[4 * i + 1] = v[i] >> 16, hdr[4 * i + 2] = v[i] >> 8, hdr[4 * i + 3] = v[i];
    memset(out, 0, total);
    memcpy(out, hdr, 40);
    memcpy(out + off_st, f.s, f.n);
    memcpy(out + off_str, f.str, f.ns);
    return total;
}

/* ---- running ---- */
static uint64_t rep[32];
static unsigned seen;
static uint64_t report_pa, poke_at;
static int poked;
static volatile sig_atomic_t timed_out;
static volatile int vm_over;
static void on_alarm(int s) { (void)s; timed_out = 1; }
static void on_usr1(int s) { (void)s; }

static int vcpu_fd[MAX_CPUS];
static struct gh_vcpu_run *vrun[MAX_CPUS];
static pthread_t vthread[MAX_CPUS];
static int vexits[MAX_CPUS];

/* One exit of a vCPU's. Answers 1 when its VM is over. */
static int handle_exit(int cpu) {
    struct gh_vcpu_run *run = vrun[cpu];
    if (run->exit_reason == GH_VCPU_EXIT_MMIO) {
        uint64_t a = run->mmio.phys_addr, v = 0;
        if (run->mmio.is_write && a >= report_pa && a < report_pa + 0x100) {
            memcpy(&v, run->mmio.data, run->mmio.len < 8 ? run->mmio.len : 8);
            unsigned i = (unsigned)((a - report_pa) / 8);
            rep[i] = v, seen |= 1u << i;
            if (i == 15) return 1;
        } else if (!run->mmio.is_write) {
            memset(run->mmio.data, 0, sizeof run->mmio.data);
        } else if (poke_at && a == poke_at) {
            poked = 1;
        } else {
            memcpy(&v, run->mmio.data, run->mmio.len < 8 ? run->mmio.len : 8);
            printf("guest (cpu %d) wrote %#llx at %#llx\n", cpu, (unsigned long long)v, (unsigned long long)a);
        }
        return 0;
    }
    if (run->exit_reason == GH_VCPU_EXIT_STATUS) {
        if (cpu == 0 || run->status.status != 2)
            printf("VM status %u on cpu %d (1 load failed, 2 exited, 3 crashed), reason type %u\n", run->status.status, cpu,
                   run->status.type);
        return 1;
    }
    printf("unknown exit %u on cpu %d\n", run->exit_reason, cpu);
    return 1;
}

/* A CPU the guest starts: Gunyah's vCPUs run only while a thread sits in GH_VCPU_RUN for them. */
static void *secondary_thread(void *arg) {
    int cpu = (int)(intptr_t)arg;
    while (!vm_over && !timed_out) {
        if (ioctl(vcpu_fd[cpu], GH_VCPU_RUN, 0) < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!vm_over) printf("run vCPU %d            FAILED: %s (errno %d)\n", cpu, strerror(errno), errno);
            break;
        }
        vexits[cpu]++;
        if (handle_exit(cpu)) break;
    }
    return NULL;
}

static const char *hvc_result(uint64_t v) {
    switch ((int64_t)v) {
    case 0: return "0 (success)";
    case -1: return "-1 NOT_SUPPORTED";
    case -2: return "-2 INVALID_PARAMETERS";
    case -3: return "-3 DENIED";
    default: return "other";
    }
}

/* `size` bytes of `host` given to the VM at `pa`, in pieces of 256 MB at most, halved where the kernel has no
   room for a piece's page list (ENOMEM), as the engine does. */
static int lend_image, lend_rest; /* --lend: both; --lend-image */
static int share(int vm, uint32_t *label, uint64_t pa, uint8_t *host, uint64_t size, const char *what, int lend, int declare) {
    uint64_t chunk = 256ull << 20, done = 0;
    if (ram_how == RAM_PRIVATE && declare) chunk = size; /* one piece: only one can be labelled 0 */
    int count = 0;
    while (done < size) {
        uint64_t n = size - done < chunk ? size - done : chunk;
        struct gh_userspace_memory_region r = {.label = *label, .flags = declare && ram_rw ? 3 : GH_MEM_RWX, .guest_phys_addr = pa + done,
                                               .memory_size = n, .userspace_addr = (uintptr_t)host + done};
        crumb("try: give the VM %s: %llu MB at %#llx (piece %d)", what, (unsigned long long)(n >> 20),
              (unsigned long long)(pa + done), count + 1);
        if (ioctl(vm, lend ? GH_VM_ANDROID_LEND_USER_MEM : GH_VM_SET_USER_MEM_REGION, &r) == 0) {
            if (declare && npieces < MAX_PIECES) pieces[npieces++] = (Piece){*label, pa + done, n};
            (*label)++, count++, done += n;
            continue;
        }
        if (errno != ENOMEM || chunk <= (8ull << 20)) {
            char w[96];
            snprintf(w, sizeof w, "give %s (%llu of %llu MB)", what, (unsigned long long)(done >> 20), (unsigned long long)(size >> 20));
            return step(w, -1);
        }
        chunk >>= 1;
    }
    char w[96];
    snprintf(w, sizeof w, "give %s", what);
    step(w, 0);
    if (count > 1) printf("                       in %d pieces\n", count);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static Layout layout;
    Layout *L = &layout;
    layout = SMALL;
    uint64_t mem_mb = 2, image_mb = 0, dtb_room_kb = 0, poke = 0, base = ~0ull, gic = 0, ram_at = ~0ull;
    int mmu16k = 0, peek = 0;
    const char *kmsg_path = NULL, *tree_only = NULL;
    int touch = 0, ncpus = 1, bells = 0, seconds = 6;
    const char *until = "";
    int dev = -1, vm = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--touch")) touch = 1;
        else if (!strcmp(a, "--lend")) lend_image = lend_rest = 1;
        else if (!strcmp(a, "--lend-image")) lend_image = 1;
        else if (!strcmp(a, "--mmu16k")) mmu16k = 1;
        else if (!strcmp(a, "--ram-rw")) ram_rw = 1;
        else if (!strcmp(a, "--shm-alloc")) shm_alloc = 1;
        else if (!strcmp(a, "--peek")) peek = 1;
        else if (!strcmp(a, "--bells")) bells = 1;
        else if (!v) { fprintf(stderr, "gunyah-probe: %s needs a value\n", a); return 2; }
        else if (!strcmp(a, "--mem")) mem_mb = strtoull(v, NULL, 10), i++;
        else if (!strcmp(a, "--cpus")) ncpus = atoi(v), i++;
        else if (!strcmp(a, "--seconds")) seconds = atoi(v), i++;
        else if (!strcmp(a, "--crumb")) crumb_path = v, i++;
        else if (!strcmp(a, "--until")) until = v, i++;
        else if (!strcmp(a, "--kmsg")) kmsg_path = v, i++;
        else if (!strcmp(a, "--image-mb")) image_mb = strtoull(v, NULL, 10), i++;
        else if (!strcmp(a, "--dtb-room")) dtb_room_kb = strtoull(v, NULL, 10), i++;
        else if (!strcmp(a, "--poke")) poke = strtoull(v, NULL, 16), i++;
        else if (!strcmp(a, "--base")) base = strtoull(v, NULL, 16) & ~0x1fffffull, i++;
        else if (!strcmp(a, "--gic")) gic = strtoull(v, NULL, 16), i++;
        else if (!strcmp(a, "--ram-at")) ram_at = strtoull(v, NULL, 16) & ~0xfffull, i++;
        else if (!strcmp(a, "--size-max")) size_max_mb = strtoull(v, NULL, 10), i++;
        else if (!strcmp(a, "--tree-only")) tree_only = v, i++;
        else if (!strcmp(a, "--ram")) {
            ram_how = !strcmp(v, "resmem") ? RAM_RESMEM : !strcmp(v, "private") ? RAM_PRIVATE : !strcmp(v, "plain") ? RAM_PLAIN : RAM_SHM;
            i++;
        }
        else if (!strcmp(a, "--layout")) layout = !strcmp(v, "engine") ? ENGINE : !strcmp(v, "zero") ? ZERO : SMALL, i++;
        else { fprintf(stderr, "gunyah-probe: unknown argument %s\n", a); return 2; }
    }
    if (ncpus < 1) ncpus = 1;
    if (ncpus > MAX_CPUS) ncpus = MAX_CPUS;
    if (seconds < 1) seconds = 1;
    /* The probe layout's first 2 MB hold the image; the engine layout's memory is all apart from it. */
    int small = L->nodes == 0;
    if (base != ~0ull) L->image_pa = base, L->ram_pa = small ? base + L->image_size : L->ram_pa;
    if (gic) L->gic_dist = gic, L->gic_redist = gic + 0x10000, L->redist_each = 0x20000;
    if (gic && L->report >= gic && L->report < gic + 0x10000 + 0x20000ull * MAX_CPUS) L->report = 0x20010000ull; /* not the GIC's own */
    if (small && image_mb >= 2 && image_mb <= 512) {
        L->image_size = image_mb << 20, L->ram_pa = L->image_pa + L->image_size;
        if (mem_mb < image_mb) mem_mb = image_mb;
    } else if (L->nodes == 2 && image_mb >= 2 && image_mb <= 512) {
        L->image_size = image_mb << 20; /* base 0: a longer first region, the memory still at 0x70000000 */
    }
    uint64_t image_now = L->image_size >> 20;
    uint64_t ram_size = small ? (mem_mb > image_now ? (mem_mb - image_now) << 20 : 0) : mem_mb << 20;
    /* A piece the resource manager adds by itself lies right after the first region, whatever the layout. */
    if (ram_how == RAM_PRIVATE) L->ram_pa = L->image_pa + L->image_size;
    else if (ram_at != ~0ull) L->ram_pa = ram_at;
    /* --touch with a long first region and nothing after it: the first region past its first 2 MB. */
    uint64_t touch_pa = ram_size ? L->ram_pa : L->image_pa + 0x200000, touch_size = ram_size ? ram_size : L->image_size - 0x200000;
    /* The 16 KB page table: 16 KB of the first region nothing else has (the tree and its room lie elsewhere). */
    uint64_t table = !mmu16k ? 0 : L->dtb_off >= 0x100000 ? L->image_pa + 0x8000 : L->image_size >= 0x200000 ? L->image_pa + 0x180000 : 0;
    static const char *const how_name[] = {"a shm device each", "a reserved-memory node each", "one private piece, lent", "said nowhere"};
    kmsg_start(kmsg_path);
    report_pa = L->report, poke_at = poke;
    printf("gunyah-probe: uid %d; %s layout (base %#llx, first region %llu MB), %llu MB, %d CPU%s%s%s%s\n", (int)getuid(), L->name,
           (unsigned long long)L->image_pa, (unsigned long long)image_now, (unsigned long long)mem_mb, ncpus,
           ncpus == 1 ? "" : "s", touch ? ", every page touched" : "", bells ? ", the engine's doorbells" : "",
           lend_image && lend_rest ? ", memory lent" : lend_image ? ", first region lent" : "");
    if (ram_size || L->rom_size)
        printf("                       memory beyond the first region: %s%s\n", how_name[ram_how],
               ram_size ? "" : " (the ROM's place)");
    if (tree_only) {
        /* The pieces as sharing would make them, 256 MB each. */
        uint32_t l = ram_how == RAM_PRIVATE ? 0 : 1;
        if (L->rom_size) pieces[npieces++] = (Piece){l++, L->rom_pa, L->rom_size};
        for (uint64_t d = 0; d < ram_size && npieces < MAX_PIECES;) {
            uint64_t n = ram_how == RAM_PRIVATE || ram_size - d < (256ull << 20) ? ram_size - d : 256ull << 20;
            pieces[npieces++] = (Piece){l++, L->ram_pa + d, n};
            d += n;
        }
        static uint8_t t[65536];
        size_t n = build_dtb(t, sizeof t, L, ncpus, bells);
        FILE *o = fopen(tree_only, "wb");
        if (!n || !o || fwrite(t, 1, n, o) != n) { printf("RESULT: FAIL the tree was not written\n"); return 1; }
        fclose(o);
        printf("device tree            %zu bytes, written to %s\nRESULT: PASS\n", n, tree_only);
        return 0;
    }
    crumb("begin: %s layout, %llu MB, %d CPUs%s%s", L->name, (unsigned long long)mem_mb, ncpus, touch ? ", touch" : "",
          bells ? ", doorbells" : "");

    crumb("try: open /dev/gunyah");
    dev = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
    if (step("open /dev/gunyah", dev)) goto fail;
    if (!strcmp(until, "open")) goto early;
    crumb("try: create VM");
    vm = ioctl(dev, GH_CREATE_VM, 0);
    if (step("create VM", vm)) goto fail;
    if (!strcmp(until, "create")) goto early;

    uint8_t *image = mmap(NULL, L->image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (image == MAP_FAILED) { step("map the image", -1); goto fail; }
    memcpy(image, probe_guest, sizeof probe_guest);
    uint64_t params[6] = {L->report, touch && touch_size ? touch_pa : 0, touch ? touch_size : 0, (uint64_t)ncpus, poke, table};
    memcpy(image + PROBE_GUEST_PARAMS, params, sizeof params);

    struct rlimit nolimit = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &nolimit); /* Gunyah pins what it is given */
    /* Labels: the first region 0 and the rest from 1; with --ram private the piece after it is 0 (the resource
       manager adds only a piece labelled 0 by itself) and the first region, which it finds by its handle, 1. */
    uint32_t label = ram_how == RAM_PRIVATE ? 1 : 0, rest_label = ram_how == RAM_PRIVATE ? 0 : 1;
    if (share(vm, &label, L->image_pa, image, L->image_size, "the image", lend_image, 0)) goto fail;
    int declare = ram_how != RAM_PLAIN, lend_ram = lend_rest || ram_how == RAM_PRIVATE;
    if (L->rom_size) {
        uint8_t *rom = mmap(NULL, L->rom_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (rom == MAP_FAILED) { step("map the ROM's place", -1); goto fail; }
        if (ram_how == RAM_PRIVATE) rest_label = 2; /* not the piece labelled 0: that is the memory's */
        if (share(vm, &rest_label, L->rom_pa, rom, L->rom_size, "the ROM's place", lend_rest, declare && ram_how != RAM_PRIVATE)) goto fail;
        if (ram_how == RAM_PRIVATE) rest_label = 0;
    }
    if (ram_size) {
        /* Anonymous memory of plain 4 KB pages, as the engine's: no file's pages, no transparent huge pages. */
        uint8_t *ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (ram == MAP_FAILED) { step("map guest memory", -1); goto fail; }
        madvise(ram, ram_size, MADV_NOHUGEPAGE);
        if (share(vm, &rest_label, L->ram_pa, ram, ram_size, "guest memory", lend_ram, declare)) goto fail;
    }
    if (!strcmp(until, "memory")) goto early;
    /* The tree, written now that the pieces and their labels are known. The first region's pages are pinned
       and still this process's to write until the VM starts. */
    size_t dtb_len = build_dtb(image + L->dtb_off, L->image_size - L->dtb_off, L, ncpus, bells);
    if (!dtb_len) { printf("the VM's device tree does not fit\n"); goto fail; }
    /* Room after the tree: up to the ROM's place when that lies in the first region (base 0), else its end. */
    uint64_t room_cap = (L->dtb_off < 0x100000 && L->image_size > 0x100000 ? 0x100000 : L->image_size) - L->dtb_off;
    struct gh_vm_dtb_config dtb = {.guest_phys_addr = L->image_pa + L->dtb_off, .size = dtb_room_kb ? dtb_room_kb << 10 : 1024 << 10};
    dtb.size &= ~0xfffull;
    if (dtb.size < ((dtb_len + 0xfff) & ~0xfffull)) dtb.size = (dtb_len + 0xfff) & ~0xfffull;
    if (dtb.size > room_cap) dtb.size = room_cap;
    if (dtb_len > dtb.size) { printf("the VM's device tree does not fit\n"); goto fail; }
    printf("device tree            %zu bytes in a region of %llu KB\n", dtb_len, (unsigned long long)(dtb.size >> 10));
    crumb("try: set device tree (%zu bytes, region %llu KB)", dtb_len, (unsigned long long)(dtb.size >> 10));
    if (step("set device tree", ioctl(vm, GH_VM_SET_DTB_CONFIG, &dtb))) goto fail;
    if (!strcmp(until, "tree")) goto early;

    if (bells) {
        int made = 0, refused = 0;
        crumb("try: add the interrupt doorbells");
        for (int id = 0; id < NIRQ; id++) {
            if (!bell_wanted(id)) continue;
            int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (efd < 0) { step("eventfd", -1); goto fail; }
            struct gh_fn_irqfd_arg a = {.fd = (uint32_t)efd, .label = (uint32_t)id, .flags = bell_is_msi(id) ? 0 : GH_IRQFD_FLAGS_LEVEL};
            struct gh_fn_desc fn = {.type = GH_FN_IRQFD, .arg_size = sizeof a, .arg = (uintptr_t)&a};
            if (ioctl(vm, GH_VM_ADD_FUNCTION, &fn) < 0) {
                if (!refused++) printf("doorbell %d             refused: %s (errno %d)\n", id, strerror(errno), errno);
                close(efd);
            } else {
                made++;
            }
        }
        printf("%-22s %d made, %d refused\n", "doorbells", made, refused);
        crumb("ok: doorbells: %d made, %d refused", made, refused);
    }

    for (int i = 0; i < ncpus; i++) {
        uint32_t id = (uint32_t)i;
        struct gh_fn_desc fn = {.type = GH_FN_VCPU, .arg_size = sizeof id, .arg = (uintptr_t)&id};
        crumb("try: add vCPU %d", i);
        vcpu_fd[i] = ioctl(vm, GH_VM_ADD_FUNCTION, &fn);
        char w[32];
        snprintf(w, sizeof w, ncpus == 1 ? "add vCPU" : "add vCPU %d", i);
        if (step(w, vcpu_fd[i])) goto fail;
    }
    if (!strcmp(until, "vcpu")) goto early;
    crumb("try: start VM");
    if (step("start VM", ioctl(vm, GH_VM_START, 0))) goto fail;
    if (!strcmp(until, "start")) goto early;
    long sz = ioctl(vcpu_fd[0], GH_VCPU_MMAP_SIZE, 0);
    if (sz < 0) sz = ioctl(dev, GH_VCPU_MMAP_SIZE, 0);
    if (step("vCPU run area size", sz)) goto fail;
    for (int i = 0; i < ncpus; i++) {
        vrun[i] = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd[i], 0);
        if (vrun[i] == MAP_FAILED) { step("map vCPU run area", -1); goto fail; }
    }

    struct sigaction sa = {.sa_handler = on_alarm}; /* no SA_RESTART: the alarm breaks a stuck GH_VCPU_RUN */
    sigaction(SIGALRM, &sa, NULL);
    struct sigaction su = {.sa_handler = on_usr1};
    sigaction(SIGUSR1, &su, NULL);
    crumb("try: run the guest (up to %d s)", seconds);
    for (int i = 1; i < ncpus; i++) pthread_create(&vthread[i], NULL, secondary_thread, (void *)(intptr_t)i);
    alarm((unsigned)seconds);
    int done = 0;
    while (!done && !timed_out && vexits[0] < 100000) {
        if (ioctl(vcpu_fd[0], GH_VCPU_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            step("run vCPU", -1);
            break;
        }
        vexits[0]++;
        done = handle_exit(0);
    }
    alarm(0);
    vm_over = 1;
    for (int i = 1; i < ncpus; i++) {
        pthread_kill(vthread[i], SIGUSR1); /* out of GH_VCPU_RUN */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 2;
        if (pthread_timedjoin_np(vthread[i], NULL, &until) != 0) printf("vCPU %d's thread did not return\n", i);
    }
    printf("guest ran: %d exits%s\n", vexits[0], timed_out ? ", stopped after the time allowed" : "");
    crumb("ok: the guest ran (%d exits%s)", vexits[0], timed_out ? ", out of time" : "");

#define HAVE(i) (seen & (1u << (i)))
    if (HAVE(0)) printf("MIDR                   %#llx\n", (unsigned long long)rep[0]);
    if (HAVE(1)) {
        unsigned tg16 = (unsigned)(rep[1] >> 20) & 0xf;
        printf("16K pages              %s (MMFR0 %#llx)\n", tg16 ? "yes" : "not in the ID register Gunyah shows the guest; --mmu16k tries them", (unsigned long long)rep[1]);
    }
    if (HAVE(2)) printf("pointer auth (ISAR1)   %#llx\n", (unsigned long long)rep[2]);
    if (HAVE(3)) printf("exception level        EL%u\n", (unsigned)(rep[3] >> 2) & 3);
    if (HAVE(4)) printf("PAC hypercall          %s%s\n", hvc_result(rep[4]), rep[4] ? ": the engine rewrites the guest's calls" : "");
    if (HAVE(5)) printf("SMCCC OEM UID          %s\n", hvc_result(rep[5]));
    if (HAVE(6)) printf("PSCI version           %#llx\n", (unsigned long long)rep[6]);
    if (HAVE(7)) printf("PFR0                   %#llx\n", (unsigned long long)rep[7]);
    if (HAVE(8)) printf("guest exception        ESR %#llx at %#llx\n", (unsigned long long)rep[8], (unsigned long long)rep[9]);
    if (poke) printf("store at %#llx        %s\n", (unsigned long long)poke,
                     poked ? "came out to this program, as a device's must" : HAVE(14) ? "did NOT come out: it went to memory, or nowhere"
                           : "did NOT come out: the guest took an exception there");
    const char *why = NULL;
    static char whybuf[160];
    uint64_t pages = touch ? touch_size >> 12 : 0;
    if (mmu16k) printf("16 KB pages, tried     %s\n", !table ? "not tried: no room for the table in this layout"
                       : HAVE(16) && rep[16] == 0x16c ? "they WORK: the MMU on, memory read through them"
                       : HAVE(16) ? "the MMU came on, and memory read back wrong" : "the guest did not come back from turning the MMU on");
    if (touch && touch_size) {
        if (HAVE(10)) printf("pages written          %llu of %llu\n", (unsigned long long)rep[10], (unsigned long long)pages);
        if (HAVE(11)) printf("pages read back wrong  %llu\n", (unsigned long long)rep[11]);
    }
    if (ncpus > 1) {
        if (HAVE(12)) printf("PSCI CPU_ON            %s\n", hvc_result(rep[12]));
        if (HAVE(13)) printf("CPUs that came up      %llu of %d\n", (unsigned long long)rep[13], ncpus - 1);
    }
    if (!HAVE(15)) why = timed_out ? "the guest did not finish in the time allowed" : "the guest did not finish";
    else if (rep[15] != 0xd0e) why = "the guest took an exception";
    else if (touch && touch_size && (!HAVE(10) || rep[10] != pages)) why = "not every page was written";
    else if (touch && touch_size && (!HAVE(11) || rep[11] != 0)) why = "pages read back wrong";
    else if (mmu16k && table && !(HAVE(16) && rep[16] == 0x16c)) why = "16 KB pages did not work";
    else if (poke && !poked) why = "a store at a device's address did not come out of the guest";
    else if (ncpus > 1 && (!HAVE(13) || rep[13] != (uint64_t)(ncpus - 1))) {
        snprintf(whybuf, sizeof whybuf, "%llu of %d other CPUs came up", HAVE(13) ? (unsigned long long)rep[13] : 0ull, ncpus - 1);
        why = whybuf;
    }

    if (peek) {
        crumb("try: read a byte of the first region from the host (it is %s)", lend_image ? "lent" : "shared");
        fflush(stdout);
        pid_t child = fork();
        if (child == 0) {
            volatile uint8_t b = image[0x200000 - 0x1000 < L->image_size ? 0x200000 - 0x1000 : 0];
            _exit(b == 0 ? 0 : 1); /* either way it was read */
        }
        int st = 0;
        waitpid(child, &st, 0);
        if (WIFEXITED(st)) printf("host reads its memory  yes: the first region is still the host's to read\n");
        else printf("host reads its memory  NO: the read was stopped by signal %d\n", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
        crumb("ok: the read ended (%s)", WIFEXITED(st) ? "read" : "signal");
        if (!WIFEXITED(st) && !why) why = "the host cannot read memory lent to the VM";
    }
    crumb("try: take the VM down");
    for (int i = 0; i < ncpus; i++) close(vcpu_fd[i]);
    close(vm);
    close(dev);
    crumb("ok: the VM is down");
    if (why) {
        printf("RESULT: FAIL %s\n", why);
        crumb("end: FAIL %s", why);
        return 1;
    }
    printf("RESULT: PASS\n");
    crumb("end: PASS");
    return 0;
early:
    crumb("try: take the VM down (stopped after `%s` as asked)", until);
    for (int i = 0; i < ncpus; i++) if (vcpu_fd[i] > 0) close(vcpu_fd[i]);
    if (vm >= 0) close(vm);
    if (dev >= 0) close(dev);
    crumb("ok: the VM is down");
    printf("RESULT: PASS (stopped after `%s` as asked)\n", until);
    crumb("end: PASS");
    return 0;
fail:
    printf("RESULT: FAIL a step was refused (above)\n");
    crumb("end: FAIL a step was refused");
    return 1;
}
