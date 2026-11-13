/* hengevm-android: boot an installed virtual Mac headless, serial console on stderr.
   The disk, aux and identity come from an install made by Henge on an iPhone or iPad
   (or hengevm on a Mac); copy them to the device and point this at them. */
#include "vm.h"
#include "vnc.h"
#include "fetch.h"
#include "install.h"
#include "gunyah.h"
#include "machine.h"
#include "cpu.h"
#include "vmapple.h"
#include "virtio_usb.h"
#include "pvg_lite.h"
#include <sys/stat.h>
#include <curl/curl.h>
/* Graphics: reims (pvg_reims.c): what crossed to the app's GPU device. */
void hv_pvg_reims_activity(uint64_t *requests, uint64_t *irqs);
uint64_t hv_pvg_reims_wait_ns(void);

static void curl_global_init_all(void) { curl_global_init(CURL_GLOBAL_ALL); }

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void hv_android_share_with_app(const char *path); /* pvg_android.c */

/* The tablet's own state in the heartbeat: when the whole Mac slows at once (0.3.27: frames from 10 ms to
   400 ms, the copies' comparisons twenty times slower, the Mac's CPUs a quarter as fast, all together) the
   cause is the tablet's, and the log had nothing to tell memory from heat. So: the memory Android can still
   give, how much swap (zram, or Xiaomi's "memory extension" on flash) is in use, the kernel's memory pressure
   (the share of the last 10 s in which something waited for memory), the hottest sensor, and whether the
   fastest core is being held below its top frequency. Each part is left out where the tablet does not let
   the app read it. */
static long long file_number(const char *path) {
    FILE *f = fopen(path, "r");
    long long v = -1;
    if (f) {
        if (fscanf(f, "%lld", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

static void host_state(FILE *out) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[160];
        long long total = -1, avail = -1, swap_total = -1, swap_free = -1, huge = -1, whole = -1, v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %lld kB", &v) == 1) total = v;
            else if (sscanf(line, "MemAvailable: %lld kB", &v) == 1) avail = v;
            else if (sscanf(line, "SwapTotal: %lld kB", &v) == 1) swap_total = v;
            else if (sscanf(line, "SwapFree: %lld kB", &v) == 1) swap_free = v;
            else if (sscanf(line, "ShmemHugePages: %lld kB", &v) == 1) huge = v;
            else if (sscanf(line, "ShmemPmdMapped: %lld kB", &v) == 1) whole = v;
        }
        fclose(f);
        if (total > 0 && avail >= 0) fprintf(out, " | tablet: %lld of %lld MB of memory available", avail >> 10, total >> 10);
        /* Settings > Huge pages: how much shared memory (the Mac's RAM is nearly all of it) lies in 2 MB pages,
           and how much of that a process maps as such, which is what spares the TLB. Shown while asked for,
           0 included: "0 MB" is the answer where the kernel gave none (the tablet, build 57, On: no figure). */
        if (huge > 0 || (huge == 0 && getenv("HV_RAM_THP")))
            fprintf(out, ", %lld MB in huge pages (%lld MB mapped whole)", huge >> 10, whole > 0 ? whole >> 10 : 0);
        if (swap_total > 0 && swap_free >= 0) fprintf(out, ", %lld of %lld MB of swap used", (swap_total - swap_free) >> 10, swap_total >> 10);
    }
    /* The engine's own page faults since the last line, and those of them that had to wait for a page to come
       back from swap or storage (/proc/self/stat: minflt and majflt, the 10th and 12th fields). Android lets an
       app read these where it refuses /proc/vmstat (the tablet, 2026-11-13: no swap figures in the log). A fault
       that waits stops one of the Mac's CPUs for as long. */
    f = fopen("/proc/self/stat", "r");
    if (f) {
        static unsigned long long last_minor, last_major;
        static int have;
        char text[1024];
        size_t n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = 0;
        const char *after = strrchr(text, ')'); /* the process's name may hold anything, spaces and brackets too */
        unsigned long long minor = 0, major = 0;
        if (after && sscanf(after + 1, " %*c %*d %*d %*d %*d %*d %*u %llu %*u %llu", &minor, &major) == 2) {
            if (have) fprintf(out, ", the engine took %llu page faults, %llu of them waiting for swap or storage", minor - last_minor, major - last_major);
            last_minor = minor, last_major = major, have = 1;
        }
    }
    /* What the tablet moved through swap since the last line (/proc/vmstat counts pages since boot), and the
       page faults that had to wait for it: on a tablet short of memory every page that comes back is unpacked
       on the same cores the Mac's CPUs run on, while one of them stands still. */
    f = fopen("/proc/vmstat", "r");
    if (f) {
        static long long last_in = -1, last_out, last_major;
        char line[160];
        long long in = -1, outp = -1, major = -1, v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "pswpin %lld", &v) == 1) in = v;
            else if (sscanf(line, "pswpout %lld", &v) == 1) outp = v;
            else if (sscanf(line, "pgmajfault %lld", &v) == 1) major = v;
        }
        fclose(f);
        if (in >= 0 && outp >= 0) {
            long long page = sysconf(_SC_PAGESIZE);
            if (last_in >= 0)
                fprintf(out, " (%lld MB swapped in and %lld MB out since the last line, %lld page faults waited for storage or swap)",
                        (in - last_in) * page >> 20, (outp - last_out) * page >> 20, major >= 0 ? major - last_major : 0);
            last_in = in, last_out = outp, last_major = major;
        }
    }
    f = fopen("/proc/pressure/memory", "r");
    if (f) {
        char line[160];
        double some = -1, full = -1, v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "some avg10=%lf", &v) == 1) some = v;
            else if (sscanf(line, "full avg10=%lf", &v) == 1) full = v;
        }
        fclose(f);
        if (some >= 0) fprintf(out, ", waiting for memory %.0f%% of the time (everything stalled %.0f%%)", some, full < 0 ? 0 : full);
    }
    long long hottest = -1;
    for (int z = 0; z < 120; z++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        long long milli = file_number(path);
        if (milli > 1000 && milli < 130000 && milli > hottest) hottest = milli; /* millidegrees; skips unset and absurd ones */
    }
    if (hottest > 0) fprintf(out, ", hottest sensor %lld C", hottest / 1000);
    /* The last CPU is the fastest kind on these chips: its allowed top frequency against its real one. */
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu > 0) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_max_freq", ncpu - 1);
        long long allowed = file_number(path);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", ncpu - 1);
        long long top = file_number(path);
        if (allowed > 0 && top > 0) {
            if (allowed < top) fprintf(out, ", fastest core held to %.1f of %.1f GHz", allowed / 1e6, top / 1e6);
            else fprintf(out, ", fastest core not held back");
        }
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage (both forms also take --engine jit|gunyah): hengevm-android --install FOLDER [--release 15.6.1] [--ipsw FILE] [--disk-gb 64] [--rom FILE] [--cache DIR] [--ram MB] [--cpus N]\n"
            "   or: hengevm-android --mac FOLDER (exported from Henge on iOS) [options]\n"
            "   or: hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage\n"
            "                       --ecid N --seed N [--ram MB (4096)] [--cpus N (2)] [--log 0-3] [--jit-mb MB] [--net 0|1]\n"
            "                       [--vnc PORT (5900, 0 = off)] [--gpu on|off|capture|vulkan|reims|applegfx] [--shader-dir DIR]\n"
            "  --gpu off      no paravirtual GPU (experiment: macOS may fall back to a plain framebuffer)\n"
            "  --gpu capture  keep the GPU device and save every shader library and pipeline macOS uses\n"
            "                 (default DIR: shaders/ next to the disk), for building the Vulkan translation\n"
            "  --gpu vulkan   hand the GPU's commands to the Android app's Vulkan renderer through gpu.link\n"
            "                 next to the disk (HV_GPU_LINK overrides); guest RAM stays a named file it maps\n"
            "  --gpu reims    the GPU device is reims-vgpu in the Android app, on Vulkan, through gpu.reims next to\n"
            "                 the disk (HV_GPU_REIMS overrides); guest RAM stays a named file it maps\n"
            "  --gpu applegfx a second, independent GPU host (libapplegfx-vulkan) instead of reims, through\n"
            "                 gpu.applegfx next to the disk (HV_GPU_APPLEGFX overrides); falls back if not built in\n");
    exit(2);
}

