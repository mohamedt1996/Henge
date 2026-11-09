/* SPDX-License-Identifier: GPL-2.0-or-later */
/* macOS's shaders, translated on the tablet. Each function of a .metallib (AIR bitcode) goes through
   air2spv (android/air2spv, shipped as libair2spv.so) once; the SPIR-V, its layout and constant data are
   kept by bitcode hash in the cache folder, so the next boot finds them ready.

   Libraries are translated in the background as they arrive, at low priority. A draw that needs a
   function not yet done (shaders_get) puts it at the front of the queue and waits for it. */
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

typedef struct Job { struct Job *next; uint64_t hash; char name[96]; uint32_t len; uint8_t *bc; } Job;

struct Shaders {
    char tool[1024], dir[1024];
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake, finished;
    Job *head, *tail;
    int stop;
    uint64_t done, refused, pending;
    uint64_t seen[8192];
    char **env;
};

uint64_t shaders_hash(const void *d, size_t n) {
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

static void path_of(Shaders *s, uint64_t h, const char *ext, char *out, size_t n) {
    snprintf(out, n, "%s/%016llx.%s", s->dir, (unsigned long long)h, ext);
}

/* Done (1), refused (-1) or not yet (0). */
static int state_of(Shaders *s, uint64_t h) {
    char p[1100];
    path_of(s, h, "layout", p, sizeof p);
    if (exists(p)) return 1;
    path_of(s, h, "err", p, sizeof p);
    return exists(p) ? -1 : 0;
}

static void translate(Shaders *s, Job *j) {
    char bc[1100], spv[1100], json[1100], layout[1100], err[1100], tmp[1200], log[1100];
    path_of(s, j->hash, "bc", bc, sizeof bc);
    path_of(s, j->hash, "spv", spv, sizeof spv);
    path_of(s, j->hash, "json", json, sizeof json);
    path_of(s, j->hash, "layout", layout, sizeof layout);
    path_of(s, j->hash, "err", err, sizeof err);
    path_of(s, j->hash, "log", log, sizeof log); /* air2spv's messages; it becomes .err if it refuses */
    snprintf(tmp, sizeof tmp, "%s.tmp", layout);
    int st = state_of(s, j->hash);
    if (st) { st > 0 ? s->done++ : s->refused++; return; }
    int fd = open(bc, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t w = write(fd, j->bc, j->len);
    close(fd);
    if (w != (ssize_t)j->len) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 2, log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    /* The layout is written last and renamed into place: its presence says the rest is there. */
    char *argv[] = {s->tool, "--json", json, "--layout", tmp, "-o", spv, bc, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, s->tool, &fa, NULL, argv, s->env);
    posix_spawn_file_actions_destroy(&fa);
    int status = -1;
    if (rc == 0) waitpid(pid, &status, 0);
    else rlog("air2spv %s: %s", s->tool, strerror(rc));
    if (rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && rename(tmp, layout) == 0) {
        unlink(log);
        s->done++;
    } else {
        unlink(spv), unlink(tmp);
        rename(log, err);
        s->refused++;
        if (s->refused <= 20) {
            char msg[200] = "";
            FILE *f = fopen(err, "r");
            if (f) {
                char line[200];
                while (fgets(line, sizeof line, f)) memcpy(msg, line, sizeof msg); /* the last line says why */
                fclose(f);
            }
            rlog("shader %s refused: %s", j->name, msg);
        }
        if (!exists(err)) close(open(err, O_WRONLY | O_CREAT, 0600)); /* refused, even with no message */
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
        pthread_mutex_lock(&s->lock);
        s->pending--;
        pthread_cond_broadcast(&s->finished);
        pthread_mutex_unlock(&s->lock);
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
    pthread_cond_init(&s->finished, NULL);
    if (pthread_create(&s->thread, NULL, work, s) != 0) { free(s->env); free(s); return NULL; }
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

/* Lock held. */
static void enqueue_locked(Shaders *s, const char *name, const uint8_t *bc, uint32_t len, uint64_t h, int front) {
    Job *j = calloc(1, sizeof *j);
    if (!j || !(j->bc = malloc(len))) { free(j); return; }
    memcpy(j->bc, bc, len);
    j->len = len, j->hash = h;
    snprintf(j->name, sizeof j->name, "%s", name);
    s->pending++;
    if (front) {
        j->next = s->head, s->head = j;
        if (!s->tail) s->tail = j;
    } else {
        if (s->tail) s->tail->next = j; else s->head = j;
        s->tail = j;
    }
    pthread_cond_signal(&s->wake);
}

static void enqueue(Shaders *s, const char *name, const uint8_t *bc, uint32_t len) {
    uint64_t h = shaders_hash(bc, len);
    pthread_mutex_lock(&s->lock);
    if (seen_add(s, h)) enqueue_locked(s, name, bc, len, h, 0);
    pthread_mutex_unlock(&s->lock);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = sz >= 0 ? malloc((size_t)sz + 1) : NULL;
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    if (d) d[sz] = 0, *n = (size_t)sz;
    return d;
}

int shaders_get(Shaders *s, const uint8_t *bc, uint32_t len, uint8_t **spv, size_t *spv_len, char **layout, uint8_t **cdata, size_t *clen) {
    if (!s) return -1;
    uint64_t h = shaders_hash(bc, len);
    pthread_mutex_lock(&s->lock);
    int st = state_of(s, h);
    if (!st) {
        /* Not done: to the front of the queue (it may already be in it, behind others; doing it twice is harmless). */
        seen_add(s, h);
        enqueue_locked(s, "draw", bc, len, h, 1);
        while (!(st = state_of(s, h)) && !s->stop) pthread_cond_wait(&s->finished, &s->lock);
    }
    pthread_mutex_unlock(&s->lock);
    if (st < 0) return -1;
    char p[1100];
    size_t ln = 0;
    path_of(s, h, "spv", p, sizeof p);
    *spv = slurp(p, spv_len);
    path_of(s, h, "layout", p, sizeof p);
    *layout = (char *)slurp(p, &ln);
    path_of(s, h, "spv.const", p, sizeof p);
    *cdata = slurp(p, clen);
    if (!*cdata) *clen = 0;
    if (!*spv || !*layout) { free(*spv); free(*layout); free(*cdata); return -1; }
    return 0;
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The function list of a .metallib (as android/tools/metallib_inventory.py reads it): each entry is a
   group of {tag, u16 length, value} fields; NAME, TYPE (0 vertex, 1 fragment, 2 kernel), OFFT (third u64:
   bitcode offset) and MDSZ (size). Calls fn for each; stops when it returns nonzero. */
static void each_function(const uint8_t *d, uint32_t n, int (*fn)(void *, const char *, int, const uint8_t *, uint32_t), void *ctx) {
    if (n < 88 || memcmp(d, "MTLB", 4)) return;
    uint64_t fl = rd64(d + 24), bc_off = rd64(d + 72), bc_size = rd64(d + 80);
    if (fl + 4 > n || bc_off > n || bc_size > n - bc_off) return;
    uint32_t count = rd32(d + fl);
    uint64_t at = fl + 4;
    for (uint32_t i = 0; i < count && at + 4 <= n; i++) {
        uint32_t group = rd32(d + at);
        uint64_t end = at + group, t = at + 4;
        if (group < 4 || end > n) return;
        char name[96] = "?";
        int type = -1;
        uint64_t offt = UINT64_MAX, size = 0;
        while (t + 6 <= end) {
            if (!memcmp(d + t, "ENDT", 4)) break;
            uint16_t len;
            memcpy(&len, d + t + 4, 2);
            const uint8_t *v = d + t + 6;
            if (t + 6 + len > end) break;
            if (!memcmp(d + t, "NAME", 4)) snprintf(name, sizeof name, "%.*s", (int)strnlen((const char *)v, len), (const char *)v);
            else if (!memcmp(d + t, "TYPE", 4) && len >= 1) type = v[0] <= 2 ? v[0] : -1;
            else if (!memcmp(d + t, "OFFT", 4) && len >= 24) offt = rd64(v + 16);
            else if (!memcmp(d + t, "MDSZ", 4) && len >= 8) size = rd64(v);
            t += 6 + len;
        }
        if (offt != UINT64_MAX && size && offt + size <= bc_size && fn(ctx, name, type, d + bc_off + offt, (uint32_t)size)) return;
        at = end;
    }
}

typedef struct { const uint8_t *bc; uint32_t len; int stage; char *name; } First;

static int take_first(void *c, const char *name, int type, const uint8_t *bc, uint32_t len) {
    First *f = c;
    f->bc = bc, f->len = len, f->stage = type;
    snprintf(f->name, 96, "%s", name);
    return 1;
}

int metallib_first(const uint8_t *lib, uint32_t n, const uint8_t **bc, uint32_t *len, int *stage, char name[96]) {
    First f = {NULL, 0, -1, name};
    each_function(lib, n, take_first, &f);
    if (!f.bc) return -1;
    *bc = f.bc, *len = f.len, *stage = f.stage;
    return 0;
}

static int take_all(void *c, const char *name, int type, const uint8_t *bc, uint32_t len) {
    (void)type;
    enqueue(c, name, bc, len);
    return 0;
}

void shaders_library(Shaders *s, const uint8_t *d, uint32_t n) {
    if (s) each_function(d, n, take_all, s);
}

void shaders_stats(Shaders *s, uint64_t out[3]) {
    out[0] = s ? s->done : 0, out[1] = s ? s->refused : 0, out[2] = s ? s->pending : 0;
}
