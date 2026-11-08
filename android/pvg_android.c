/* Android's side of the paravirtual GPU. pvg_lite.c speaks the guest driver's protocol as on iOS; on
   iOS pvg_metal.m then replays the guest's Metal calls on the host's Metal. Android has no Metal, and
   this engine (static glibc) cannot load Android's Vulkan driver (bionic), so drawing has to happen in
   the app's own process, from Vulkan, with macOS's shaders translated to SPIR-V.

   This file is step one of that: with HV_SHADER_CAPTURE=DIR, it walks every exec the guest sends and
   saves each shader library (.metallib) and serialized pipeline it uses, once, named by a hash of its
   bytes, with an index (DIR/index.tsv) of how often each pipeline is bound. That is the inventory a
   translation table is built from (android/tools/metallib_inventory.py). Commands still complete, so
   macOS runs as without a GPU backend. Without the variable this is the no-renderer stub it was. */
#include "pvg_lite.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { OBJ_FUNCTION = 6, OBJ_SERIALIZED = 7, MAX_TASKS = 512, MAX_REFS = 8192, SEEN_SLOTS = 1 << 14 };

typedef struct Job { struct Job *next; uint32_t ch, len; uint32_t *cmd; } Job;

struct HvPvgMetal {
    HvPvgLite *lite;
    char dir[900];
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    Job *head, *tail;
    /* What has been saved, by content hash; and per (task, ref) entry, so each object is read once. */
    uint64_t saved[SEEN_SLOTS];
    uint64_t looked[SEEN_SLOTS];
    struct { uint64_t hash; uint64_t binds; } pipes[2048];
    unsigned npipes;
    uint64_t execs, libs, pipelines;
};

static uint64_t fnv(const void *d, size_t n) {
    const uint8_t *p = d;
    uint64_t h = 1469598103934665603ull;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h ? h : 1;
}

/* An open-addressed set; returns 1 if newly added. */
static int set_add(uint64_t *set, uint64_t key) {
    for (uint32_t i = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 50), n = 0; n < SEEN_SLOTS; i = (i + 1) & (SEEN_SLOTS - 1), n++) {
        if (set[i] == key) return 0;
        if (!set[i]) { set[i] = key; return 1; }
    }
    return 0; /* full: stop saving rather than loop */
}

static void save(HvPvgMetal *m, const char *kind, const char *ext, const void *d, size_t n, uint64_t *hash_out) {
    uint64_t h = fnv(d, n);
    if (hash_out) *hash_out = h;
    if (!set_add(m->saved, h)) return;
    char path[1000];
    snprintf(path, sizeof path, "%s/%s-%016llx.%s", m->dir, kind, (unsigned long long)h, ext);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(d, 1, n, f);
    fclose(f);
    if (!strcmp(kind, "lib")) m->libs++; else m->pipelines++;
}

static void note_bind(HvPvgMetal *m, uint64_t h) {
    for (unsigned i = 0; i < m->npipes; i++)
        if (m->pipes[i].hash == h) { m->pipes[i].binds++; return; }
    if (m->npipes < sizeof m->pipes / sizeof *m->pipes) m->pipes[m->npipes].hash = h, m->pipes[m->npipes++].binds = 1;
}

static void write_index(HvPvgMetal *m) {
    char path[1000], tmp[1010];
    snprintf(path, sizeof path, "%s/index.tsv", m->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# execs %llu libraries %llu pipelines %llu\n# pipeline\tbinds\n", (unsigned long long)m->execs,
            (unsigned long long)m->libs, (unsigned long long)m->pipelines);
    for (unsigned i = 0; i < m->npipes; i++)
        fprintf(f, "pipe-%016llx\t%llu\n", (unsigned long long)m->pipes[i].hash, (unsigned long long)m->pipes[i].binds);
    fclose(f);
    rename(tmp, path);
}