/* A Mac exported from Henge on iOS: AVPBooter.vmapple2.bin, auxiliary.bin, disk.img, machine.json. */
static uint64_t json_u64(const char *js, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(js, pat);
    if (!k || !(k = strchr(k + strlen(pat), ':'))) return 0;
    return strtoull(k + 1, NULL, 10);
}

static void load_mac(HvVmConfig *vc, const char *dir) {
    static char rom[1024], aux[1024], disk[1024], info[1024], js[4096];
    snprintf(rom, sizeof rom, "%s/AVPBooter.vmapple2.bin", dir);
    snprintf(aux, sizeof aux, "%s/auxiliary.bin", dir);
    snprintf(disk, sizeof disk, "%s/disk.img", dir);
    snprintf(info, sizeof info, "%s/machine.json", dir);
    vc->rom = rom, vc->aux = aux, vc->disk = disk;
    FILE *f = fopen(info, "r");
    if (!f) { fprintf(stderr, "hengevm: no %s\n", info); exit(1); }
    size_t n = fread(js, 1, sizeof js - 1, f);
    fclose(f);
    js[n] = 0;
    vc->ecid = json_u64(js, "ecid"), vc->seed = json_u64(js, "seed");
    uint64_t ram = json_u64(js, "ramMB");
    if (ram) vc->ram_mb = ram;
    fprintf(stderr, "hengevm: %s: ECID %llu, %llu MB\n", dir, (unsigned long long)vc->ecid, (unsigned long long)vc->ram_mb);
}

/* The boot ROM at path, from the download cache when it holds a genuine one, else from Apple; the
   cache keeps a copy so the next install or run needs no network. */
static int ensure_rom(const char *path, const char *cache) {
    char cached[1100], cmd[2400];
    if (cache) {
        snprintf(cached, sizeof cached, "%s/AVPBooter.vmapple2.bin", cache);
        if (hv_fetch_boot_rom(cached) != 0) return -1;
        if (strcmp(cached, path)) {
            snprintf(cmd, sizeof cmd, "cp '%s' '%s'", cached, path);
            if (system(cmd) != 0) return -1;
        }
        return 0;
    }
    return hv_fetch_boot_rom(path);
}

static volatile int install_done = -2;
/* A failed install leaves nothing worth flushing, and the engine must not linger as "Running"
   with the restore tools' last lines unwritten: if the Mac has not stopped in 20 s, leave. */
