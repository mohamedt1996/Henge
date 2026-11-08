/* hengevm-android: boot an installed virtual Mac headless, serial console on stderr.
   The disk, aux and identity come from an install made by Henge on an iPhone or iPad
   (or hengevm on a Mac); copy them to the device and point this at them. */
#include "vm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr,
            "usage: hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage\n"
            "                       --ecid N --seed N [--ram MB (4096)] [--cpus N (2)] [--log 0-3] [--jit-mb MB]\n");
    exit(2);
}

static volatile sig_atomic_t stopping;
static void on_signal(int sig) {
    (void)sig;
    if (stopping++) _exit(130);
    hv_vm_stop();
}

int main(int argc, char **argv) {
    HvVmConfig vc = {.aux_offset = 16384, .ram_mb = 4096, .ncpus = 2, .log = 1, .no_net = 1, .no_sound = 1};
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        const char *a = argv[i], *v = argv[++i];
        if (!strcmp(a, "--rom")) vc.rom = v;
        else if (!strcmp(a, "--disk")) vc.disk = v;
        else if (!strcmp(a, "--aux")) vc.aux = v;
        else if (!strcmp(a, "--aux-offset")) vc.aux_offset = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--ecid")) vc.ecid = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--seed")) vc.seed = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--ram")) vc.ram_mb = strtoull(v, NULL, 0);
        else if (!strcmp(a, "--cpus")) vc.ncpus = atoi(v);
        else if (!strcmp(a, "--log")) vc.log = atoi(v);
        else if (!strcmp(a, "--jit-mb")) vc.jit_bytes = (size_t)atoi(v) << 20;
        else usage();
    }
    if (!vc.rom || !vc.disk || !vc.aux) usage();
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Android has no swap to speak of: back guest RAM with a file unless told otherwise. */
    if (!getenv("HV_RAM_FILE")) {
        static char ram[1024];
        snprintf(ram, sizeof ram, "%s.ram", vc.disk);
        setenv("HV_RAM_FILE", ram, 0);
    }
    char err[256];
    if (hv_vm_create(&vc, err, sizeof err) != 0) {
        fprintf(stderr, "hengevm: %s\n", err);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "hengevm: %d CPUs, %llu MB, headless (no GPU, network or sound yet)\n", hv_vm_ncpus(),
            (unsigned long long)vc.ram_mb);
    int reason = hv_vm_run_cpu0();
    hv_vm_flush_disks();
    fprintf(stderr, "hengevm: stopped: %s\n", hv_vm_reason_name(reason));
    return reason == 0 ? 0 : 1;
}
