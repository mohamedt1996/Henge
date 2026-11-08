/* hengevm-android: boot an installed virtual Mac headless, serial console on stderr.
   The disk, aux and identity come from an install made by Henge on an iPhone or iPad
   (or hengevm on a Mac); copy them to the device and point this at them. */
#include "vm.h"
#include "vnc.h"
#include "fetch.h"
#include "install.h"
#include <sys/stat.h>
#include <curl/curl.h>
static void curl_global_init_all(void) { curl_global_init(CURL_GLOBAL_ALL); }

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr,
            "usage: hengevm-android --install FOLDER [--release 15.6.1] [--ipsw FILE] [--disk-gb 64] [--rom FILE] [--ram MB] [--cpus N]\n"
            "   or: hengevm-android --mac FOLDER (exported from Henge on iOS) [options]\n"
            "   or: hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage\n"
            "                       --ecid N --seed N [--ram MB (4096)] [--cpus N (2)] [--log 0-3] [--jit-mb MB] [--net 0|1]\n"
            "                       [--vnc PORT (5900, 0 = off)] [--gpu on|off|capture] [--shader-dir DIR]\n"
            "  --gpu off      no paravirtual GPU (experiment: macOS may fall back to a plain framebuffer)\n"
            "  --gpu capture  keep the GPU device and save every shader library and pipeline macOS uses\n"
            "                 (default DIR: shaders/ next to the disk), for building the Vulkan translation\n");
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

static volatile int install_done = -2;
static void on_install(int step, double p, void *user) {
    (void)user;
    static const char *names[] = {"detecting", "preparing", "sending the system", "verifying", "flashing firmware",
                                  "flashing", "updating", "finishing"};
    static int last = -1, lastpct = -1;
    if (step == HV_INSTALL_DONE || step == HV_INSTALL_FAILED) {
        install_done = step == HV_INSTALL_DONE ? 0 : -1;
        fprintf(stderr, "\nhenge: install %s\n", step == HV_INSTALL_DONE ? "finished" : "FAILED");
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
    const char *gpu = "on", *shader_dir = NULL;
    const char *install_dir = NULL, *release = "15.6.1", *ipsw = NULL;
    uint64_t disk_gb = 64;
    HvVmConfig vc = {.aux_offset = 16384, .ram_mb = 4096, .ncpus = 2, .log = 1, .no_net = 0, .no_sound = 1};
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        const char *a = argv[i], *v = argv[++i];
        if (!strcmp(a, "--install")) install_dir = v;
        else if (!strcmp(a, "--release")) release = v;
        else if (!strcmp(a, "--ipsw")) ipsw = v;
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
        } else if (hv_fetch_boot_rom(rom_path) != 0) {
            return 1;
        }
        if (!ipsw) {
            snprintf(ipsw_path, sizeof ipsw_path, "%s/%s.ipsw", install_dir, r->id);
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
    }
    if (!vc.rom || !vc.disk || !vc.aux) usage();
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
    } else if (strcmp(gpu, "on") && strcmp(gpu, "capture")) {
        usage();
    }
    setvbuf(stderr, NULL, _IONBF, 0);
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
    if (vnc_port) {
        if (hv_vnc_start(vnc_port) == 0) fprintf(stderr, "hengevm: display on vnc://127.0.0.1:%d\n", vnc_port);
        else fprintf(stderr, "hengevm: vnc port %d unavailable\n", vnc_port);
    }
    if (install_dir && hv_install_start(hv_vm_board()->vusb, ipsw, work, vc.ecid, on_install, NULL) != 0) {
        fprintf(stderr, "henge: the installer could not start\n");
        return 1;
    }
    if (install_dir) usleep(500000); /* usbmuxd installs its own handlers; take SIGINT and SIGTERM back */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "hengevm: %d CPUs, %llu MB, network %s, no sound yet\n", hv_vm_ncpus(),
            (unsigned long long)vc.ram_mb, vc.no_net ? "off" : "on (NAT)");
    int reason = hv_vm_run_cpu0();
    hv_vm_flush_disks();
    fprintf(stderr, "hengevm: stopped: %s\n", hv_vm_reason_name(reason));
    if (install_dir) {
        if (install_done != 0) return 1;
        fprintf(stderr, "henge: macOS is installed. Start it with: hengevm-android --mac %s\n", install_dir);
        return 0;
    }
    return reason == 0 ? 0 : 1;
}
