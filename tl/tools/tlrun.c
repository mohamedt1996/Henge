/* Runs a macOS arm64 executable through Henge's translation-layer loader in
   this process, in a region shaped like iOS's, and never through dyld:
   tlrun [-v] [-strict] [-soft] [-noproc] [-region MB] [-shim LIB]... EXECUTABLE [ARGS...]
   -soft: missing symbols return 0 (said once) and missing classes and constants get stand-ins; -noproc: the program
   may not start other processes, as on iOS. A watch thread reports any file of the program's app that dyld loads
   behind the loader's back (that would mean the run proves nothing about iOS). */
#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/tl_loader.h"

#ifdef TL_APPKIT_MINI
/* tlrun-mini: the toy AppKit's classes are named as AppKit's, so this build must never meet the real AppKit. */
#include "../shim/appkit_mini.h"

static void report_appkit(void) {
    fprintf(stderr, "tlrun: the real AppKit %s loaded in this process\n", tl_appkit_is_real_loaded() ? "WAS" : "was never");
}
#endif

static const char *g_shimmed[16];
static int g_nshimmed;

/* A library named with -shim has no host library: only the shim answers (none here), as AppKit will on iOS. */
static const char *map_name(const char *name, char *buf, size_t len) {
    (void)buf, (void)len;
    for (int i = 0; i < g_nshimmed; i++)
        if (strstr(name, g_shimmed[i])) return NULL;
    return name;
}

static char g_app_dir[4096];

/* TLRUN_MSGLOG_SECS=N: the Objective-C runtime logs every message for N seconds (to /tmp/msgSends-PID, its own
   fixed place), for listing which AppKit methods an app really uses. */
extern void instrumentObjcMessageSends(int flag);
static void *msglog_off(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    instrumentObjcMessageSends(0);
    fprintf(stderr, "tlrun: message log off\n");
    return NULL;
}

/* TLRUN_POKE="NAME:0xVMADDR=BYTE": once an image whose path ends in NAME is linked, set that byte (a test hook). */
static void poke(TlImage *im) {
    const char *spec = getenv("TLRUN_POKE");
    if (!spec) return;
    const char *colon = strrchr(spec, ':');
    if (!colon) return;
    size_t nl = (size_t)(colon - spec), pl = strlen(tl_image_path(im));
    if (pl < nl || strncmp(tl_image_path(im) + pl - nl, spec, nl)) return;
    unsigned long long at = 0;
    unsigned val = 0;
    if (sscanf(colon + 1, "%llx=%u", &at, &val) != 2) return;
    ((unsigned char *)tl_image_base(im))[at] = (unsigned char)val;
    fprintf(stderr, "tlrun: poked %s+%#llx = %u\n", tl_image_path(im), at, val);
}

/* Anything under the program's app (or beside it) that dyld has loaded did not go through the loader. */
static void *watch_dyld(void *unused) {
    (void)unused;
    static char said[64][1024];
    int nsaid = 0;
    for (;;) {
        for (uint32_t k = 0; k < _dyld_image_count(); k++) {
            const char *n = _dyld_get_image_name(k);
            if (!n || !g_app_dir[0] || strncmp(n, g_app_dir, strlen(g_app_dir))) continue;
            int seen = 0;
            for (int j = 0; j < nsaid; j++) seen |= !strcmp(said[j], n);
            if (seen) continue;
            if (nsaid < 64) snprintf(said[nsaid++], sizeof said[0], "%s", n);
            fprintf(stderr, "tlrun: DYLD LOADED %s (not through the loader)\n", n);
        }
        sleep(2);
    }
    return NULL;
}

int main(int argc, const char **argv, const char **envp) {
    TlHost host;
    size_t region_mb = 1024;
    for (int k = 1; k + 1 < argc && argv[k][0] == '-'; k++)
        if (!strcmp(argv[k], "-region")) region_mb = (size_t)atoi(argv[k + 1]);
    if (tl_host_init_split(&host, region_mb << 20)) {
        fprintf(stderr, "tlrun: no executable region\n");
        return 120;
    }
    host.map_install_name = map_name;
    host.image_linked = poke;
#ifdef TL_APPKIT_MINI
    /* AppKit (and Cocoa, which would load it) answered by the toy AppKit, never the real one. */
    g_shimmed[g_nshimmed++] = "/AppKit.framework/";
    g_shimmed[g_nshimmed++] = "/Cocoa.framework/";
    host.shim_lookup = tl_appkit_mini_lookup;
    atexit(report_appkit);
#endif
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) host.verbose = 1;
        else if (!strcmp(argv[i], "-strict")) host.strict = 1;
        else if (!strcmp(argv[i], "-soft")) host.soft_missing = 1;
        else if (!strcmp(argv[i], "-noproc")) host.no_processes = 1;
        else if (!strcmp(argv[i], "-region") && i + 1 < argc) i++;
        else if (!strcmp(argv[i], "-shim") && i + 1 < argc) g_shimmed[g_nshimmed++] = argv[++i];
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: tlrun [-v] [-strict] [-soft] [-noproc] [-region MB] [-shim LIB]... EXECUTABLE [ARGS...]\n");
        return 121;
    }
    char err[1024] = "";
    {
        char real[4096];
        if (realpath(argv[i], real)) {
            const char *app = strstr(real, ".app/");
            if (app) snprintf(g_app_dir, sizeof g_app_dir, "%.*s", (int)(app - real + 4), real);
            else {
                snprintf(g_app_dir, sizeof g_app_dir, "%s", real);
                char *slash = strrchr(g_app_dir, '/');
                if (slash) slash[1] = 0;
            }
        }
        pthread_t t;
        pthread_create(&t, NULL, watch_dyld, NULL);
        pthread_detach(t);
    }
    TlImage *exe = tl_load(&host, argv[i], err, sizeof err);
    if (!exe) {
        fprintf(stderr, "tlrun: %s\n", err);
        return 122;
    }
    /* Proof that dyld never saw it. */
    const char *path = tl_image_path(exe);
    for (uint32_t k = 0; k < _dyld_image_count(); k++)
        if (!strcmp(_dyld_get_image_name(k), path)) {
            fprintf(stderr, "tlrun: dyld has %s loaded\n", path);
            return 123;
        }
    if (getenv("TLRUN_REPORT"))
        fprintf(stderr, "tlrun: %s at %p, %zu bytes, %d missing symbols; not in dyld's %u images\n", path,
                tl_image_base(exe), tl_image_size(exe), tl_missing_count(), _dyld_image_count());
    for (int k = 0; k < tl_missing_count(); k++) fprintf(stderr, "tlrun: missing %s\n", tl_missing_name(k));
    /* An executable inside an app bundle runs as that app. */
    const char *macos = strstr(path, ".app/Contents/MacOS/");
    if (macos) {
        char app[4096];
        snprintf(app, sizeof app, "%.*s", (int)(macos - path + 4), path);
        if (tl_adopt_main_bundle(app)) fprintf(stderr, "tlrun: %s is not a bundle\n", app);
    }
    if (getenv("TLRUN_MSGLOG_SECS")) {
        pthread_t t;
        instrumentObjcMessageSends(1);
        pthread_create(&t, NULL, msglog_off, (void *)(uintptr_t)atoi(getenv("TLRUN_MSGLOG_SECS")));
    }
    return tl_run_main(exe, argc - i, argv + i, envp);
}
