/* SPDX-License-Identifier: GPL-2.0-or-later */
/* macOS's shaders, translated on the tablet as they arrive. Each function of a .metallib (AIR
   bitcode) goes through air2spv (android/air2spv, shipped as libair2spv.so) once; the SPIR-V and
   its sidecar are kept by bitcode hash in the cache folder, so the next boot finds them ready.
   Translation runs on its own thread at low priority: macOS never waits for it. */
#include "replay.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct Job { struct Job *next; uint64_t hash; char name[128]; uint32_t len; uint8_t *bc; } Job;

struct Shaders {
    char tool[1024], dir[1024];
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    Job *head, *tail;
    int stop;
    uint64_t done, refused, pending;
    uint64_t seen[8192];
    char **env;
};

static uint64_t fnv(const void *d, size_t n) {
    const uint8_t *p = d;
    uint64_t h = 1469598103934665603ull;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h ? h : 1;
}

static int seen_add(Shaders *s, uint64_t h) {
    for (uint32_t i = (uint32_t)(h % 8192), n = 0; n < 8192; i = (i + 1) % 8192, n++) {
        if (s->seen[i] == h) return 0;
        if (!s->seen[i]) { s->seen[i] = h; return 1; }
    }
    return 0;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void translate(Shaders *s, Job *j) {
    char bc[1100], spv[1100], json[1100], err[1100];
    snprintf(bc, sizeof bc, "%s/%016llx.bc", s->dir, (unsigned long long)j->hash);
    snprintf(spv, sizeof spv, "%s/%016llx.spv", s->dir, (unsigned long long)j->hash);
    snprintf(json, sizeof json, "%s/%016llx.json", s->dir, (unsigned long long)j->hash);
    snprintf(err, sizeof err, "%s/%016llx.err", s->dir, (unsigned long long)j->hash);
    if (exists(spv)) { s->done++; return; }
    if (exists(err)) { s->refused++; return; }
    int fd = open(bc, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t w = write(fd, j->bc, j->len);
    close(fd);
    if (w != (ssize_t)j->len) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 2, err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    char *argv[] = {s->tool, "--json", json, "-o", spv, bc, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, s->tool, &fa, NULL, argv, s->env);
    posix_spawn_file_actions_destroy(&fa);
    int status = -1;
    if (rc == 0) waitpid(pid, &status, 0);
    else rlog("air2spv %s: %s", s->tool, strerror(rc));
    if (rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        unlink(err);
        s->done++;
    } else {
        unlink(spv);
        s->refused++;
        if (s->refused <= 20) {
            char msg[200] = "";
            FILE *f = fopen(err, "r");
            if (f) { if (!fgets(msg, sizeof msg, f)) msg[0] = 0; fclose(f); }
            rlog("shader %s refused: %s", j->name, msg);
        }
    }
    unlink(bc);
}

static void *work(void *arg) {
    Shaders *s = arg;
    setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (!s->head && !s->stop) pthread_cond_wait(&s->wake, &s->lock);
        if (s->stop) { pthread_mutex_unlock(&s->lock); return NULL; }
        Job *j = s->head;
        s->head = j->next;
        if (!s->head) s->tail = NULL;
        pthread_mutex_unlock(&s->lock);
        translate(s, j);
        __atomic_fetch_sub(&s->pending, 1, __ATOMIC_RELAXED);
        free(j->bc);
        free(j);
        if ((s->done + s->refused) % 50 == 0) rlog("shaders: %llu translated, %llu refused, %llu waiting", (unsigned long long)s->done,
                                                   (unsigned long long)s->refused, (unsigned long long)s->pending);
    }
}

Shaders *shaders_create(const char *tool, const char *dir) {
    if (!tool || !dir || access(tool, X_OK) != 0) { rlog("shader translation off: no air2spv at %s", tool ? tool : "(none)"); return NULL; }
    Shaders *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->tool, sizeof s->tool, "%s", tool);
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    mkdir(dir, 0700);
    /* air2spv is a static glibc program, like the engine: Android's seccomp filter kills glibc's rseq
       registration, so it is turned off for it as VmService does for the engine. */
    size_t n = 0;
    while (environ && environ[n]) n++;
    s->env = calloc(n + 2, sizeof *s->env);
    if (!s->env) { free(s); return NULL; }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "GLIBC_TUNABLES=", 15)) s->env[k++] = environ[i];
    s->env[k++] = "GLIBC_TUNABLES=glibc.pthread.rseq=0";
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    if (pthread_create(&s->thread, NULL, work, s) != 0) { free(s); return NULL; }
    return s;
}

void shaders_destroy(Shaders *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->stop = 1;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread, NULL);
    for (Job *j = s->head; j;) { Job *n = j->next; free(j->bc); free(j); j = n; }
    free(s->env);
    free(s);
}

static void enqueue(Shaders *s, const char *name, const uint8_t *bc, uint32_t len) {
    uint64_t h = fnv(bc, len);
    if (!seen_add(s, h)) return;
    Job *j = calloc(1, sizeof *j);
    if (!j || !(j->bc = malloc(len))) { free(j); return; }
    memcpy(j->bc, bc, len);
    j->len = len, j->hash = h;
    snprintf(j->name, sizeof j->name, "%s", name);
    __atomic_fetch_add(&s->pending, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&s->lock);
    if (s->tail) s->tail->next = j; else s->head = j;
    s->tail = j;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The function list of a .metallib (as android/tools/metallib_inventory.py reads it): each entry is a
   group of {tag, u16 length, value} fields; NAME, OFFT (third u64: bitcode offset) and MDSZ (size). */
void shaders_library(Shaders *s, const uint8_t *d, uint32_t n) {
    if (!s || n < 88 || memcmp(d, "MTLB", 4)) return;
    uint64_t fl = rd64(d + 24), bc_off = rd64(d + 72), bc_size = rd64(d + 80);
    if (fl + 4 > n || bc_off > n || bc_size > n - bc_off) return;
    uint32_t count = rd32(d + fl);
    uint64_t at = fl + 4;
    for (uint32_t i = 0; i < count && at + 4 <= n; i++) {
        uint32_t group = rd32(d + at);
        uint64_t end = at + group, t = at + 4;
        if (group < 4 || end > n) return;
        char name[128] = "?";
        uint64_t offt = UINT64_MAX, size = 0;
        while (t + 6 <= end) {
            if (!memcmp(d + t, "ENDT", 4)) break;
            uint16_t len;
            memcpy(&len, d + t + 4, 2);
            const uint8_t *v = d + t + 6;
            if (t + 6 + len > end) break;
            if (!memcmp(d + t, "NAME", 4)) snprintf(name, sizeof name, "%.*s", (int)strnlen((const char *)v, len), (const char *)v);
            else if (!memcmp(d + t, "OFFT", 4) && len >= 24) offt = rd64(v + 16);
            else if (!memcmp(d + t, "MDSZ", 4) && len >= 8) size = rd64(v);
            t += 6 + len;
        }
        if (offt != UINT64_MAX && size && offt + size <= bc_size) enqueue(s, name, d + bc_off + offt, (uint32_t)size);
        at = end;
    }
}

void shaders_stats(Shaders *s, uint64_t out[3]) {
    out[0] = s ? s->done : 0, out[1] = s ? s->refused : 0, out[2] = s ? s->pending : 0;
}