static void *install_watchdog(void *arg) {
    (void)arg;
    sleep(20);
    fprintf(stderr, "henge: the engine did not stop after the failed install; exiting\n");
    fflush(stdout);
    _exit(1);
}
/* Every 30 s (installing or booting), where each guest CPU is, what the disk and GPU did, and who owns the USB port.
   A boot that gives no serial output still shows here whether the kernel runs (EL1 PCs at 0xfffffe..., blocks
   climbing), spins in one place, sleeps (blocks flat), stopped (halted), and whether it does disk or GPU work.
   Idle is the same PC on every CPU with a few thousand blocks: that alone is not a hang. A hang is no CPU moving
   off its PC and no disk or GPU work for 2 minutes; then each CPU's registers go to the log, once per hang. */
static int heartbeat_no_cpus; /* Gunyah runs the CPU: its state is not ours to read */

/* The Mac's CPUs on the tablet's fast cores. A phone's cores are of two or three speeds (the Mi Pad 7: one
   at 2.8 GHz, four at 2.6, three at 1.9), and nothing kept a guest CPU's thread, which translates and runs
   the Mac's code flat out, off the slow ones. HV_CPU_ANY=1 leaves them where Android puts them. host/vm.c
   calls this (weak there).

   0.3.56: each of the Mac's CPUs has a fast core of its own, where there are as many (HV_CPU_PIN=0: any of the
   fast cores, as before). The four threads floated over the five fast cores: the log's "waited for a core" was
   3 to 10% of their time, and a thread that moves leaves its core's caches behind. The Mac's CPU 0 takes the
   fastest core (it is the one that polls the devices too), the others the next ones down, highest core number
   first, so what is left over is the lowest-numbered fast core: nothing of the engine's runs there, which
   leaves it to the app's GPU threads (the app keeps Android's other groups off the Mac's cores with root:
   VmService.coreTuning, which reckons the same cores by the same rule). The engine's other threads (disks,
   network, sound, VNC, the heartbeat) are kept to the slow cores: they wait more than they work. Not while
   installing, where the restore tools' threads do the work. */
static cpu_set_t fast_set, slow_set;
static int fast_count, fast_total, slow_count;
static int fast_order[64];              /* the fast cores, fastest first, of one speed the highest number first */
static long core_khz[64];
static pid_t fast_tids[HV_MAX_CPUS];
static cpu_set_t own_set[HV_MAX_CPUS];  /* what each of those threads is bound to */
static int fast_ntids;
static int pin_cores, pin_helpers;      /* a core of its own per CPU; the other threads on the slow cores */
int hv_runner_installing;               /* set by main before any thread is made */

/* The engine's threads that are not the Mac's CPUs, onto the slow cores. New ones are found at the next pass. */
static void helpers_to_slow(void) {
    if (!pin_helpers || !slow_count) return;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    int n = __atomic_load_n(&fast_ntids, __ATOMIC_ACQUIRE);
    if (n > HV_MAX_CPUS) n = HV_MAX_CPUS;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        pid_t tid = (pid_t)atoi(e->d_name);
        if (tid <= 0) continue;
        int cpu_thread = 0;
        for (int k = 0; k < n; k++) cpu_thread |= fast_tids[k] == tid;
        if (!cpu_thread) sched_setaffinity(tid, sizeof slow_set, &slow_set);
    }
    closedir(d);
}

/* Again, now and then: Android (or the app, moving the engine into the top-app group with root) may move
   the process between scheduling groups, and Linux 6.1 gives every thread the new group's cores when it does. */
