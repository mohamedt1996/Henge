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
#include "gpu/link.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <libgen.h>

/* Gunyah runs the engine as root (su): a file it makes for the app (gpu.link, the RAM file) would be root's and
   carry su's SELinux label until the engine exits. Hand it to the app now: its uid, and its folder's label. */
void hv_android_share_with_app(const char *path) {
    const char *u = getenv("HENGE_APP_UID");
    if (!u || !*u || geteuid() != 0 || !path || !*path) return;
    uid_t uid = (uid_t)strtoul(u, NULL, 10);
    if (chown(path, uid, uid) != 0) fprintf(stderr, "hengevm: giving %s to the app: %s\n", path, strerror(errno));
    char dir[1100], label[256];
    snprintf(dir, sizeof dir, "%s", path);
    ssize_t n = getxattr(dirname(dir), "security.selinux", label, sizeof label);
    if (n > 0 && setxattr(path, "security.selinux", label, (size_t)n, 0) != 0)
        fprintf(stderr, "hengevm: labelling %s for the app: %s\n", path, strerror(errno));
}

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
    /* HV_GPU_LINK: commands go to the app's Vulkan renderer (android/gpu/link.h) instead. */
    HvLinkHeader *link;
    uint64_t seq;
    int linked; /* the app was attached at the last check */
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

/* ---------------------------------------------------------------- the link to the app -- */

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void pause_us(long us) {
    struct timespec t = {0, us * 1000};
    nanosleep(&t, NULL);
}

/* The app has looked at the link recently. CLOCK_MONOTONIC is the same clock in both processes. */
static int app_attached(HvPvgMetal *m) {
    uint64_t beat = __atomic_load_n(&m->link->heartbeat, __ATOMIC_ACQUIRE);
    int on = beat && now_ns() - beat < HV_LINK_STALE_NS;
    if (on != m->linked) {
        m->linked = on;
        fprintf(stderr, "hengevm: gpu link: renderer %s\n", on ? "attached" : "gone; completing commands without it");
    }
    return on;
}