/* An object's entry: {type | size << 8, offset lo, offset hi}. */
static int entry(HvPvgMetal *m, uint32_t task, uint32_t ref, uint32_t e[3]) {
    uint64_t list = hv_pvg_lite_task_objects(m->lite, task);
    return list && ref && ref < MAX_REFS && hv_pvg_lite_task_read(m->lite, task, list + (uint64_t)ref * 12, e, 12) == 0 ? 0 : -1;
}

/* A shader library: {u64 offset, u32 length, tag, size} pointing at the .metallib bytes. */
static void capture_function(HvPvgMetal *m, uint32_t task, uint64_t at) {
    struct __attribute__((packed)) { uint64_t off; uint32_t len, tag, size; } h;
    if (hv_pvg_lite_task_read(m->lite, task, at, &h, sizeof h) || !h.len || h.len > (64u << 20)) return;
    uint8_t *lib = malloc(h.len);
    if (lib && hv_pvg_lite_task_read(m->lite, task, h.off, lib, h.len) == 0) save(m, "lib", "metallib", lib, h.len, NULL);
    free(lib);
}

/* A serialized pipeline: {tag, size, ref, length, data}; tags 0xe render, 0x33 tile, 0xb compute. */
static void capture_serialized(HvPvgMetal *m, uint32_t task, uint64_t at, uint32_t size, uint64_t *hash) {
    if (size < 16 || size > (16u << 20)) return;
    uint32_t *all = malloc(size);
    if (all && hv_pvg_lite_task_read(m->lite, task, at, all, size) == 0 && (all[0] == 0xe || all[0] == 0x33 || all[0] == 0xb)) {
        char kind[16];
        snprintf(kind, sizeof kind, "pipe%x", all[0]);
        save(m, "pipe", kind, all, size, hash);
    }
    free(all);
}

static void capture_object(HvPvgMetal *m, uint32_t task, uint32_t ref, uint64_t *pipe_hash) {
    uint32_t e[3];
    if (entry(m, task, ref, e)) return;
    uint32_t type = e[0] & 0xff, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    uint64_t key = fnv(e, 12) ^ ((uint64_t)task << 48 | (uint64_t)ref << 32);
    if (type == OBJ_SERIALIZED) {
        /* Bound often: hash the bytes each time only when the entry is new; remember its hash otherwise. */
        static struct { uint64_t key, hash; } last[4096];
        uint32_t slot = (uint32_t)(key % 4096);
        if (last[slot].key == key) { if (pipe_hash) *pipe_hash = last[slot].hash; return; }
        uint64_t h = 0;
        capture_serialized(m, task, at, size, &h);
        last[slot].key = key, last[slot].hash = h;
        if (pipe_hash) *pipe_hash = h;
    } else if (type == OBJ_FUNCTION && set_add(m->looked, key)) {
        capture_function(m, task, at);
    }
}

/* Functions are named from inside serialized pipelines, so also sweep the task's object list for them. */
static void sweep(HvPvgMetal *m, uint32_t task) {
    static uint32_t swept[MAX_TASKS];
    if (task >= MAX_TASKS) return;
    uint32_t e[3];
    for (uint32_t ref = swept[task] ? swept[task] : 1; ref < MAX_REFS; ref++) {
        if (entry(m, task, ref, e)) break;
        if ((e[0] & 0xff) == OBJ_FUNCTION) capture_object(m, task, ref, NULL);
        swept[task] = ref;
    }
}

/* One command buffer: segments of {size, type | cont << 8} holding {tag, size} records. */
static void walk_buffer(HvPvgMetal *m, uint32_t task, uint64_t off, uint64_t len) {
    uint64_t at = off, end = off + len;
    while (at + 8 <= end) {
        uint32_t hdr[2];
        if (hv_pvg_lite_task_read(m->lite, task, at, hdr, 8)) return;
        at += 8;
        uint32_t type = hdr[1] & 0xff;
        if (type == 5) { /* protection options, then the real header */
            at += 8;
            if (hv_pvg_lite_task_read(m->lite, task, at, hdr, 8)) return;
            at += 8;
            type = hdr[1] & 0xff;
        }
        if (type > 5 || !hdr[0]) return;
        uint64_t seg_end = at + hdr[0] > end ? end : at + hdr[0];
        while (at + 8 <= seg_end) {
            uint32_t r[2];
            if (hv_pvg_lite_task_read(m->lite, task, at, r, 8) || r[1] < 8 || at + r[1] > seg_end) return;
            uint32_t a[2] = {0};
            if (r[1] >= 16) hv_pvg_lite_task_read(m->lite, task, at + 8, a, 8);
            uint64_t h = 0;
            if (type == 0 && r[0] == 116) capture_object(m, task, a[0], &h);      /* render: set pipeline */
            else if (type == 1 && r[0] == 208) capture_object(m, task, a[0], &h); /* compute: set pipeline */
            if (h) note_bind(m, h);
            at += r[1];
        }
        at = seg_end;
    }
}

