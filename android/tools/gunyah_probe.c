/* gunyah-probe: what this tablet's Gunyah hypervisor lets an app-made VM do, for deciding whether HengeVM
   can run the guest on it instead of the JIT. Run as root (the app's device check does, through su).

   It creates a 2 MB VM through /dev/gunyah (the Android GH_* interface, include/uapi/linux/gunyah.h),
   boots a tiny EL1 program (gunyah_probe_guest.S) and reports, through MMIO writes it catches here:
     - MIDR, and whether 16K pages exist (macOS cannot run without them)
     - what the hypervisor answers to vmapple's PAC hypercall (XNU stops at boot unless it gets 0) and to
       SMCCC/PSCI queries: the GH_* interface forwards only MMIO to the VMM, so every HVC is Gunyah's own
     - any exception the guest takes instead (ESR, ELR)
   Each step prints ok or the error, so a failure shows exactly where the tablet stops it. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
#define GH_FN_VCPU 1
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

#define BASE 0x80000000ull
#define SIZE 0x200000ull
#define DTB_OFF 0x100000ull
#define REPORT 0x10000000ull

static uint64_t rep[16];
static unsigned seen;
static volatile sig_atomic_t timed_out;
static void on_alarm(int s) { (void)s; timed_out = 1; }

static int step(const char *what, long r) {
    if (r < 0) printf("%-22s FAILED: %s (errno %d)\n", what, strerror(errno), errno);
    else printf("%-22s ok\n", what);
    return r < 0;
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("gunyah-probe: uid %d\n", (int)getuid());
    int dev = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
    if (step("open /dev/gunyah", dev)) return 1;
    int vm = ioctl(dev, GH_CREATE_VM, 0);
    if (step("create VM", vm)) return 1;

    uint8_t *mem = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(mem, probe_guest, sizeof probe_guest);
    memcpy(mem + DTB_OFF, probe_dtb, sizeof probe_dtb);
    struct gh_userspace_memory_region region = {.label = 0, .flags = GH_MEM_RWX, .guest_phys_addr = BASE,
                                                .memory_size = SIZE, .userspace_addr = (uintptr_t)mem};
    if (step("give guest memory", ioctl(vm, GH_VM_SET_USER_MEM_REGION, &region))) return 1;
    struct gh_vm_dtb_config dtb = {.guest_phys_addr = BASE + DTB_OFF, .size = 0x1000};
    if (step("set device tree", ioctl(vm, GH_VM_SET_DTB_CONFIG, &dtb))) return 1;
    uint32_t id = 0;
    struct gh_fn_desc fn = {.type = GH_FN_VCPU, .arg_size = sizeof id, .arg = (uintptr_t)&id};
    int vcpu = ioctl(vm, GH_VM_ADD_FUNCTION, &fn);
    if (step("add vCPU", vcpu)) return 1;
    if (step("start VM", ioctl(vm, GH_VM_START, 0))) return 1;
    long sz = ioctl(vcpu, GH_VCPU_MMAP_SIZE, 0);
    if (sz < 0) sz = ioctl(dev, GH_VCPU_MMAP_SIZE, 0);
    if (step("vCPU run area size", sz)) return 1;
    struct gh_vcpu_run *run = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) { step("map vCPU run area", -1); return 1; }

    struct sigaction sa = {.sa_handler = on_alarm}; /* no SA_RESTART: the alarm breaks a stuck GH_VCPU_RUN */
    sigaction(SIGALRM, &sa, NULL);
    alarm(6); /* under the device check's 10 s limit */
    int exits = 0, done = 0;
    while (!done && !timed_out && exits < 1000) {
        if (ioctl(vcpu, GH_VCPU_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            step("run vCPU", -1);
            break;
        }
        exits++;
        if (run->exit_reason == GH_VCPU_EXIT_MMIO) {
            uint64_t a = run->mmio.phys_addr, v = 0;
            if (run->mmio.is_write && a >= REPORT && a < REPORT + 0x80) {
                memcpy(&v, run->mmio.data, run->mmio.len < 8 ? run->mmio.len : 8);
                unsigned i = (unsigned)((a - REPORT) / 8);
                rep[i] = v, seen |= 1u << i;
                if (i == 15) done = 1;
            } else if (!run->mmio.is_write) {
                memset(run->mmio.data, 0, sizeof run->mmio.data);
            } else {
                printf("guest wrote %#llx at %#llx\n", (unsigned long long)v, (unsigned long long)a);
            }
        } else if (run->exit_reason == GH_VCPU_EXIT_STATUS) {
            printf("VM status %u (1 load failed, 2 exited, 3 crashed), reason type %u\n", run->status.status,
                   run->status.type);
            break;
        } else {
            printf("unknown exit %u\n", run->exit_reason);
            break;
        }
    }
    alarm(0);
    printf("guest ran: %d exits%s\n", exits, timed_out ? ", stopped after 6 s" : "");

#define HAVE(i) (seen & (1u << (i)))
    if (HAVE(0)) printf("MIDR                   %#llx\n", (unsigned long long)rep[0]);
    if (HAVE(1)) {
        unsigned tg16 = (unsigned)(rep[1] >> 20) & 0xf;
        printf("16K pages              %s (MMFR0 %#llx)\n", tg16 ? "yes" : "NO: macOS cannot run here", (unsigned long long)rep[1]);
    }
    if (HAVE(2)) printf("pointer auth           %s (ISAR1 %#llx)\n", (rep[2] & 0xff0) ? "yes" : "no", (unsigned long long)rep[2]);
    if (HAVE(3)) printf("guest EL               %llu\n", (unsigned long long)(rep[3] >> 2) & 3);
    if (HAVE(4)) printf("vmapple PAC hypercall  %s%s\n", hvc_result(rep[4]),
                        rep[4] ? ": XNU would stop at boot" : "");
    if (HAVE(5)) printf("SMCCC OEM UID          %#llx\n", (unsigned long long)rep[5]);
    if (HAVE(6)) printf("PSCI version           %#llx\n", (unsigned long long)rep[6]);
    if (HAVE(7)) printf("PFR0                   %#llx\n", (unsigned long long)rep[7]);
    if (HAVE(8)) printf("guest took exception   ESR %#llx ELR %#llx\n", (unsigned long long)rep[8], (unsigned long long)rep[9]);
    if (HAVE(15)) printf("guest finished         %s\n", rep[15] == 0xd0e ? "yes" : "in its exception handler");
    else printf("guest finished         no (it never reported back)\n");
    return 0;
}