static void link_publish(HvPvgMetal *m) {
    HvLinkHeader *h = m->link;
    for (uint32_t t = 0; t < HV_LINK_TASKS; t++) {
        h->task_root[t] = hv_pvg_lite_task_root(m->lite, t);
        h->task_objects[t] = hv_pvg_lite_task_objects(m->lite, t);
    }
    h->surface_table = hv_pvg_lite_surface_table(m->lite);
    h->display_page = hv_pvg_lite_display_pa(m->lite);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/* Puts the command in the ring; returns its seq, or 0 if there was no room (at once when nowait, else once
   the app went away or the wait budget ran out). */
static uint64_t wait_budget_ns(void);
static uint64_t link_send(HvPvgMetal *m, uint32_t ch, const uint32_t *cmd, uint32_t len, int nowait) {
    HvLinkHeader *h = m->link;
    uint32_t bytes = (uint32_t)((sizeof(HvLinkRecord) + len + 7) & ~7u);
    if (bytes > HV_LINK_RING_BYTES / 2) return 0;
    uint64_t head = h->head, until = now_ns() + wait_budget_ns();
    for (;;) {
        uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
        uint32_t at = (uint32_t)(head % HV_LINK_RING_BYTES), room = HV_LINK_RING_BYTES - at;
        uint64_t need = bytes + (room < bytes ? room : 0); /* a wrap wastes the rest of the ring */
        if (HV_LINK_RING_BYTES - (head - tail) >= need) break;
        if (nowait || !app_attached(m) || now_ns() > until) return 0;
        pause_us(100);
    }
    uint8_t *ring = hv_link_ring(h);
    uint32_t at = (uint32_t)(head % HV_LINK_RING_BYTES), room = HV_LINK_RING_BYTES - at;
    if (room < bytes) {
        if (room >= sizeof(HvLinkRecord)) *(HvLinkRecord *)(ring + at) = (HvLinkRecord){room, HV_REC_WRAP, 0, 0, 0};
        head += room, at = 0;
    }
    HvLinkRecord *r = (HvLinkRecord *)(ring + at);
    *r = (HvLinkRecord){bytes, HV_REC_COMMAND, ch, len, ++m->seq};
    memcpy(r + 1, cmd, len);
    __atomic_store_n(&h->head, head + bytes, __ATOMIC_RELEASE);
    return m->seq;
}

/* How long one command may hold its stamp for the app. macOS's GPU driver treats a stamp that does not
   move for a few seconds as a hung GPU and panics (IOGPUFamily); the renderer can be that slow (first
   draws translate shaders, a stuck Vulkan fence), so the guest is never made to wait longer than this. */
static uint64_t wait_budget_ns(void) {
    static uint64_t ns;
    if (!ns) {
        const char *e = getenv("HV_GPU_WAIT_MS");
        long ms = e ? strtol(e, NULL, 10) : 250;
        ns = (uint64_t)(ms > 0 ? ms : 1) * 1000000ull;
    }
    return ns;
}

/* Returns 1 if the app finished seq in time. */
static int link_wait(HvPvgMetal *m, uint64_t seq) {
    uint64_t until = now_ns() + wait_budget_ns();
    for (unsigned spins = 0; __atomic_load_n(&m->link->done, __ATOMIC_ACQUIRE) < seq; spins++) {
        if ((spins & 63) == 0 && (!app_attached(m) || now_ns() > until)) return 0;
        pause_us(spins < 200 ? 20 : 200);
    }
    return 1;
}

static void *link_work(void *arg) {
    HvPvgMetal *m = arg;
    uint64_t behind_since = 0, late = 0, dropped = 0;
    for (;;) {
        pthread_mutex_lock(&m->lock);
        while (!m->head) pthread_cond_wait(&m->wake, &m->lock);
        Job *j = m->head;
        m->head = j->next;
        if (!m->head) m->tail = NULL;
        pthread_mutex_unlock(&m->lock);
        uint32_t op = j->cmd[0] & 0xffff;
        /* The display's own state (mode page, acks, swap counters) stays the engine's. */
        if (op != 0x43 && op != 0x45) hv_pvg_lite_command(m->lite, j->cmd, j->len);
        if (app_attached(m)) {
            link_publish(m);
            /* While the app is behind, commands still go to it (it catches up in order) but the guest's
               stamps no longer wait for it; the ring must not block either. */
            int behind = behind_since && __atomic_load_n(&m->link->done, __ATOMIC_ACQUIRE) < m->seq;
            uint64_t seq = link_send(m, j->ch, j->cmd, j->len, behind);
            if (!seq) {
                if (dropped++ % 1000 == 0)
                    fprintf(stderr, "hengevm: gpu link: ring full while the renderer is behind; %llu commands not drawn\n",
                            (unsigned long long)dropped);
            } else if (!behind && !link_wait(m, seq) && m->linked) {
                behind_since = now_ns();
                if (late++ % 100 == 0)
                    fprintf(stderr, "hengevm: gpu link: renderer over %llu ms on a command (%llu times); not holding macOS for it\n",
                            (unsigned long long)(wait_budget_ns() / 1000000), (unsigned long long)late);
            } else if (behind_since && !behind) {
                behind_since = 0;
            }
        }
        hv_pvg_lite_complete(m->lite, j->ch, j->cmd[2]);
        free(j->cmd);
        free(j);
    }
    return NULL;
}

static HvLinkHeader *link_open(HvPvgLite *lite, const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd >= 0) hv_android_share_with_app(path);
    if (fd < 0 || ftruncate(fd, HV_LINK_FILE_BYTES) != 0) {
        fprintf(stderr, "hengevm: gpu link %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return NULL;
    }
    HvLinkHeader *h = mmap(NULL, HV_LINK_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (h == MAP_FAILED) return NULL;
    memset(h, 0, sizeof *h);
    h->version = HV_LINK_VERSION;
    h->header_bytes = HV_LINK_HEADER_BYTES, h->ring_bytes = HV_LINK_RING_BYTES;
    if (hv_pvg_lite_ram(lite, &h->ram_pa, &h->ram_size) != 0) fprintf(stderr, "hengevm: gpu link: no guest RAM region\n");
    const char *ram = getenv("HV_RAM_FILE");
    snprintf(h->ram_file, sizeof h->ram_file, "%s", ram ? ram : "");
    hv_pvg_lite_mode(&h->display_w, &h->display_h);
    h->engine_alive = 1;
    __atomic_store_n(&h->magic, HV_LINK_MAGIC, __ATOMIC_RELEASE); /* last: the app checks it first */
    return h;
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
    const char *linkpath = getenv("HV_GPU_LINK");
    if (linkpath && *linkpath) {
        HvPvgMetal *m = calloc(1, sizeof *m);
        if (!m) return NULL;
        m->lite = lite;
        m->link = link_open(lite, linkpath);
        if (!m->link) { free(m); return NULL; }
        pthread_mutex_init(&m->lock, NULL);
        pthread_cond_init(&m->wake, NULL);
        if (pthread_create(&m->worker, NULL, link_work, m) != 0) { free(m); return NULL; }
        fprintf(stderr, "hengevm: gpu link %s: waiting for the app's renderer\n", linkpath);
        return m;
    }
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