static void exec(HvPvgMetal *m, const uint32_t *args, uint32_t n) {
    uint32_t task = args[0], resources = args[1], buffers = args[2];
    if (buffers > 64 || 12ull + resources * 8ull + buffers * 16ull > n) return;
    const uint8_t *list = (const uint8_t *)args + 12 + resources * 8;
    for (uint32_t i = 0; i < buffers; i++) {
        uint64_t b[2];
        memcpy(b, list + i * 16, 16);
        walk_buffer(m, task, b[0], b[1]);
    }
    if (++m->execs % 256 == 1) {
        sweep(m, task);
        write_index(m);
        if (m->execs % 4096 == 1)
            fprintf(stderr, "hengevm: shader capture: %llu libraries, %llu pipelines, %llu execs\n", (unsigned long long)m->libs,
                    (unsigned long long)m->pipelines, (unsigned long long)m->execs);
    }
}

static void *work(void *arg) {
    HvPvgMetal *m = arg;
    for (;;) {
        pthread_mutex_lock(&m->lock);
        while (!m->head) pthread_cond_wait(&m->wake, &m->lock);
        Job *j = m->head;
        m->head = j->next;
        if (!m->head) m->tail = NULL;
        pthread_mutex_unlock(&m->lock);
        uint32_t op = j->cmd[0] & 0xffff, skip = (j->cmd[0] >> 16) * 8;
        uint32_t n = j->len > 12 + skip ? j->len - 12 - skip : 0;
        const uint32_t *args = (const uint32_t *)((const uint8_t *)j->cmd + 12 + skip);
        if ((op == 0x43 || op == 0x45) && n >= 12) exec(m, args, n);
        else hv_pvg_lite_command(m->lite, j->cmd, j->len); /* the display: page, acks, swaps */
        hv_pvg_lite_complete(m->lite, j->ch, j->cmd[2]);
        free(j->cmd);
        free(j);
    }
    return NULL;
}

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    const char *dir = getenv("HV_SHADER_CAPTURE");
    if (!dir || !*dir) return NULL;
    HvPvgMetal *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->lite = lite;
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    if (mkdir(m->dir, 0755) != 0 && errno != EEXIST) { free(m); return NULL; }
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->wake, NULL);
    if (pthread_create(&m->worker, NULL, work, m) != 0) { free(m); return NULL; }
    fprintf(stderr, "hengevm: shader capture into %s\n", m->dir);
    return m;
}

/* In order, off the device lock: completing a stamp raises the guest's interrupt. */
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *l, uint32_t ch, uint32_t *cmd, uint32_t len) {
    (void)l;
    Job *j = malloc(sizeof *j);
    if (!j) { free(cmd); return; }
    *j = (Job){NULL, ch, len, cmd};
    pthread_mutex_lock(&m->lock);
    if (m->tail) m->tail->next = j; else m->head = j;
    m->tail = j;
    pthread_cond_signal(&m->wake);
    pthread_mutex_unlock(&m->lock);
}

int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t w, uint32_t h) { (void)m; (void)bgra; (void)w; (void)h; return -1; }
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **px, uint32_t *w, uint32_t *h) { (void)m; (void)px; (void)w; (void)h; return -1; }
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) { (void)m; (void)mapping; }
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) { (void)m; (void)layer; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { (void)m; return 0; }
