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
static void curl_global_init_all(void) { curl_global_init(CURL_GLOBAL_ALL); }

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void hv_android_share_with_app(const char *path); /* pvg_android.c */

static void usage(void) {
    fprintf(stderr,
            "usage (both forms also take --engine jit|gunyah): hengevm-android --install FOLDER [--release 15.6.1] [--ipsw FILE] [--disk-gb 64] [--rom FILE] [--cache DIR] [--ram MB] [--cpus N]\n"
            "   or: hengevm-android --mac FOLDER (exported from Henge on iOS) [options]\n"
            "   or: hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage\n"
            "                       --ecid N --seed N [--ram MB (4096)] [--cpus N (2)] [--log 0-3] [--jit-mb MB] [--net 0|1]\n"
            "                       [--vnc PORT (5900, 0 = off)] [--gpu on|off|capture|vulkan|reims] [--shader-dir DIR]\n"
            "  --gpu off      no paravirtual GPU (experiment: macOS may fall back to a plain framebuffer)\n"
            "  --gpu capture  keep the GPU device and save every shader library and pipeline macOS uses\n"
            "                 (default DIR: shaders/ next to the disk), for building the Vulkan translation\n"
            "  --gpu vulkan   hand the GPU's commands to the Android app's Vulkan renderer through gpu.link\n"
            "                 next to the disk (HV_GPU_LINK overrides); guest RAM stays a named file it maps\n"
            "  --gpu reims    the GPU device is reims-vgpu in the Android app, on Vulkan, through gpu.reims next to\n"
            "                 the disk (HV_GPU_REIMS overrides); guest RAM stays a named file it maps\n");
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
static void dump_cpu(int i, HvCpu *c) {
    fprintf(stderr, "henge: stuck? cpu%d EL%u pc %llx sp %llx x29 %llx x30 %llx | elr %llx spsr %llx esr %llx far %llx | "
                    "x0 %llx x1 %llx x2 %llx x3 %llx daif %llx\n",
            i, c->el, (unsigned long long)c->pc, (unsigned long long)c->sp, (unsigned long long)c->x[29],
            (unsigned long long)c->x[30], (unsigned long long)c->elr_el1, (unsigned long long)c->spsr_el1,
            (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->daif);
}
static void *install_heartbeat(void *arg) {
    (void)arg;
    uint64_t last[HV_MAX_CPUS] = {0}, last_pc[HV_MAX_CPUS] = {0};
    uint64_t last_disk = 0, last_gpu = 0;
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
        hv_pvg_lite_activity(b->pvg, &cmds, &swaps);
        fprintf(stderr, " | disk +%llu | gpu +%llu cmds, %llu frames", (unsigned long long)(disk - last_disk),
                (unsigned long long)(cmds - last_gpu), (unsigned long long)swaps);
        if (disk != last_disk || cmds != last_gpu) moved = 1;
        last_disk = disk, last_gpu = cmds;
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
    const char *gpu = "on", *shader_dir = NULL, *engine = "jit";
    const char *install_dir = NULL, *release = "15.6.1", *ipsw = NULL, *cache = NULL;
    uint64_t disk_gb = 64;
    HvVmConfig vc = {.aux_offset = 16384, .ram_mb = 4096, .ncpus = 2, .log = 1, .no_net = 0, .no_sound = 1};
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
        else if (!strcmp(a, "--jit-mb")) vc.jit_bytes = (size_t)atoi(v) << 20;
        else usage();
    }
    static char rom_path[1100], ipsw_path[1100], work[1100];
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
        if (!strcmp(gpu, "vulkan") || !strcmp(gpu, "reims")) {
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
    } else if (strcmp(gpu, "on") && strcmp(gpu, "capture") && strcmp(gpu, "vulkan") && strcmp(gpu, "reims")) {
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
    fprintf(stderr, "hengevm: %d CPUs, %llu MB, network %s, no sound yet\n", hv_vm_ncpus(),
            (unsigned long long)vc.ram_mb, vc.no_net ? "off" : "on (NAT)");
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