static void *fast_again(void *unused) {
    (void)unused;
    for (int i = 0;; i++) {
        sleep(i < 4 ? 3 : 30);
        int n = __atomic_load_n(&fast_ntids, __ATOMIC_ACQUIRE);
        for (int k = 0; k < n && k < HV_MAX_CPUS; k++) sched_setaffinity(fast_tids[k], sizeof own_set[k], &own_set[k]);
        helpers_to_slow();
        if (i == 1 && pin_helpers && slow_count)
            fprintf(stderr, "hengevm: the engine's other threads (disks, network, sound) are kept to the %d slow cores\n", slow_count);
    }
    return NULL;
}
static pthread_once_t fast_once = PTHREAD_ONCE_INIT;
static void fast_find(void) {
    long lo = 0, hi = 0;
    int n = 0;
    for (; n < 64; n++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        FILE *f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%ld", &core_khz[n]) != 1) core_khz[n] = 0;
        fclose(f);
        if (!n || core_khz[n] < lo) lo = core_khz[n];
        if (core_khz[n] > hi) hi = core_khz[n];
    }
    fast_total = n;
    CPU_ZERO(&fast_set);
    CPU_ZERO(&slow_set);
    if (!n || lo == hi) return; /* unreadable, or all of one speed: nothing to choose */
    for (int i = 0; i < n; i++) {
        if (core_khz[i] > lo) CPU_SET(i, &fast_set), fast_order[fast_count++] = i;
        else CPU_SET(i, &slow_set), slow_count++;
    }
    /* Fastest first; of one speed, the highest number first (insertion sort: a handful of cores). */
    for (int i = 1; i < fast_count; i++)
        for (int k = i; k > 0; k--) {
            int a = fast_order[k - 1], b = fast_order[k];
            if (core_khz[a] > core_khz[b] || (core_khz[a] == core_khz[b] && a > b)) break;
            fast_order[k - 1] = b, fast_order[k] = a;
        }
    const char *pin = getenv("HV_CPU_PIN");
    pin_cores = !(pin && *pin == '0') && hv_vm_ncpus() <= fast_count;
    pin_helpers = pin_cores && !hv_runner_installing;
    pthread_t th;
    if (pthread_create(&th, NULL, fast_again, NULL) == 0) pthread_detach(th);
}
void hv_host_cpu_thread(void) {
    if (getenv("HV_CPU_ANY")) return;
    pthread_once(&fast_once, fast_find);
    if (!fast_count) return;
    static int said;
    int index = hv_vm_this_cpu();
    cpu_set_t own = fast_set;
    int core = -1;
    if (pin_cores && index >= 0 && index < fast_count) {
        core = fast_order[index];
        CPU_ZERO(&own);
        CPU_SET(core, &own);
    }
    int slot = __atomic_fetch_add(&fast_ntids, 1, __ATOMIC_ACQ_REL);
    if (slot < HV_MAX_CPUS) own_set[slot] = own, fast_tids[slot] = gettid();
    int rc = sched_setaffinity(0, sizeof own, &own);
    if (rc != 0 && core >= 0) { /* that core is not ours to have (offline, or not in Android's set for us): any fast one */
        fprintf(stderr, "hengevm: the Mac's CPU %d could not be given core %d (%s); it runs on any fast core\n", index, core, strerror(errno));
        own = fast_set, core = -1;
        if (slot < HV_MAX_CPUS) own_set[slot] = own;
        rc = sched_setaffinity(0, sizeof own, &own);
    }
    if (core >= 0) {
        fprintf(stderr, "hengevm: the Mac's CPU %d has core %d of its own (%.1f GHz)\n", index, core, core_khz[core] / 1e6);
        if (!__atomic_exchange_n(&said, 1, __ATOMIC_ACQ_REL)) {
            char left[160] = "";
            size_t m = 0;
            for (int k = hv_vm_ncpus(); k < fast_count && m < sizeof left - 8; k++)
                m += (size_t)snprintf(left + m, sizeof left - m, "%s%d", m ? ", " : "", fast_order[k]);
            fprintf(stderr, "hengevm: %d fast cores of %d for the Mac's %d CPUs, one each; %s%s\n", hv_vm_ncpus(), fast_total, hv_vm_ncpus(),
                    left[0] ? "left to the app's GPU threads: core " : "no fast core is left over", left);
        }
    } else if (!__atomic_exchange_n(&said, 1, __ATOMIC_ACQ_REL)) {
        if (rc == 0) fprintf(stderr, "hengevm: the Mac's CPUs run on the tablet's %d fast cores of %d\n", fast_count, fast_total);
        else fprintf(stderr, "hengevm: could not keep the Mac's CPUs on the fast cores: %s\n", strerror(errno));
    }
}
static void dump_cpu(int i, HvCpu *c) {
    fprintf(stderr, "henge: stuck? cpu%d EL%u pc %llx sp %llx x29 %llx x30 %llx | elr %llx spsr %llx esr %llx far %llx | "
                    "x0 %llx x1 %llx x2 %llx x3 %llx daif %llx\n",
            i, c->el, (unsigned long long)c->pc, (unsigned long long)c->sp, (unsigned long long)c->x[29],
            (unsigned long long)c->x[30], (unsigned long long)c->elr_el1, (unsigned long long)c->spsr_el1,
            (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->daif);
}
/* How long the Mac's CPU threads ran and how long they stood ready with no core to run on, since the last line
   (/proc/self/task/<tid>/schedstat: run time and run-queue wait, in ns). A CPU thread that waits has lost its
   core to another thread: the app's GPU threads, Android. What is left of the interval it slept (the Mac idle,
   or a page fault). */
static void cpu_threads_state(FILE *out) {
    static uint64_t last_run[HV_MAX_CPUS], last_wait[HV_MAX_CPUS];
    static struct timespec last_t;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double dt = last_t.tv_sec ? (double)(t.tv_sec - last_t.tv_sec) + (t.tv_nsec - last_t.tv_nsec) / 1e9 : 0;
    last_t = t;
    int n = __atomic_load_n(&fast_ntids, __ATOMIC_ACQUIRE), read = 0;
    if (n > HV_MAX_CPUS) n = HV_MAX_CPUS;
    double ran = 0, waited = 0;
    for (int k = 0; k < n; k++) {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", (int)fast_tids[k]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long run = 0, wait = 0;
        if (fscanf(f, "%llu %llu", &run, &wait) == 2) {
            if (last_run[k] || last_wait[k]) ran += (run - last_run[k]) / 1e9, waited += (wait - last_wait[k]) / 1e9, read++;
            last_run[k] = run, last_wait[k] = wait;
        }
        fclose(f);
    }
    if (read && dt > 0)
        fprintf(out, " | the Mac's %d CPU threads ran %.0f%% of the time and waited %.1f%% of it for a core", read,
                100.0 * ran / (dt * read), 100.0 * waited / (dt * read));
}

static void *install_heartbeat(void *arg) {
    (void)arg;
    uint64_t last[HV_MAX_CPUS] = {0}, last_pc[HV_MAX_CPUS] = {0};
    uint64_t last_disk = 0, last_gpu = 0, last_wait = 0;
    uint64_t last_busy = 0, last_fl = 0, last_flus = 0, last_spared = 0, last_tr = 0, last_full = 0, last_drop = 0;
    int still = 0, reported = 0;
    for (int t = 30; install_done == -2; t += 30) {
        sleep(30);
        HvVmapple *b = hv_vm_board();
        HvVirtioUsb *u = b->vusb;
        int moved = 0;
        fprintf(stderr, "henge: guest %d s:", t);
        for (int i = 0; i < hv_vm_ncpus() && !heartbeat_no_cpus; i++) {
            HvCpu *c = hv_vm_cpu(i);
            uint64_t n = c->icount;
            if (c->halted) fprintf(stderr, " cpu%d stopped at %llx", i, (unsigned long long)c->pc);
            else if (c->off) fprintf(stderr, " cpu%d off", i);
            else {
                fprintf(stderr, " cpu%d EL%u %llx +%llu", i, c->el, (unsigned long long)c->pc,
                        (unsigned long long)(n - last[i]));
                if (c->pc != last_pc[i]) moved = 1;
            }
            last[i] = n, last_pc[i] = c->pc;
        }
        uint64_t disk = (b->root_blk ? b->root_blk->requests : 0) + (b->aux_blk ? b->aux_blk->requests : 0);
        uint64_t cmds = 0, swaps = 0;
        if (b->pvg) {
            hv_pvg_lite_activity(b->pvg, &cmds, &swaps);
            fprintf(stderr, " | disk +%llu | gpu +%llu cmds, %llu frames", (unsigned long long)(disk - last_disk),
                    (unsigned long long)(cmds - last_gpu), (unsigned long long)swaps);
        } else { /* Graphics: reims: the device is the app's; count what crosses to it */
            hv_pvg_reims_activity(&cmds, &swaps);
            /* How long the Mac's CPUs stood waiting for the app's GPU to answer a register, of this
               interval's 30 s per CPU: what the GPU costs the Mac, apart from the CPU's own slowness. */
            uint64_t wait = hv_pvg_reims_wait_ns();
            fprintf(stderr, " | disk +%llu | gpu (reims) +%llu register accesses, %llu interrupts, CPUs waited %.1f s on it",
                    (unsigned long long)(disk - last_disk), (unsigned long long)(cmds - last_gpu), (unsigned long long)swaps,
                    (wait - last_wait) / 1e9);
            last_wait = wait;
        }
        if (disk != last_disk || cmds != last_gpu) moved = 1;
        last_disk = disk, last_gpu = cmds;
        {
            /* The disk's time and the JIT's upkeep, of this 30 s: requests' time in the workers, syncs of flash
               (and those spared, nothing having been written), blocks translated, and translations thrown away
               (all of them: "full"). */
            uint64_t busy = 0, fl = 0, flus = 0, spared = 0;
            HvVirtioBlk *bl[2] = {b->root_blk, b->aux_blk};
            for (int i = 0; i < 2; i++) {
                if (!bl[i]) continue;
                busy += __atomic_load_n(&bl[i]->busy_us, __ATOMIC_RELAXED), fl += __atomic_load_n(&bl[i]->flushes, __ATOMIC_RELAXED);
                flus += __atomic_load_n(&bl[i]->flush_us, __ATOMIC_RELAXED);
                spared += __atomic_load_n(&bl[i]->disk->flushes_spared, __ATOMIC_RELAXED);
            }
            HvMachine *m = hv_vm_machine();
            fprintf(stderr, " | disk time %.1f s, %llu flushes (%.1f s, %llu spared) | jit +%llu blocks, %llu drops (%llu full), %.0f MB in use (%.0f MB given back at flushes)",
                    (busy - last_busy) / 1e6, (unsigned long long)(fl - last_fl), (flus - last_flus) / 1e6,
                    (unsigned long long)(spared - last_spared), (unsigned long long)(m->stats.translations - last_tr),
                    (unsigned long long)(m->stats.block_flushes - last_drop), (unsigned long long)(m->stats.flush_full - last_full),
                    (double)m->jit.used * 4 / (1 << 20), (double)m->jit.released / (1 << 20));
            /* What the translating costs and what it translates (0.3.54; the counters are core/dispatch.c's): the
               time in the translator of all CPUs together; how many of the blocks were in the cache before its
               last drop (the same code again: the cache is smaller than what the Mac runs) and how many of those
               at the same place in the Mac's memory (the rest moved: the Mac paged it out and in); a block's size
               in the Mac's instructions and in bytes of the cache; code pages the Mac wrote over (their blocks die
               at once and their space with them until the next drop), blocks two CPUs made at once, and how much
               of what the last drops threw away was dead already. */
            static HvStats was, now;
            now = m->stats;
            uint64_t tr = now.translations - was.translations;
            uint64_t words = (now.words_inline - was.words_inline) + (now.words_ool - was.words_ool) + (now.words_pool - was.words_pool);
            uint64_t sampled = now.drop_sampled - was.drop_sampled;
            if (tr)
                fprintf(stderr, "; translating took %.2f s of the CPUs' time; %.0f%% of the blocks were in the cache before its last drop (%.0f%% at the same "
                                "memory); a block is %.1f instructions, %.0f bytes; %llu code pages written over, %llu blocks made twice",
                        (now.translate_ticks - was.translate_ticks) / 24e6, 100.0 * (now.retrans_va - was.retrans_va) / tr,
                        100.0 * (now.retrans_vapa - was.retrans_vapa) / tr, (double)(now.translated_insns - was.translated_insns) / tr,
                        4.0 * words / tr, (unsigned long long)(now.flush_smc - was.flush_smc),
                        (unsigned long long)(now.dup_translations - was.dup_translations));
            if (sampled) fprintf(stderr, "; %.0f%% of what was dropped was dead already", 100.0 * (now.drop_stale - was.drop_stale) / sampled);
            was = now;
            last_busy = busy, last_fl = fl, last_flus = flus, last_spared = spared;
            last_tr = m->stats.translations, last_full = m->stats.flush_full, last_drop = m->stats.block_flushes;
        }
        cpu_threads_state(stderr);
        host_state(stderr);
        if (u) fprintf(stderr, " | usb: %s, kernel driver %s (status %#x)", u->rom_on ? "iBoot has it" : "iBoot let go",
                       u->pci_on ? "on" : "not started", u->vio.status);
        fputc('\n', stderr);
        if (heartbeat_no_cpus) continue;
        still = moved ? 0 : still + 1;
        if (moved) reported = 0;
        if (still >= 4 && !reported) { /* 2 minutes without anything moving */
            reported = 1;
            for (int i = 0; i < hv_vm_ncpus(); i++) {
                HvCpu *c = hv_vm_cpu(i);
                if (!c->halted && !c->off) dump_cpu(i, c);
            }
        }
    }
    return NULL;
}
static void on_install(int step, double p, void *user) {
    (void)user;
    static const char *names[] = {"detecting", "preparing", "sending the system", "verifying", "flashing firmware",
                                  "flashing", "updating", "finishing"};
    static int last = -1, lastpct = -1;
    if (step == HV_INSTALL_DONE || step == HV_INSTALL_FAILED) {
        install_done = step == HV_INSTALL_DONE ? 0 : -1;
        fprintf(stderr, "\nhenge: install %s\n", step == HV_INSTALL_DONE ? "finished" : "FAILED");
        fflush(stdout);
        if (step == HV_INSTALL_FAILED) {
            pthread_t t;
            if (pthread_create(&t, NULL, install_watchdog, NULL) == 0) pthread_detach(t);
        }
        hv_vm_stop();
        return;
    }
    int pct = (int)(p * 100);
    if (step != last || pct / 5 != lastpct / 5) {
        fprintf(stderr, "\rhenge: install: %s %d%%      ", step >= 0 && step < 8 ? names[step] : "working", pct);
        last = step, lastpct = pct;
    }
}

static volatile sig_atomic_t stopping;
static void on_signal(int sig) {
    (void)sig;
    if (stopping++) _exit(130);
    hv_vm_stop();
}

int main(int argc, char **argv) {
    int vnc_port = 5900;
    int stats_seconds = 0; /* --stats N: the engine's own statistics every N s (core/dispatch.c print_stats), as host/main.c has */
    const char *gpu = "on", *shader_dir = NULL, *engine = "jit";
    const char *install_dir = NULL, *release = "15.6.1", *ipsw = NULL, *cache = NULL;
    uint64_t disk_gb = 64;
    HvVmConfig vc = {.aux_offset = 16384, .ram_mb = 4096, .ncpus = 2, .log = 1, .no_net = 0, .no_sound = 0};
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        const char *a = argv[i], *v = argv[++i];
        if (!strcmp(a, "--install")) install_dir = v;
        else if (!strcmp(a, "--release")) release = v;
        else if (!strcmp(a, "--ipsw")) ipsw = v;
        else if (!strcmp(a, "--cache")) cache = v;
        else if (!strcmp(a, "--disk-gb")) disk_gb = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--mac")) load_mac(&vc, v);
        else if (!strcmp(a, "--rom")) vc.rom = v;
        else if (!strcmp(a, "--disk")) vc.disk = v;
        else if (!strcmp(a, "--aux")) vc.aux = v;
        else if (!strcmp(a, "--aux-offset")) vc.aux_offset = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--ecid")) vc.ecid = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--seed")) vc.seed = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--ram")) vc.ram_mb = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--cpus")) vc.ncpus = atoi(v);
        else if (!strcmp(a, "--log")) vc.log = atoi(v);
        else if (!strcmp(a, "--vnc")) vnc_port = atoi(v);
        else if (!strcmp(a, "--gpu")) gpu = v;
        else if (!strcmp(a, "--engine")) engine = v;
        else if (!strcmp(a, "--shader-dir")) shader_dir = v;
        else if (!strcmp(a, "--net")) vc.no_net = !atoi(v);
        else if (!strcmp(a, "--sound")) vc.no_sound = !atoi(v);
        else if (!strcmp(a, "--jit-mb")) vc.jit_bytes = (size_t)atoi(v) << 20;
        else if (!strcmp(a, "--stats")) stats_seconds = atoi(v);
        else usage();
    }
    static char rom_path[1100], ipsw_path[1100], work[1100];
    hv_runner_installing = install_dir != NULL; /* the restore tools' threads keep every core (hv_host_cpu_thread) */
    if (install_dir) {
        curl_global_init_all();
        const HvRelease *r = hv_release_find(release);
        if (!ipsw && !r) {
            fprintf(stderr, "henge: unknown release %s; one of:\n", release);
            hv_release_list(stderr);
            return 2;
        }
        uint64_t ecid;
        struct stat st;
        char info[1100];
        snprintf(info, sizeof info, "%s/machine.json", install_dir);
        if (stat(info, &st) != 0 && hv_new_mac(install_dir, "Mac", vc.ram_mb, disk_gb, &ecid) != 0) {
            fprintf(stderr, "henge: cannot create %s\n", install_dir);
            return 1;
        }
        snprintf(rom_path, sizeof rom_path, "%s/AVPBooter.vmapple2.bin", install_dir);
        if (vc.rom) { /* --rom given: use that file instead of fetching */
            char cmd[2300];
            snprintf(cmd, sizeof cmd, "cp '%s' '%s'", vc.rom, rom_path);
            if (strcmp(vc.rom, rom_path) && system(cmd) != 0) return 1;
        } else if (ensure_rom(rom_path, cache) != 0) {
            return 1;
        }
        if (!ipsw) {
            /* In the cache when given, so a reinstall (or a new Mac) reuses the download. */
            snprintf(ipsw_path, sizeof ipsw_path, "%s/%s.ipsw", cache ? cache : install_dir, r->id);
            struct stat have;
            if (stat(ipsw_path, &have) == 0 && have.st_size > 0)
                fprintf(stderr, "henge: using the saved %s (%s)\n", r->name, ipsw_path);
            fprintf(stderr, "henge: %s\n", r->name);
            if (hv_download(r->url, ipsw_path) != 0) {
                fprintf(stderr, "henge: download failed; run again to resume\n");
                return 1;
            }
            ipsw = ipsw_path;
        }
        uint64_t ram = vc.ram_mb;
        load_mac(&vc, install_dir);
        vc.ram_mb = ram;
        vc.installer = 1;
        snprintf(work, sizeof work, "%s/restore", install_dir);
        mkdir(work, 0755);
        /* The guest runs under the JIT here, many times slower than a real Mac: iBoot alone can
           take over a minute between the kernelcache upload and dropping USB. Stretch the
           restore tools' waits so they don't give up on a machine that is still working. */
        setenv("HV_RESTORE_TIMEOUT_SCALE", "10", 0);
        /* The restore OS's kernel log on the serial console (this log): where a long boot is, or why it stopped. */
        setenv("HV_RESTORE_BOOT_ARGS", "serial=3 -v", 0);
        setenv("HV_PSCI_LOG", "1", 0);
        setenv("HV_INSTALL_LOG", "1", 0); /* the restore tools' debug lines too: the app's normal log view hides them */ /* CPUs started, stopped, the machine reset or powered off: all in the log */
    }
    if (!vc.rom || !vc.disk || !vc.aux) usage();
    int gunyah = !strcmp(engine, "gunyah");
    if (!gunyah && strcmp(engine, "jit")) usage();
    if (gunyah) {
        /* The CPU runs on Gunyah (host/gunyah.c): every CPU asked for, each a vCPU. Guest RAM is pinned for the
           hypervisor, so it must be anonymous: a long-term pin of a file's pages (the RAM file Graphics: Vulkan
           and reims share with the app) leaves them dirty under the file system's writeback, which on this
           kernel (no FOLL_LONGTERM refusal for file pages before Linux 6.5) can panic and reboot the tablet.
           So those two run only with the JIT engine for now; Gunyah gets pvg_lite. */
        if (!strcmp(gpu, "vulkan") || !strcmp(gpu, "reims") || !strcmp(gpu, "applegfx")) {
            fprintf(stderr, "hengevm: Graphics %s needs the JIT engine for now (Gunyah cannot pin a file's pages "
                            "safely); using GPU instead\n", gpu);
            gpu = "on";
        }
        setenv("HV_RAM_FILE", "", 1);
        fprintf(stderr, "hengevm: engine: Gunyah (experimental), %d CPUs\n", vc.ncpus);
    }
    if (!install_dir && access(vc.rom, R_OK) != 0) { /* a Mac whose boot ROM went missing: put it back */
        curl_global_init_all();
        fprintf(stderr, "henge: %s is missing; restoring it\n", vc.rom);
        if (ensure_rom(vc.rom, cache) != 0) return 1;
    }
    if (!strcmp(gpu, "off")) {
        setenv("HV_NO_GPU", "1", 1);
        fprintf(stderr, "hengevm: GPU off (experiment)\n");
    } else if (!strcmp(gpu, "capture") && !vc.installer) {
        static char dir[1100];
        if (!shader_dir) {
            snprintf(dir, sizeof dir, "%s", vc.disk);
            char *slash = strrchr(dir, '/');
            snprintf(slash ? slash + 1 : dir, sizeof dir - (slash ? (size_t)(slash + 1 - dir) : 0), "shaders");
            shader_dir = dir;
        }
        setenv("HV_SHADER_CAPTURE", shader_dir, 1);
    } else if (!strcmp(gpu, "vulkan") && !vc.installer) {
        static char link[1100];
        if (!getenv("HV_GPU_LINK")) {
            snprintf(link, sizeof link, "%s", vc.disk);
            char *slash = strrchr(link, '/');
            snprintf(slash ? slash + 1 : link, sizeof link - (slash ? (size_t)(slash + 1 - link) : 0), "gpu.link");
            setenv("HV_GPU_LINK", link, 1);
        }
        setenv("HV_RAM_KEEP", "1", 1);
    } else if (!strcmp(gpu, "reims") && !vc.installer) {
        /* The GPU device itself is reims-vgpu in the app (android/pvg_reims.c, android/gpu/reims). */
        static char path[1100];
        if (!getenv("HV_GPU_REIMS")) {
            snprintf(path, sizeof path, "%s", vc.disk);
            char *slash = strrchr(path, '/');
            snprintf(slash ? slash + 1 : path, sizeof path - (slash ? (size_t)(slash + 1 - path) : 0), "gpu.reims");
            setenv("HV_GPU_REIMS", path, 1);
        }
        setenv("HV_RAM_KEEP", "1", 1);
    } else if (!strcmp(gpu, "applegfx") && !vc.installer) {
        /* A second, independent GPU host (android/pvg_applegfx.c, libapplegfx-vulkan), chosen instead of reims. */
        static char path[1100];
        if (!getenv("HV_GPU_APPLEGFX")) {
            snprintf(path, sizeof path, "%s", vc.disk);
            char *slash = strrchr(path, '/');
            snprintf(slash ? slash + 1 : path, sizeof path - (slash ? (size_t)(slash + 1 - path) : 0), "gpu.applegfx");
            setenv("HV_GPU_APPLEGFX", path, 1);
        }
        setenv("HV_RAM_KEEP", "1", 1);
    } else if (strcmp(gpu, "on") && strcmp(gpu, "capture") && strcmp(gpu, "vulkan") && strcmp(gpu, "reims") &&
               strcmp(gpu, "applegfx")) {
        usage();
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0); /* the app's log is a file: block buffering held lines back until exit */
    /* Android has no swap to speak of: back guest RAM with a file unless told otherwise. */
    if (!getenv("HV_RAM_FILE")) {
        static char ram[1024];
        snprintf(ram, sizeof ram, "%s.ram", vc.disk);
        setenv("HV_RAM_FILE", ram, 0);
    }
    /* DNS for the guest's NAT: Android has no /etc/resolv.conf, so fall back to public resolvers. */
    if (!vc.no_net && !getenv("SLIRP_RESOLV_CONF") && access("/etc/resolv.conf", R_OK) != 0 &&
        access("/data/data/com.termux/files/usr/etc/resolv.conf", R_OK) != 0) {
        static char conf[1024];
        snprintf(conf, sizeof conf, "%s.resolv.conf", vc.disk);
        FILE *f = fopen(conf, "w");
        if (f) {
            fputs("nameserver 1.1.1.1\nnameserver 8.8.8.8\n", f);
            fclose(f);
            setenv("SLIRP_RESOLV_CONF", conf, 1);
        }
    }
    char err[256];
    if (hv_vm_create(&vc, err, sizeof err) != 0) {
        fprintf(stderr, "hengevm: %s\n", err);
        return 1;
    }
    if (getenv("HV_RAM_KEEP")) hv_android_share_with_app(getenv("HV_RAM_FILE")); /* the renderer opens it by name */
    if (stats_seconds > 0 && !gunyah) {
        /* Settings > Engine statistics: every N s, where the JIT's time goes (TLB misses, slow memory accesses,
           exceptions, interrupts, the Mac's idle share and thirty lines more). Set after the machine is made,
           which clears it. The counters cost a little on the paths they count. */
        hv_vm_machine()->stats_interval = stats_seconds;
        fprintf(stderr, "hengevm: engine statistics every %d s (Settings)\n", stats_seconds);
    }
    if (vnc_port) {
        if (hv_vnc_start(vnc_port) == 0) fprintf(stderr, "hengevm: display on vnc://127.0.0.1:%d\n", vnc_port);
        else fprintf(stderr, "hengevm: vnc port %d unavailable\n", vnc_port);
    }
    if (install_dir && hv_install_start(hv_vm_board()->vusb, ipsw, work, vc.ecid, on_install, NULL) != 0) {
        fprintf(stderr, "henge: the installer could not start\n");
        return 1;
    }
    heartbeat_no_cpus = gunyah;
    { /* installs and normal boots alike: a still screen says nothing about whether the CPUs still run */
        pthread_t hb;
        if (pthread_create(&hb, NULL, install_heartbeat, NULL) == 0) pthread_detach(hb);
    }
    if (install_dir) usleep(500000); /* usbmuxd installs its own handlers; take SIGINT and SIGTERM back */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "hengevm: %d CPUs, %llu MB, network %s, sound %s\n", hv_vm_ncpus(),
            (unsigned long long)vc.ram_mb, vc.no_net ? "off" : "on (NAT)", vc.no_sound ? "off" : "on");
    if (!gunyah) {
        /* CPUs 1 and up need their own threads (parked until PSCI CPU_ON) and the 1 ms kicker. Without them
           XNU's CPU_ON for cpu1 waits forever for a thread that was never started: cpu0 stuck, +0 blocks. */
        hv_vm_start(0, NULL);
        hv_vm_cpu_qos(); /* this thread runs CPU 0: its own signal stack, like the others */
    }
    int reason = gunyah ? hv_gunyah_run() : hv_vm_run_cpu0();
    hv_vm_flush_disks();
    fprintf(stderr, "hengevm: stopped: %s\n", hv_vm_reason_name(reason));
    if (install_dir) {
        if (install_done != 0) return 1;
        fprintf(stderr, "henge: macOS is installed. Start it with: hengevm-android --mac %s\n", install_dir);
        return 0;
    }
    return reason == 0 ? 0 : 1;
}
