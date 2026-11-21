/* Henge's Mach-O loader for the Mac-app translation layer. See tl_loader.h. */
#include "tl_loader.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <mach-o/fat.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TL_PAGE 0x4000ull
#define TL_MAX_SEGS 32
#define TL_MAX_DEPS 256
#define TL_MAX_RPATHS 32

typedef struct TlSeg {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int prot;
} TlSeg;

typedef struct TlDep {
    char *install_name;
    int weak, reexport;
    TlImage *tl;        /* a guest image this loader loaded */
    void *handle;       /* or a host library */
    int no_library;     /* neither: only the shim can answer */
} TlDep;

struct TlImage {
    char *path, *dir;
    int fd;
    uint8_t *map;
    size_t maplen;
    const uint8_t *slice; /* the arm64 slice of the file */
    size_t slicelen;
    const struct mach_header_64 *fmh;
    uint8_t *base;        /* where the mach header (start of __TEXT) is mapped */
    int64_t slide;        /* runtime address - vmaddr */
    uint64_t vmmin, vmsize;
    TlSeg segs[TL_MAX_SEGS];
    int nsegs;
    TlDep deps[TL_MAX_DEPS];
    int ndeps;
    char *rpaths[TL_MAX_RPATHS];
    int nrpaths;
    const uint8_t *trie;
    uint32_t trie_size;
    const struct linkedit_data_command *chained;
    const struct dyld_info_command *dyldinfo;
    uint64_t entryoff;
    int has_main;
    int initialized, visiting;
    pthread_key_t tlv_key;
    uint64_t tlv_start, tlv_size, tlv_init_size; /* vmaddrs */
    int ns;          /* the "process" it belongs to: 0 the program, 1.. what it started (processes as threads) */
    TlImage *next;
};

static TlHost *g_host;
uint64_t tl_missing_data(const char *sym);     /* tl_stubs.m: a stand-in if the name says it is data */
uint64_t tl_missing_data_any(const char *sym); /* tl_stubs.m: a stand-in for something known to be data */
void *tl_runtime_lookup(const char *symbol);
static TlImage *g_images, *g_main;
#define TL_MAX_NS 64
static TlImage *g_ns_main[TL_MAX_NS]; /* each namespace's executable */
static int g_loading_ns;              /* the namespace being loaded and bound (under g_lock) */
static TlImage *main_of(int ns) { return ns >= 0 && ns < TL_MAX_NS ? g_ns_main[ns] : NULL; }

/* A process started inside the app (processes as threads). */
typedef struct TlProc {
    int ns, parent_ns;
    pid_t pid;
    TlImage *exe;
    int argc;
    char **argv, **envp;
    pthread_t thread;
    int done, status, kill_requested;
    void *exit_watchers[16]; /* dispatch sources standing in for DISPATCH_SOURCE_TYPE_PROC on this pid */
    int nexit_watchers;
} TlProc;
static TlProc *g_procs[TL_MAX_NS];
static int g_main_argc; /* the program's own arguments (not its host's), once its main runs */
static char **g_main_argv;
static TlImage *g_order[1024]; /* every image, in the order it was loaded */
static _Atomic int g_norder;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- messages ---- */

static void tl_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_host && g_host->log) g_host->log(line);
    else fprintf(stderr, "tl: %s\n", line);
}

void tl_log_line(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    tl_log("%s", line);
}

static void set_err(char *err, size_t len, const char *fmt, ...) {
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

/* ---- the executable region ---- */

int tl_host_init_split(TlHost *host, size_t bytes) {
    memset(host, 0, sizeof *host);
    bytes = (bytes + TL_PAGE - 1) & ~(TL_PAGE - 1);
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur,
                 &max, VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    tl_host_init_region(host, rx, (void *)rw, bytes);
    return 0;
}

void tl_host_init_region(TlHost *host, void *rx, void *rw, size_t bytes) {
    host->rx = rx;
    host->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    host->size = bytes;
    host->used = 0;
}

static uint8_t *region_take(size_t bytes) {
    bytes = (bytes + TL_PAGE - 1) & ~(TL_PAGE - 1);
    if (g_host->used + bytes > g_host->size) return NULL;
    uint8_t *p = g_host->rx + g_host->used;
    g_host->used += bytes;
    return p;
}

/* Code for stubs the loader writes itself (missing symbols). */
static uint8_t *g_stub_page, *g_stub_end, *g_stub_next;

static uint32_t *stub_alloc(size_t bytes) {
    if (!g_stub_page || g_stub_next + bytes > g_stub_end) {
        g_stub_page = region_take(TL_PAGE);
        if (!g_stub_page) return NULL;
        g_stub_next = g_stub_page;
        g_stub_end = g_stub_page + TL_PAGE;
    }
    uint32_t *p = (uint32_t *)g_stub_next;
    g_stub_next += bytes;
    return p;
}

static void code_write(void *rx_dst, const void *src, size_t bytes) {
    memcpy((uint8_t *)rx_dst + g_host->rw_diff, src, bytes);
}

static void code_finish(void *rx, size_t bytes) {
    sys_dcache_flush((uint8_t *)rx + g_host->rw_diff, bytes);
    sys_icache_invalidate(rx, bytes);
}

/* ---- missing symbols ---- */

static char *g_missing[16384];
static int g_nmissing;

int tl_missing_count(void) { return g_nmissing; }
const char *tl_missing_name(int i) { return i >= 0 && i < g_nmissing ? g_missing[i] : NULL; }

typedef struct TlMissing {
    const char *name;
    int called;
} TlMissing;

/* Reached through a missing symbol's stub: stops the program, or (soft_missing) returns 0, said once per symbol. */
static uint64_t tl_missing_called(TlMissing *m) {
    if (!__atomic_exchange_n(&m->called, 1, __ATOMIC_RELAXED))
        tl_log("called a missing symbol: %s%s", m->name, g_host && g_host->soft_missing ? " (returns 0)" : "");
    if (g_host && g_host->soft_missing) return 0;
    fflush(NULL);
    abort();
}

/* ldr x0, record; ldr x16, handler; br x16; nop; then the two literals. */
static uint64_t missing_stub(const char *name) {
    char *copy = strdup(name);
    if (g_nmissing < (int)(sizeof g_missing / sizeof *g_missing)) g_missing[g_nmissing++] = copy;
    TlMissing *rec = calloc(1, sizeof *rec);
    rec->name = copy;
    uint32_t *p = stub_alloc(32);
    if (!p) return 0;
    uint32_t code[8];
    code[0] = 0x58000000u | (4u << 5) | 0;  /* ldr x0, [pc + 16] -> code[4..5] */
    code[1] = 0x58000000u | (5u << 5) | 16; /* ldr x16, [pc + 20] -> code[6..7] */
    code[2] = 0xd61f0200u;                  /* br x16 */
    code[3] = 0xd503201fu;                  /* nop */
    uint64_t rec_addr = (uint64_t)rec, handler = (uint64_t)&tl_missing_called;
    memcpy(&code[4], &rec_addr, 8);
    memcpy(&code[6], &handler, 8);
    code_write(p, code, sizeof code);
    code_finish(p, sizeof code);
    return (uint64_t)p;
}

/* ---- reading the file ---- */

static uint64_t read_uleb(const uint8_t **pp, const uint8_t *end) {
    uint64_t v = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        if (shift < 64) v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

static int64_t read_sleb(const uint8_t **pp, const uint8_t *end) {
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        b = *p++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift);
    *pp = p;
    return v;
}

static int pick_slice(TlImage *im, char *err, size_t errlen) {
    const uint8_t *m = im->map;
    uint32_t magic = *(const uint32_t *)m;
    if (magic == FAT_CIGAM || magic == FAT_MAGIC || magic == FAT_CIGAM_64 || magic == FAT_MAGIC_64) {
        int swap = magic == FAT_CIGAM || magic == FAT_CIGAM_64, wide = magic == FAT_CIGAM_64 || magic == FAT_MAGIC_64;
        const struct fat_header *fh = (const void *)m;
        uint32_t n = swap ? OSSwapInt32(fh->nfat_arch) : fh->nfat_arch;
        int saw_arm64e = 0, saw_x86 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t cpu, sub;
            uint64_t off, size;
            if (wide) {
                const struct fat_arch_64 *a = (const void *)(m + sizeof *fh + i * sizeof(struct fat_arch_64));
                cpu = swap ? OSSwapInt32(a->cputype) : a->cputype;
                sub = swap ? OSSwapInt32(a->cpusubtype) : a->cpusubtype;
                off = swap ? OSSwapInt64(a->offset) : a->offset;
                size = swap ? OSSwapInt64(a->size) : a->size;
            } else {
                const struct fat_arch *a = (const void *)(m + sizeof *fh + i * sizeof(struct fat_arch));
                cpu = swap ? OSSwapInt32(a->cputype) : a->cputype;
                sub = swap ? OSSwapInt32(a->cpusubtype) : a->cpusubtype;
                off = swap ? OSSwapInt32(a->offset) : a->offset;
                size = swap ? OSSwapInt32(a->size) : a->size;
            }
            if (cpu == CPU_TYPE_X86_64) saw_x86 = 1;
            if (cpu != CPU_TYPE_ARM64) continue;
            /* Plain arm64 is subtype 0 or 1; 2 (and newer values, 12 on macOS 27) are arm64e with pointer authentication. */
            if ((sub & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) { saw_arm64e = 1; continue; }
            if (off + size > im->maplen) break;
            im->slice = m + off;
            im->slicelen = size;
            return 0;
        }
        set_err(err, errlen, "%s has no arm64 code%s", im->path,
                saw_arm64e ? " (only arm64e, Apple's own)" : saw_x86 ? " (Intel only)" : "");
        return -1;
    }
    if (magic != MH_MAGIC_64) {
        set_err(err, errlen, "%s is not a 64-bit Mach-O file", im->path);
        return -1;
    }
    const struct mach_header_64 *mh = (const void *)m;
    if (mh->cputype != CPU_TYPE_ARM64 || (mh->cpusubtype & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) {
        set_err(err, errlen, "%s is %s, not arm64", im->path,
                mh->cputype == CPU_TYPE_X86_64 ? "Intel" : mh->cputype == CPU_TYPE_ARM64 ? "arm64e" : "another CPU's code");
        return -1;
    }
    im->slice = m;
    im->slicelen = im->maplen;
    return 0;
}

#define FOR_EACH_CMD(im, lc)                                                                                  \
    for (const struct load_command *lc = (const void *)((im)->slice + sizeof(struct mach_header_64)),        \
                                   *lc##_end = (const void *)((im)->slice + sizeof(struct mach_header_64) + \
                                                              (im)->fmh->sizeofcmds);                        \
         lc < lc##_end && lc->cmdsize; lc = (const void *)((const uint8_t *)lc + lc->cmdsize))

static int parse(TlImage *im, char *err, size_t errlen) {
    if (pick_slice(im, err, errlen)) return -1;
    im->fmh = (const void *)im->slice;
    if (im->fmh->filetype != MH_EXECUTE && im->fmh->filetype != MH_DYLIB && im->fmh->filetype != MH_BUNDLE) {
        set_err(err, errlen, "%s: unsupported Mach-O type %u", im->path, im->fmh->filetype);
        return -1;
    }
    uint64_t lo = UINT64_MAX, hi = 0;
    FOR_EACH_CMD(im, lc) {
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *s = (const void *)lc;
            if (!strcmp(s->segname, "__PAGEZERO")) break;
            if (im->nsegs == TL_MAX_SEGS) { set_err(err, errlen, "%s: too many segments", im->path); return -1; }
            TlSeg *g = &im->segs[im->nsegs++];
            memcpy(g->name, s->segname, 16);
            g->vmaddr = s->vmaddr, g->vmsize = s->vmsize, g->fileoff = s->fileoff, g->filesize = s->filesize;
            g->prot = s->initprot;
            if ((s->vmaddr & (TL_PAGE - 1)) && s->vmsize) {
                set_err(err, errlen, "%s: segment %s is not 16 KB aligned", im->path, g->name);
                return -1;
            }
            if (s->vmsize && s->vmaddr < lo) lo = s->vmaddr;
            if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB: {
            const struct dylib_command *d = (const void *)lc;
            if (im->ndeps == TL_MAX_DEPS) { set_err(err, errlen, "%s: too many libraries", im->path); return -1; }
            TlDep *dep = &im->deps[im->ndeps++];
            dep->install_name = strdup((const char *)lc + d->dylib.name.offset);
            dep->weak = lc->cmd == LC_LOAD_WEAK_DYLIB;
            dep->reexport = lc->cmd == LC_REEXPORT_DYLIB;
            break;
        }
        case LC_RPATH: {
            const struct rpath_command *r = (const void *)lc;
            if (im->nrpaths < TL_MAX_RPATHS) im->rpaths[im->nrpaths++] = strdup((const char *)lc + r->path.offset);
            break;
        }
        case LC_DYLD_EXPORTS_TRIE: {
            const struct linkedit_data_command *l = (const void *)lc;
            im->trie = im->slice + l->dataoff, im->trie_size = l->datasize;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: im->chained = (const void *)lc; break;
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
            im->dyldinfo = (const void *)lc;
            if (!im->trie && im->dyldinfo->export_size)
                im->trie = im->slice + im->dyldinfo->export_off, im->trie_size = im->dyldinfo->export_size;
            break;
        case LC_MAIN: {
            const struct entry_point_command *e = (const void *)lc;
            im->entryoff = e->entryoff, im->has_main = 1;
            break;
        }
        case LC_ENCRYPTION_INFO_64: {
            const struct encryption_info_command_64 *e = (const void *)lc;
            if (e->cryptid) { set_err(err, errlen, "%s is encrypted (App Store FairPlay)", im->path); return -1; }
            break;
        }
        default: break;
        }
    }
    if (!im->nsegs || lo == UINT64_MAX) { set_err(err, errlen, "%s has no segments", im->path); return -1; }
    im->vmmin = lo;
    im->vmsize = (hi - lo + TL_PAGE - 1) & ~(TL_PAGE - 1);
    return 0;
}

/* ---- mapping ---- */

/* Where an image's data lives. Normally right where the file puts it, in place of the executable region's pages
   after its code (ordinary memory mapped over them). Some systems refuse that: an iOS JIT region a debugger made
   (StikDebug, iOS 26) can be neither replaced nor unmapped. Then each image gets an address range of its own,
   ordinary memory, with its code pages mapped into it a second time (vm_remap of the region's pages, made
   read-and-execute) and its data between them as the file has it. Checked once, by running a function through
   such a mapping; TL_CODE_ALIAS=1 (a test switch) uses it from the start. */
static int g_code_alias = -1; /* -1 not decided, 0 data over the region, 1 code aliases */

#include <setjmp.h>
#include <signal.h>
static sigjmp_buf g_alias_jump;
static volatile sig_atomic_t g_alias_probing;
static void alias_fault(int sig) {
    if (g_alias_probing) siglongjmp(g_alias_jump, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* A second mapping of the region's pages [rx, rx + bytes) at dst (which this process owns), read and execute. */
static int alias_code(uint8_t *dst, uint8_t *rx, size_t bytes) {
    vm_address_t at = (vm_address_t)dst;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &at, bytes, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                (vm_address_t)rx, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) { errno = EPERM; tl_log("code alias: vm_remap %s", mach_error_string(kr)); return -1; }
    kr = vm_protect(mach_task_self(), at, bytes, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) { errno = EPERM; tl_log("code alias: vm_protect r-x %s (max %#x)", mach_error_string(kr), max); return -1; }
    sys_icache_invalidate(dst, bytes);
    return 0;
}

/* Whether code runs through such a second mapping here: a two-instruction function, called through one. */
static int alias_works(void) {
    static const uint32_t fn[2] = {0x528B4B40u, 0xD65F03C0u}; /* movz w0, #0x5a5a ; ret */
    uint8_t *rx = region_take(TL_PAGE);
    uint8_t *dst = mmap(NULL, TL_PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!rx || dst == MAP_FAILED) return 0;
    code_write(rx, fn, sizeof fn);
    code_finish(rx, sizeof fn);
    if (alias_code(dst, rx, TL_PAGE)) return 0;
    struct sigaction sa, ob, os, oi;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alias_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob), sigaction(SIGSEGV, &sa, &os), sigaction(SIGILL, &sa, &oi);
    int ok = 0;
    g_alias_probing = 1;
    if (!sigsetjmp(g_alias_jump, 1)) ok = ((int (*)(void))(void *)dst)() == 0x5a5a;
    g_alias_probing = 0;
    sigaction(SIGBUS, &ob, NULL), sigaction(SIGSEGV, &os, NULL), sigaction(SIGILL, &oi, NULL);
    tl_log("code alias test: %s", ok ? "runs" : "faults");
    return ok;
}

/* The other way when the region's pages cannot be replaced: leave the data in the region, at its place after the
   code, and make just those pages writable and not executable (vm_protect read-write). Checked once on a page of
   the region: made read-write, written and read back. */
static int g_data_inplace = -1;
static int inplace_writable(uint8_t *p, size_t bytes) {
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)p, bytes, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) { tl_log("data in place: vm_protect rw- %s", mach_error_string(kr)); errno = EPERM; return -1; }
    return 0;
}
static int inplace_works(void) {
    uint8_t *p = region_take(TL_PAGE);
    if (!p || inplace_writable(p, TL_PAGE)) return 0;
    struct sigaction sa, ob, os;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alias_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob), sigaction(SIGSEGV, &sa, &os);
    int ok = 0;
    g_alias_probing = 1;
    if (!sigsetjmp(g_alias_jump, 1)) {
        volatile uint32_t *w = (volatile uint32_t *)(p + 64);
        *w = 0x5a5a1234u;
        ok = *w == 0x5a5a1234u && *(volatile uint32_t *)(p + 64 + g_host->rw_diff) == 0x5a5a1234u;
    }
    g_alias_probing = 0;
    sigaction(SIGBUS, &ob, NULL), sigaction(SIGSEGV, &os, NULL);
    tl_log("data in place test: %s", ok ? "writable" : "faults");
    return ok;
}

static int map_segments_aliased(TlImage *im, char *err, size_t errlen) {
    uint8_t *base = mmap(NULL, im->vmsize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { set_err(err, errlen, "%s: no address space (%s)", im->path, strerror(errno)); return -1; }
    im->base = base;
    im->slide = (int64_t)((uint64_t)base - im->vmmin);
    for (int i = 0; i < im->nsegs; i++) {
        TlSeg *g = &im->segs[i];
        if (!g->vmsize) continue;
        uint8_t *dst = (uint8_t *)(g->vmaddr + im->slide);
        uint64_t span = (g->vmsize + TL_PAGE - 1) & ~(TL_PAGE - 1);
        uint64_t copy = g->filesize < g->vmsize ? g->filesize : g->vmsize;
        if (g->fileoff + copy > im->slicelen) { set_err(err, errlen, "%s: segment %s is cut short", im->path, g->name); return -1; }
        if (g->prot & VM_PROT_EXECUTE) {
            uint8_t *rx = region_take(span);
            if (!rx) { set_err(err, errlen, "%s: the executable region is full", im->path); return -1; }
            code_write(rx, im->slice + g->fileoff, copy);
            code_finish(rx, span);
            if (alias_code(dst, rx, span)) { set_err(err, errlen, "%s: cannot map code segment %s", im->path, g->name); return -1; }
        } else {
            void *p = mmap(dst, span, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p != dst) { set_err(err, errlen, "%s: cannot place segment %s (%s)", im->path, g->name, strerror(errno)); return -1; }
            memcpy(dst, im->slice + g->fileoff, copy);
        }
    }
    return 0;
}

static int map_segments(TlImage *im, char *err, size_t errlen) {
    if (g_code_alias < 0 && getenv("TL_CODE_ALIAS")) g_code_alias = alias_works() ? 1 : 0;
    if (g_data_inplace < 0 && getenv("TL_DATA_INPLACE")) g_data_inplace = inplace_works() ? 1 : 0;
    if (g_code_alias == 1) return map_segments_aliased(im, err, errlen);
    uint8_t *base = region_take(im->vmsize);
    if (!base) { set_err(err, errlen, "%s: the executable region is full", im->path); return -1; }
    im->base = base;
    im->slide = (int64_t)((uint64_t)base - im->vmmin);
    for (int i = 0; i < im->nsegs; i++) {
        TlSeg *g = &im->segs[i];
        if (!g->vmsize) continue;
        uint8_t *dst = (uint8_t *)(g->vmaddr + im->slide);
        uint64_t span = (g->vmsize + TL_PAGE - 1) & ~(TL_PAGE - 1);
        uint64_t copy = g->filesize < g->vmsize ? g->filesize : g->vmsize;
        if (g->fileoff + copy > im->slicelen) { set_err(err, errlen, "%s: segment %s is cut short", im->path, g->name); return -1; }
        if (g->prot & VM_PROT_EXECUTE) {
            code_write(dst, im->slice + g->fileoff, copy); /* the rest of the region is still zero: it is never reused */
            code_finish(dst, span);
        } else {
            if (g_data_inplace == 1) {
                /* The region's own pages, made writable; the debugger's marks and old contents cleared. */
                if (inplace_writable(dst, span)) { set_err(err, errlen, "%s: cannot make segment %s writable", im->path, g->name); return -1; }
                memset(dst, 0, span);
                memcpy(dst, im->slice + g->fileoff, copy);
                continue;
            }
            /* Ordinary memory in place of these pages of the region, at the file's distance from the code. */
            void *p = mmap(dst, span, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p != dst && g_code_alias < 0 && g_data_inplace < 0 && (errno == EPERM || errno == EACCES)) {
                /* The region's pages cannot be replaced here (iOS 26's debugger-made JIT region): the data stays in
                   the region with its pages made writable, if the system allows that; else code aliases, if code
                   runs through them. This segment (and the rest of this image) again that way. */
                tl_log("data over the executable region refused (%s): trying data in place", strerror(errno));
                g_data_inplace = inplace_works() ? 1 : 0;
                if (g_data_inplace == 1) {
                    i--;
                    continue;
                }
                tl_log("trying code aliases");
                g_code_alias = alias_works() ? 1 : 0;
                if (g_code_alias == 1) return map_segments_aliased(im, err, errlen);
                errno = EPERM;
            }
            if (p != dst) { set_err(err, errlen, "%s: cannot place segment %s (%s)", im->path, g->name, strerror(errno)); return -1; }
            if (g_code_alias < 0) g_code_alias = 0;
            memcpy(dst, im->slice + g->fileoff, copy);
        }
    }
    return 0;
}

/* ---- libraries ---- */

static TlImage *load_one(const char *path, char *err, size_t errlen);

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* @executable_path, @loader_path and @rpath against this image and the executable. */
static int expand_path(TlImage *im, const char *in, char *out, size_t len) {
    TlImage *exe = main_of(im->ns);
    const char *exe_dir = exe ? exe->dir : im->dir;
    if (!strncmp(in, "@executable_path/", 17)) return snprintf(out, len, "%s/%s", exe_dir, in + 17) < (int)len;
    if (!strncmp(in, "@loader_path/", 13)) return snprintf(out, len, "%s/%s", im->dir, in + 13) < (int)len;
    if (!strncmp(in, "@rpath/", 7)) {
        TlImage *owners[2] = {im, exe && exe != im ? exe : NULL};
        for (int o = 0; o < 2; o++) {
            TlImage *who = owners[o];
            if (!who) continue;
            for (int i = 0; i < who->nrpaths; i++) {
                char base[PATH_MAX];
                if (!expand_path(who, who->rpaths[i], base, sizeof base)) continue;
                snprintf(out, len, "%s/%s", base, in + 7);
                if (file_exists(out)) return 1;
            }
        }
        return 0;
    }
    return snprintf(out, len, "%s", in) < (int)len;
}

static int load_deps(TlImage *im, char *err, size_t errlen) {
    for (int i = 0; i < im->ndeps; i++) {
        TlDep *dep = &im->deps[i];
        const char *name = dep->install_name;
        if (name[0] == '@') {
            char path[PATH_MAX];
            if (expand_path(im, name, path, sizeof path) && file_exists(path)) {
                char real[PATH_MAX];
                if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
                dep->tl = load_one(real, err, errlen);
                if (!dep->tl) return -1;
                continue;
            }
            if (dep->weak) { dep->no_library = 1; continue; }
            set_err(err, errlen, "%s needs %s, which is not in the app", im->path, name);
            return -1;
        }
        char buf[PATH_MAX];
        const char *host_path = g_host->map_install_name ? g_host->map_install_name(name, buf, sizeof buf) : name;
        if (host_path) dep->handle = dlopen(host_path, RTLD_LAZY | RTLD_GLOBAL);
        if (!dep->handle) {
            dep->no_library = 1;
            if (g_host->verbose || (!dep->weak && !g_host->shim_lookup))
                tl_log("%s: no host library for %s%s", im->path, name, host_path ? "" : " (none on this host)");
        }
    }
    return 0;
}

/* ---- symbols ---- */

typedef struct TlExport {
    int found;
    uint64_t flags;
    uint64_t value;      /* image offset, or absolute */
    uint64_t other;      /* re-export: library ordinal; resolver offset */
    const char *import;  /* re-export: the name in that library ("" = same name) */
} TlExport;

static TlExport trie_find(const uint8_t *start, uint32_t size, const char *sym) {
    TlExport r = {0};
    const uint8_t *end = start + size, *p = start;
    if (!start || !size) return r;
    for (int depth = 0; depth < 512; depth++) {
        if (p >= end) return r;
        const uint8_t *q = p;
        uint64_t term = read_uleb(&q, end);
        if (*sym == 0 && term) {
            r.flags = read_uleb(&q, end);
            if (r.flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
                r.other = read_uleb(&q, end);
                r.import = (const char *)q;
            } else {
                r.value = read_uleb(&q, end);
                if (r.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) r.other = read_uleb(&q, end);
            }
            r.found = 1;
            return r;
        }
        const uint8_t *kids = q + term;
        if (kids >= end) return r;
        uint8_t n = *kids++;
        const uint8_t *next = NULL;
        for (int i = 0; i < n && !next; i++) {
            const char *edge = (const char *)kids;
            size_t el = strnlen(edge, (size_t)(end - kids));
            kids += el + 1;
            uint64_t off = read_uleb(&kids, end);
            if (!strncmp(sym, edge, el)) {
                sym += el;
                next = start + off;
            }
        }
        if (!next) return r;
        p = next;
    }
    return r;
}

static uint64_t resolve_in_dep(TlImage *im, int ordinal, const char *sym, int *found);
static void *guest_override(const char *install_name, const char *sym);

/* An exported symbol of a guest image, following re-exports. */
static uint64_t image_export(TlImage *im, const char *sym, int *found) {
    TlExport e = trie_find(im->trie, im->trie_size, sym);
    if (!e.found) {
        /* Re-exported libraries without a trie entry (whole-library re-exports). */
        for (int i = 0; i < im->ndeps; i++) {
            if (!im->deps[i].reexport) continue;
            int f = 0;
            uint64_t v = resolve_in_dep(im, i + 1, sym, &f);
            if (f) { *found = 1; return v; }
        }
        *found = 0;
        return 0;
    }
    *found = 1;
    if (e.flags & EXPORT_SYMBOL_FLAGS_REEXPORT)
        return resolve_in_dep(im, (int)e.other, e.import && *e.import ? e.import : sym, found);
    uint64_t kind = e.flags & EXPORT_SYMBOL_FLAGS_KIND_MASK;
    if (kind == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) return e.value;
    if (e.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
        uint64_t (*resolver)(void) = (void *)(im->base + e.other);
        return resolver();
    }
    return (uint64_t)(im->base + e.value);
}

/* ---- Chromium-family code run as one process ----
   Electron in --single-process (Node.js in the browser, Blink's renderer on a thread of the same process) sets cppgc,
   the heap both use, up twice: once for Node, once as the renderer starts, and cppgc::InitializeProcess CHECKs that
   it was not set up before. In one process they share it, so the second call just returns: the branch to the
   CHECK's trap is pointed at the function's return instead. Every instruction involved is checked first; code that
   does not look as expected is left alone. */
static int is_brk(uint32_t i) { return (i & 0xFFE0001Fu) == 0xD4200000u; }
static int is_ldp_sp(uint32_t i) { return (i & 0xFC4003E0u) == 0xA84003E0u; }

/* Electron in one process, continued: Node's platform (node::NodePlatform) is V8's platform for the whole process,
   and it knows only the isolates Node registered with it. Blink's isolates, created on the renderer's thread, are
   not among them, and the first time V8 asks the platform for such an isolate's task runner Node aborts
   (ForIsolate: "Assertion failed: (data.first) != nullptr"). So the platform's GetForegroundTaskRunner and
   IdleTasksEnabled are taken over in its vtable: an isolate Node registered (through the vtable's RegisterIsolate,
   or one used on the main thread, where Node's own runs) gets Node's answer as before; any other gets a task runner
   of Henge's that takes no tasks (V8 then garbage-collects as allocation goes, without its scheduled steps) and no
   idle tasks. The vtables are found by their exported names and each slot is checked against the exported
   function before it is changed. */
void *tl_np_gftr, *tl_np_fallback[2];
static void *g_np_ite, *g_np_reg_loop, *g_np_reg_dlg, *g_np_unreg;
static void *g_np_isolates[512];
static int g_np_n;
static pthread_mutex_t g_np_lock = PTHREAD_MUTEX_INITIALIZER;

static void np_note(void *iso, int add) {
    pthread_mutex_lock(&g_np_lock);
    for (int i = 0; i < g_np_n; i++)
        if (g_np_isolates[i] == iso) {
            if (!add) g_np_isolates[i] = g_np_isolates[--g_np_n];
            pthread_mutex_unlock(&g_np_lock);
            return;
        }
    if (add && g_np_n < 512) g_np_isolates[g_np_n++] = iso;
    pthread_mutex_unlock(&g_np_lock);
}

int tl_np_known(void *platform, void *iso) {
    (void)platform;
    if (pthread_main_np()) return 1;
    pthread_mutex_lock(&g_np_lock);
    int known = 0;
    for (int i = 0; i < g_np_n && !known; i++) known = g_np_isolates[i] == iso;
    pthread_mutex_unlock(&g_np_lock);
    return known;
}

static void np_reg_loop(void *self, void *iso, void *loop) {
    np_note(iso, 1);
    ((void (*)(void *, void *, void *))g_np_reg_loop)(self, iso, loop);
}
static void np_reg_dlg(void *self, void *iso, void *dlg) {
    np_note(iso, 1);
    ((void (*)(void *, void *, void *))g_np_reg_dlg)(self, iso, dlg);
}
static void np_unreg(void *self, void *iso) {
    np_note(iso, 0);
    ((void (*)(void *, void *))g_np_unreg)(self, iso);
}
static int np_ite(void *self, void *iso) {
    if (!tl_np_known(self, iso)) return 0;
    return ((int (*)(void *, void *))g_np_ite)(self, iso);
}

/* GetForegroundTaskRunner returns a std::shared_ptr through x8: Node's for a known isolate, else Henge's. */
void tl_np_gftr_thunk(void);
__asm__(".text\n.p2align 2\n"
        "_tl_np_gftr_thunk:\n"
        "  stp x29, x30, [sp, #-64]!\n"
        "  mov x29, sp\n"
        "  stp x0, x1, [sp, #16]\n"
        "  stp x2, x8, [sp, #32]\n"
        "  bl _tl_np_known\n"
        "  mov w9, w0\n"
        "  ldp x0, x1, [sp, #16]\n"
        "  ldp x2, x8, [sp, #32]\n"
        "  ldp x29, x30, [sp], #64\n"
        "  cbz w9, 1f\n"
        "  adrp x16, _tl_np_gftr@PAGE\n"
        "  ldr x16, [x16, _tl_np_gftr@PAGEOFF]\n"
        "  br x16\n"
        "1:\n"
        "  adrp x16, _tl_np_fallback@PAGE\n"
        "  add x16, x16, _tl_np_fallback@PAGEOFF\n"
        "  ldp x9, x10, [x16]\n"
        "  stp x9, x10, [x8]\n"
        "  ret\n");

/* Henge's task runner: v8::TaskRunner's slots in V8's order (IdleTasksEnabled, NonNestableTasksEnabled,
   NonNestableDelayedTasksEnabled, two destructors, PostTaskImpl, PostNonNestableTaskImpl, PostDelayedTaskImpl,
   PostNonNestableDelayedTaskImpl, PostIdleTaskImpl). A task comes as a pointer to the caller's unique_ptr, which the
   caller destroys: leaving it there drops the task. Its shared_ptr's count starts too high ever to reach zero. */
static long np_false(void) { return 0; }
static void np_nothing(void) {}
static void *g_np_runner_vtbl[10] = {(void *)np_false, (void *)np_false, (void *)np_false, (void *)np_nothing, (void *)np_nothing,
                                     (void *)np_nothing, (void *)np_nothing, (void *)np_nothing, (void *)np_nothing, (void *)np_nothing};
static void *g_np_runner[1] = {g_np_runner_vtbl};
static void *g_np_ctrl_vtbl[6] = {(void *)np_nothing, (void *)np_nothing, (void *)np_nothing, (void *)np_false, (void *)np_nothing, (void *)np_nothing};
static struct { void *vtbl; long shared, weak; } g_np_ctrl = {g_np_ctrl_vtbl, 1L << 50, 1L << 50};

static int np_slot(void **vt, int n, uint64_t want, void *with, void **orig) {
    for (int i = 0; i < n; i++)
        if ((uint64_t)vt[i] == want) {
            *orig = vt[i];
            vt[i] = with;
            return 1;
        }
    return 0;
}

static void node_platform_fixups(TlImage *im) {
    int f[6] = {0};
    uint64_t vt = image_export(im, "__ZTVN4node12NodePlatformE", &f[0]);
    uint64_t gftr = image_export(im, "__ZN4node12NodePlatform23GetForegroundTaskRunnerEPN2v87IsolateENS1_12TaskPriorityE", &f[1]);
    uint64_t ite = image_export(im, "__ZN4node12NodePlatform16IdleTasksEnabledEPN2v87IsolateE", &f[2]);
    uint64_t rl = image_export(im, "__ZN4node12NodePlatform15RegisterIsolateEPN2v87IsolateEP9uv_loop_s", &f[3]);
    uint64_t rd = image_export(im, "__ZN4node12NodePlatform15RegisterIsolateEPN2v87IsolateEPNS_23IsolatePlatformDelegateE", &f[4]);
    uint64_t ur = image_export(im, "__ZN4node12NodePlatform17UnregisterIsolateEPN2v87IsolateE", &f[5]);
    for (int i = 0; i < 6; i++)
        if (!f[i]) return;
    if (tl_np_gftr) return; /* one Electron in the process is all this handles */
    void **slots = (void **)(uintptr_t)vt + 2; /* past offset-to-top and type info */
    void *o[5];
    if (!np_slot(slots, 64, gftr, (void *)tl_np_gftr_thunk, &o[0])) return;
    np_slot(slots, 64, ite, (void *)np_ite, &o[1]);
    np_slot(slots, 64, rl, (void *)np_reg_loop, &o[2]);
    np_slot(slots, 64, rd, (void *)np_reg_dlg, &o[3]);
    np_slot(slots, 64, ur, (void *)np_unreg, &o[4]);
    tl_np_gftr = o[0], g_np_ite = o[1], g_np_reg_loop = o[2], g_np_reg_dlg = o[3], g_np_unreg = o[4];
    tl_np_fallback[0] = g_np_runner, tl_np_fallback[1] = &g_np_ctrl;
    tl_log("node::NodePlatform in %s: isolates it does not know get a task runner of Henge's (one process)", im->path);
}

static void chromium_fixups(TlImage *im) {
    int found = 0;
    uint64_t at = image_export(im, "__ZN5cppgc17InitializeProcessEPN2v813PageAllocatorEm", &found);
    if (!found || !at) return;
    const uint32_t *ins = (const uint32_t *)(uintptr_t)at;
    int ret = -1;
    for (int i = 0; i < 48; i++)
        if (ins[i] == 0xD65F03C0u) { ret = i; break; }
    if (ret < 2) return;
    int epi = ret;
    while (epi > 0 && is_ldp_sp(ins[epi - 1])) epi--;
    if (epi == ret) return;
    for (int i = 0; i < ret; i++) {
        uint32_t w = ins[i];
        if ((w & 0xFF000000u) != 0xB5000000u) continue; /* cbnz Xn */
        int32_t off = (int32_t)(w << 8) >> 13;           /* imm19, sign-extended */
        int t = i + off;
        if (t <= ret || t >= 48 || !is_brk(ins[t])) continue;
        uint32_t patched = (w & 0xFF00001Fu) | ((uint32_t)(epi - i) & 0x7FFFFu) << 5;
        code_write((void *)(ins + i), &patched, 4);
        code_finish((void *)(ins + i), 4);
        tl_log("cppgc::InitializeProcess in %s: a second set-up returns (one process)", im->path);
        return;
    }
}

static void *cf_override(const char *sym);

static uint64_t host_symbol(void *handle, const char *install_name, const char *sym, int *found) {
    *found = 0;
    if (install_name && strstr(install_name, "/CoreFoundation.framework/")) {
        void *s = cf_override(sym);
        if (s) { *found = 1; return (uint64_t)s; }
    }
    if (!install_name || strstr(install_name, "libSystem") || !strncmp(install_name, "/usr/lib/system/", 16) ||
        strstr(install_name, "/libbsm.") || strstr(install_name, "/libsandbox.")) {
        void *s = tl_runtime_lookup(sym);
        if (s) { *found = 1; return (uint64_t)s; }
    }
    if (g_host->shim_lookup && install_name) {
        void *s = g_host->shim_lookup(install_name, sym);
        if (s) { *found = 1; return (uint64_t)s; }
    }
    if (!handle || sym[0] != '_') return 0;
    if (g_host->host_lacks && g_host->host_lacks(install_name ? install_name : "", sym)) return 0;
    void *s = dlsym(handle, sym + 1);
    if (s) *found = 1;
    return (uint64_t)s;
}

static uint64_t flat_lookup(const char *sym, int *found) {
    for (TlImage *im = g_images; im; im = im->next) {
        if (im->ns != g_loading_ns) continue;
        uint64_t v = image_export(im, sym, found);
        if (*found) return v;
    }
    return host_symbol(RTLD_DEFAULT, NULL, sym, found);
}

static uint64_t resolve_in_dep(TlImage *im, int ordinal, const char *sym, int *found) {
    *found = 0;
    if (ordinal == BIND_SPECIAL_DYLIB_SELF) return image_export(im, sym, found);
    if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) return main_of(im->ns) ? image_export(main_of(im->ns), sym, found) : 0;
    if (ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) return flat_lookup(sym, found);
    if (ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) {
        uint64_t v = host_symbol(RTLD_DEFAULT, NULL, sym, found);
        if (*found) return v;
        return flat_lookup(sym, found);
    }
    if (ordinal < 1 || ordinal > im->ndeps) return 0;
    TlDep *dep = &im->deps[ordinal - 1];
    if (dep->tl) {
        void *over = guest_override(dep->install_name, sym);
        if (over) {
            *found = 1;
            return (uint64_t)over;
        }
        return image_export(dep->tl, sym, found);
    }
    return host_symbol(dep->handle, dep->install_name, sym, found);
}

static void tl_stub_binder(void) { abort(); } /* old binaries name it; every lazy pointer is bound up front here */

/* While binding: whether the import being bound is reached through the image's call stubs (1, a function), never
   (2, data or a function taken by address), or unknown (0). Soft missing symbols use it to pick a stand-in. */
static int g_bind_kind;
static int g_binding_static_appkit;
int tl_binding_links_appkit_classes(void) { return g_binding_static_appkit; }

static int resolve_import(TlImage *im, int ordinal, const char *sym, int weak, uint64_t *out, char *err, size_t errlen) {
    if (!strcmp(sym, "dyld_stub_binder")) { *out = (uint64_t)&tl_stub_binder; return 0; }
    int found = 0;
    uint64_t v = resolve_in_dep(im, ordinal, sym, &found);
    if (found) { *out = v; return 0; }
    /* A missing weak import is normally 0, for code that tests its address. Soft: a stand-in like any other, since
       code that checks the OS version instead (@available, and the host here is new) would use 0 and crash. */
    if (weak && !g_host->soft_missing) { *out = 0; return 0; }
    const char *lib = ordinal >= 1 && ordinal <= im->ndeps ? im->deps[ordinal - 1].install_name : "(flat)";
    if (g_host->strict) {
        set_err(err, errlen, "%s: %s is missing from %s", im->path, sym, lib);
        return -1;
    }
    if (g_host->verbose) tl_log("%s: %s missing from %s, bound to a stub", im->path, sym, lib);
    if (g_host->soft_missing) {
        uint64_t data = g_bind_kind == 2 ? tl_missing_data_any(sym) : tl_missing_data(sym); /* tl_stubs.m */
        if (data) {
            if (g_nmissing < (int)(sizeof g_missing / sizeof *g_missing)) g_missing[g_nmissing++] = strdup(sym);
            *out = data;
            return 0;
        }
    }
    *out = missing_stub(sym);
    return 0;
}

/* ---- fixups ---- */

static uint8_t *find_section(TlImage *im, const char *seg, const char *name, uint64_t *size);

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Which imports the image calls through its stubs (adrp x16; ldr x16, [x16, #slot]; br x16): a flag per import,
   from the pointer slots the stubs load and the chains that bind those slots. NULL if it cannot tell. */
static uint8_t *called_imports(TlImage *im, const struct dyld_chained_fixups_header *h, const uint8_t *data) {
    uint64_t size = 0;
    uint8_t *stubs = find_section(im, "__TEXT", "__stubs", &size);
    if (!stubs || size < 12) return NULL;
    size_t n = size / 12, m = 0;
    uint64_t *slots = malloc(n * sizeof *slots);
    for (size_t k = 0; k < n; k++) {
        const uint32_t *w = (const uint32_t *)(stubs + 12 * k);
        if ((w[0] & 0x9F00001Fu) != 0x90000010u || (w[1] & 0xFFC003FFu) != 0xF9400210u) continue;
        int64_t imm = (int64_t)((((w[0] >> 5) & 0x7FFFF) << 2) | ((w[0] >> 29) & 3));
        if (imm & (1 << 20)) imm -= 1 << 21;
        uint64_t page = ((uint64_t)(stubs + 12 * k) & ~0xFFFull) + (uint64_t)(imm << 12);
        slots[m++] = page + ((w[1] >> 10) & 0xFFF) * 8;
    }
    if (!m) { free(slots); return NULL; }
    qsort(slots, m, sizeof *slots, cmp_u64);
    uint8_t *called = calloc(h->imports_count ? h->imports_count : 1, 1);
    const struct dyld_chained_starts_in_image *starts = (const void *)(data + h->starts_offset);
    for (uint32_t s = 0; s < starts->seg_count; s++) {
        if (!starts->seg_info_offset[s]) continue;
        const struct dyld_chained_starts_in_segment *seg = (const void *)((const uint8_t *)starts + starts->seg_info_offset[s]);
        if (seg->pointer_format != DYLD_CHAINED_PTR_64 && seg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) continue;
        for (uint32_t pg = 0; pg < seg->page_count; pg++) {
            uint16_t off = seg->page_start[pg];
            if (off == DYLD_CHAINED_PTR_START_NONE) continue;
            uint8_t *loc = im->base + seg->segment_offset + (uint64_t)pg * seg->page_size + off;
            for (;;) {
                uint64_t raw;
                memcpy(&raw, loc, 8);
                if (raw >> 63) {
                    uint32_t ordinal = raw & 0xFFFFFF;
                    uint64_t at = (uint64_t)loc;
                    if (ordinal < h->imports_count && bsearch(&at, slots, m, sizeof *slots, cmp_u64)) called[ordinal] = 1;
                }
                uint64_t next = (raw >> 51) & 0xFFF;
                if (!next) break;
                loc += next * 4;
            }
        }
    }
    free(slots);
    return called;
}

static int fixup_chained(TlImage *im, char *err, size_t errlen) {
    const uint8_t *data = im->slice + im->chained->dataoff;
    const struct dyld_chained_fixups_header *h = (const void *)data;
    if (h->fixups_version != 0 || h->symbols_format != 0) {
        set_err(err, errlen, "%s: unknown chained fixups version", im->path);
        return -1;
    }
    uint64_t *targets = calloc(h->imports_count ? h->imports_count : 1, sizeof *targets);
    const char *syms = (const char *)data + h->symbols_offset;
    const uint8_t *imp = data + h->imports_offset;
    uint8_t *called = g_host->soft_missing ? called_imports(im, h, data) : NULL;
    g_binding_static_appkit = 0;
    for (uint32_t i = 0; i < h->imports_count && !g_binding_static_appkit; i++) {
        int ordinal;
        uint32_t name_off;
        if (h->imports_format == DYLD_CHAINED_IMPORT) {
            const struct dyld_chained_import *x = (const void *)(imp + 4 * i);
            ordinal = x->lib_ordinal, name_off = x->name_offset;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
            const struct dyld_chained_import_addend *x = (const void *)(imp + 8 * i);
            ordinal = x->lib_ordinal, name_off = x->name_offset;
        } else break;
        const char *name = syms + name_off;
        if (ordinal >= 1 && ordinal <= im->ndeps && !strncmp(name, "_OBJC_CLASS_$_NS", 16) &&
            (strstr(im->deps[ordinal - 1].install_name, "/AppKit.framework/") || strstr(im->deps[ordinal - 1].install_name, "/Cocoa.framework/")))
            g_binding_static_appkit = 1;
    }
    for (uint32_t i = 0; i < h->imports_count; i++) {
        int ordinal, weak;
        uint32_t name_off;
        int64_t addend = 0;
        if (h->imports_format == DYLD_CHAINED_IMPORT) {
            const struct dyld_chained_import *x = (const void *)(imp + 4 * i);
            ordinal = x->lib_ordinal > 0xF0 ? (int8_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
            const struct dyld_chained_import_addend *x = (const void *)(imp + 8 * i);
            ordinal = x->lib_ordinal > 0xF0 ? (int8_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset, addend = x->addend;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND64) {
            const struct dyld_chained_import_addend64 *x = (const void *)(imp + 16 * i);
            ordinal = x->lib_ordinal > 0xFFF0 ? (int16_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset, addend = (int64_t)x->addend;
        } else {
            set_err(err, errlen, "%s: unknown import format %u", im->path, h->imports_format);
            free(targets);
            return -1;
        }
        uint64_t v;
        g_bind_kind = called ? (called[i] ? 1 : 2) : 0;
        int rc = resolve_import(im, ordinal, syms + name_off, weak, &v, err, errlen);
        g_bind_kind = 0;
        if (rc) { free(targets); free(called); return -1; }
        targets[i] = v ? v + (uint64_t)addend : 0;
    }
    free(called);
    const struct dyld_chained_starts_in_image *starts = (const void *)(data + h->starts_offset);
    for (uint32_t s = 0; s < starts->seg_count; s++) {
        if (!starts->seg_info_offset[s]) continue;
        const struct dyld_chained_starts_in_segment *seg = (const void *)((const uint8_t *)starts + starts->seg_info_offset[s]);
        if (seg->pointer_format != DYLD_CHAINED_PTR_64 && seg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            set_err(err, errlen, "%s: pointer format %u (arm64e?) is not supported", im->path, seg->pointer_format);
            free(targets);
            return -1;
        }
        for (uint32_t pg = 0; pg < seg->page_count; pg++) {
            uint16_t off = seg->page_start[pg];
            if (off == DYLD_CHAINED_PTR_START_NONE) continue;
            uint8_t *loc = im->base + seg->segment_offset + (uint64_t)pg * seg->page_size + off;
            for (;;) {
                uint64_t raw;
                memcpy(&raw, loc, 8);
                uint64_t next, val;
                if (raw >> 63) { /* bind */
                    uint32_t ordinal = raw & 0xFFFFFF;
                    uint64_t addend = (raw >> 24) & 0xFF;
                    next = (raw >> 51) & 0xFFF;
                    val = ordinal < h->imports_count ? targets[ordinal] : 0;
                    if (val) val += addend;
                } else {
                    uint64_t target = raw & 0xFFFFFFFFFull, high8 = (raw >> 36) & 0xFF;
                    next = (raw >> 51) & 0xFFF;
                    val = seg->pointer_format == DYLD_CHAINED_PTR_64 ? target + (uint64_t)im->slide
                                                                      : (uint64_t)im->base + target;
                    val |= high8 << 56;
                }
                memcpy(loc, &val, 8);
                if (!next) break;
                loc += next * 4;
            }
        }
    }
    free(targets);
    return 0;
}

static uint8_t *seg_addr(TlImage *im, int seg, uint64_t off) {
    if (seg < 0 || seg >= im->nsegs) return NULL;
    return (uint8_t *)(im->segs[seg].vmaddr + im->slide + off);
}

/* The segment index the opcodes use counts every LC_SEGMENT_64, __PAGEZERO included. */
static int opcode_seg(TlImage *im, int index) {
    int k = 0, mine = 0;
    FOR_EACH_CMD(im, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *s = (const void *)lc;
        int skipped = !strcmp(s->segname, "__PAGEZERO");
        if (k == index) return skipped ? -1 : mine;
        if (!skipped) mine++;
        k++;
    }
    return -1;
}

static int do_rebases(TlImage *im, char *err, size_t errlen) {
    const uint8_t *p = im->slice + im->dyldinfo->rebase_off, *end = p + im->dyldinfo->rebase_size;
    int seg = -1, type = REBASE_TYPE_POINTER;
    uint64_t off = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & REBASE_OPCODE_MASK, imm = b & REBASE_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case REBASE_OPCODE_DONE: return 0;
        case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: off += imm * 8; break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES: count = imm, skip = 0; goto rebase;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: count = read_uleb(&p, end), skip = 0; goto rebase;
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto rebase;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        rebase:
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (!loc || type != REBASE_TYPE_POINTER) { set_err(err, errlen, "%s: bad rebase", im->path); return -1; }
                uint64_t v;
                memcpy(&v, loc, 8);
                v += (uint64_t)im->slide;
                memcpy(loc, &v, 8);
                off += 8 + skip;
            }
            break;
        default: set_err(err, errlen, "%s: unknown rebase opcode %#x", im->path, op); return -1;
        }
    }
    return 0;
}

static int in_section(TlImage *im, const uint8_t *loc, const char *seg, const char *name) {
    uint64_t size = 0;
    const uint8_t *s = find_section(im, seg, name, &size);
    return s && loc >= s && loc < s + size;
}

static int do_binds(TlImage *im, const uint8_t *p, const uint8_t *end, int lazy, char *err, size_t errlen) {
    int seg = -1, ordinal = 0, weak = 0;
    const char *sym = "";
    int64_t addend = 0;
    uint64_t off = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case BIND_OPCODE_DONE: if (!lazy) return 0; break; /* lazy binds end each entry with DONE */
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = imm; break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ordinal = (int)read_uleb(&p, end); break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = imm ? (int)(int8_t)(BIND_OPCODE_MASK | imm) : 0; break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            p += strlen(sym) + 1;
            weak = (imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND: count = 1, skip = 0; goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1, skip = imm * 8ull; goto bind;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        bind: {
            uint64_t v;
            /* A bind into __got is data (constants, classes) or an address taken, never a call (calls go through
               __la_symbol_ptr): missing, it gets a data stand-in (a string of its name), not a function stub, whose
               code an app would read as an object (Qt's NSPasteboardType and NSImageHint constants). */
            uint8_t *first = seg_addr(im, seg, off);
            g_bind_kind = !lazy && first && (in_section(im, first, "__DATA_CONST", "__got") || in_section(im, first, "__DATA", "__got")) ? 2 : 0;
            int rc = resolve_import(im, ordinal, sym, weak, &v, err, errlen);
            g_bind_kind = 0;
            if (rc) return -1;
            if (v) v += (uint64_t)addend;
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (!loc) { set_err(err, errlen, "%s: bad bind for %s", im->path, sym); return -1; }
                memcpy(loc, &v, 8);
                off += 8 + skip;
            }
            break;
        }
        case BIND_OPCODE_THREADED: set_err(err, errlen, "%s: threaded binds are not supported", im->path); return -1;
        default: set_err(err, errlen, "%s: unknown bind opcode %#x", im->path, op); return -1;
        }
    }
    return 0;
}

/* Weak definitions: where the image's own definition should give way to one already loaded. */
static int do_weak_binds(TlImage *im) {
    const uint8_t *p = im->slice + im->dyldinfo->weak_bind_off, *end = p + im->dyldinfo->weak_bind_size;
    int seg = -1;
    const char *sym = "";
    int64_t addend = 0;
    uint64_t off = 0;
    int strong = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case BIND_OPCODE_DONE: return 0;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            p += strlen(sym) + 1;
            strong = (imm & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) != 0;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND: count = 1, skip = 0; goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1, skip = imm * 8ull; goto bind;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        bind: {
            int found = 0;
            uint64_t v = strong ? 0 : host_symbol(RTLD_DEFAULT, NULL, sym, &found);
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (loc && found) { uint64_t w = v + (uint64_t)addend; memcpy(loc, &w, 8); }
                off += 8 + skip;
            }
            break;
        }
        default: return 0;
        }
    }
    return 0;
}

static int fixup(TlImage *im, char *err, size_t errlen) {
    if (im->chained) return fixup_chained(im, err, errlen);
    if (!im->dyldinfo) return 0;
    if (do_rebases(im, err, errlen)) return -1;
    const uint8_t *s = im->slice;
    const struct dyld_info_command *d = im->dyldinfo;
    if (do_binds(im, s + d->bind_off, s + d->bind_off + d->bind_size, 0, err, errlen)) return -1;
    if (do_binds(im, s + d->lazy_bind_off, s + d->lazy_bind_off + d->lazy_bind_size, 1, err, errlen)) return -1;
    if (d->weak_bind_size) do_weak_binds(im);
    return 0;
}

/* ---- sections ---- */

typedef void (^TlSectionVisitor)(const struct section_64 *sect, uint8_t *addr);

static void each_section(TlImage *im, TlSectionVisitor visit) {
    FOR_EACH_CMD(im, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *s = (const void *)lc;
        const struct section_64 *sect = (const void *)(s + 1);
        for (uint32_t i = 0; i < s->nsects; i++) visit(&sect[i], (uint8_t *)(sect[i].addr + im->slide));
    }
}

static uint8_t *find_section(TlImage *im, const char *seg, const char *name, uint64_t *size) {
    __block uint8_t *found = NULL;
    __block uint64_t sz = 0;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        if (!found && !strncmp(sect->segname, seg, 16) && !strncmp(sect->sectname, name, 16)) found = addr, sz = sect->size;
    });
    if (size) *size = sz;
    return found;
}

/* ---- thread-local variables ---- */

typedef struct TlvDesc {
    void *thunk;
    uint64_t key;
    uint64_t offset;
} TlvDesc;

extern void *tl_tlv_get_addr(TlvDesc *d); /* tl_tlv.S */

static TlImage *g_tlv_images[256];
static int g_ntlv;

void *tl_tlv_instantiate(TlvDesc *d) {
    TlImage *im = NULL;
    for (int i = 0; i < g_ntlv; i++)
        if ((uint64_t)g_tlv_images[i]->tlv_key == d->key) im = g_tlv_images[i];
    if (!im) abort();
    uint8_t *mem = malloc(im->tlv_size ? im->tlv_size : 1);
    uint8_t *tmpl = (uint8_t *)(im->tlv_start + im->slide);
    memcpy(mem, tmpl, im->tlv_init_size);
    memset(mem + im->tlv_init_size, 0, im->tlv_size - im->tlv_init_size);
    pthread_setspecific(im->tlv_key, mem);
    return mem;
}

static int setup_tlv(TlImage *im, char *err, size_t errlen) {
    __block uint64_t lo = UINT64_MAX, hi = 0, init_hi = 0;
    __block TlvDesc *descs = NULL;
    __block uint64_t ndescs = 0;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        uint32_t type = sect->flags & SECTION_TYPE;
        if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) {
            if (sect->addr < lo) lo = sect->addr;
            if (sect->addr + sect->size > hi) hi = sect->addr + sect->size;
            if (type == S_THREAD_LOCAL_REGULAR && sect->addr + sect->size > init_hi) init_hi = sect->addr + sect->size;
        } else if (type == S_THREAD_LOCAL_VARIABLES) {
            descs = (TlvDesc *)addr;
            ndescs = sect->size / sizeof(TlvDesc);
        }
    });
    if (!descs) return 0;
    if (lo == UINT64_MAX) lo = hi = init_hi = 0;
    im->tlv_start = lo, im->tlv_size = hi - lo, im->tlv_init_size = init_hi > lo ? init_hi - lo : 0;
    if (pthread_key_create(&im->tlv_key, free)) { set_err(err, errlen, "%s: no thread-local key", im->path); return -1; }
    if (g_ntlv == (int)(sizeof g_tlv_images / sizeof *g_tlv_images)) { set_err(err, errlen, "too many images with thread-local variables"); return -1; }
    g_tlv_images[g_ntlv++] = im;
    for (uint64_t i = 0; i < ndescs; i++) {
        descs[i].thunk = (void *)&tl_tlv_get_addr;
        descs[i].key = im->tlv_key;
    }
    return 0;
}

/* ---- unwinding ---- */

struct tl_unwind_sections {
    uintptr_t dso_base;
    uintptr_t dwarf_section;
    size_t dwarf_section_length;
    uintptr_t compact_unwind_section;
    size_t compact_unwind_section_length;
};

static int find_unwind(uintptr_t addr, struct tl_unwind_sections *info) {
    for (TlImage *im = g_images; im; im = im->next) {
        if (addr < (uintptr_t)im->base || addr >= (uintptr_t)im->base + im->vmsize) continue;
        uint64_t eh = 0, cu = 0;
        uint8_t *ehp = find_section(im, "__TEXT", "__eh_frame", &eh);
        uint8_t *cup = find_section(im, "__TEXT", "__unwind_info", &cu);
        info->dso_base = (uintptr_t)im->base;
        info->dwarf_section = (uintptr_t)ehp, info->dwarf_section_length = ehp ? eh : 0;
        info->compact_unwind_section = (uintptr_t)cup, info->compact_unwind_section_length = cup ? cu : 0;
        return 1;
    }
    return 0;
}

static void register_unwinder(void) {
    static int done;
    if (done) return;
    done = 1;
    int (*add)(int (*)(uintptr_t, struct tl_unwind_sections *)) = dlsym(RTLD_DEFAULT, "__unw_add_find_dynamic_unwind_sections");
    if (add) add(find_unwind);
    else tl_log("this host cannot unwind loaded code: exceptions through it will abort");
}

/* ---- runtime registration ---- */

static void register_swift(TlImage *im) {
    static void (*protos)(const void *, const void *), (*confs)(const void *, const void *), (*types)(const void *, const void *);
    uint64_t n;
    uint8_t *p;
    if (!protos) {
        protos = dlsym(RTLD_DEFAULT, "swift_registerProtocols");
        confs = dlsym(RTLD_DEFAULT, "swift_registerProtocolConformances");
        types = dlsym(RTLD_DEFAULT, "swift_registerTypeMetadataRecords");
    }
    int any = 0;
    if ((p = find_section(im, "__TEXT", "__swift5_protos", &n)) && n) { any = 1; if (protos) protos(p, p + n); }
    if ((p = find_section(im, "__TEXT", "__swift5_proto", &n)) && n) { any = 1; if (confs) confs(p, p + n); }
    if ((p = find_section(im, "__TEXT", "__swift5_types", &n)) && n) { any = 1; if (types) types(p, p + n); }
    if (any && !(protos && confs && types)) tl_log("%s uses Swift, but the Swift runtime is not loaded", im->path);
}

static int has_objc(TlImage *im) {
    return find_section(im, "__DATA", "__objc_imageinfo", NULL) || find_section(im, "__DATA_CONST", "__objc_imageinfo", NULL);
}

typedef struct ProgramVars {
    const void *mh;
    int *NXArgcPtr;
    const char ***NXArgvPtr;
    const char ***environPtr;
    const char **__prognamePtr;
} ProgramVars;

extern int *_NSGetArgc(void);
extern char ***_NSGetArgv(void);
extern char ***_NSGetEnviron(void);
extern char **_NSGetProgname(void);

static void run_initializers(TlImage *im) {
    if (im->initialized || im->visiting) return;
    im->visiting = 1;
    for (int i = 0; i < im->ndeps; i++)
        if (im->deps[i].tl) run_initializers(im->deps[i].tl);
    im->visiting = 0;
    im->initialized = 1;
    if (has_objc(im)) {
        void (*load)(const char *, const void *) = dlsym(RTLD_DEFAULT, "_objc_load_image");
        if (load) load(im->path, im->base);
    }
    ProgramVars vars = {im->base, _NSGetArgc(), (const char ***)_NSGetArgv(), (const char ***)_NSGetEnviron(),
                        (const char **)_NSGetProgname()};
    TlProc *proc = im->ns > 0 && im->ns < TL_MAX_NS ? g_procs[im->ns] : NULL;
    if (proc) vars.NXArgcPtr = &proc->argc, vars.NXArgvPtr = (const char ***)&proc->argv, vars.environPtr = (const char ***)&proc->envp;
    static const char *apple_storage[] = {NULL};
    const char **apple = apple_storage;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        uint32_t type = sect->flags & SECTION_TYPE;
        typedef void (*Init)(int, const char **, const char **, const char **, const ProgramVars *);
        if (type == S_MOD_INIT_FUNC_POINTERS) {
            Init *fns = (Init *)addr;
            for (uint64_t k = 0; k < sect->size / 8; k++)
                fns[k](*vars.NXArgcPtr, *vars.NXArgvPtr, *vars.environPtr, apple, &vars);
        } else if (type == S_INIT_FUNC_OFFSETS) {
            uint32_t *offs = (uint32_t *)addr;
            for (uint64_t k = 0; k < sect->size / 4; k++)
                ((Init)(im->base + offs[k]))(*vars.NXArgcPtr, *vars.NXArgvPtr, *vars.environPtr, apple, &vars);
        }
    });
}

/* ---- loading ---- */

static TlImage *g_pending[512];
static int g_npending;

static TlImage *load_one(const char *path, char *err, size_t errlen) {
    for (TlImage *im = g_images; im; im = im->next)
        if (!strcmp(im->path, path) && im->ns == g_loading_ns) return im;
    TlImage *im = calloc(1, sizeof *im);
    im->ns = g_loading_ns;
    im->path = strdup(path);
    im->dir = strdup(path);
    char *slash = strrchr(im->dir, '/');
    if (slash) *slash = 0;
    im->fd = open(path, O_RDONLY);
    struct stat st;
    if (im->fd < 0 || fstat(im->fd, &st)) { set_err(err, errlen, "cannot open %s: %s", path, strerror(errno)); return NULL; }
    im->maplen = (size_t)st.st_size;
    im->map = mmap(NULL, im->maplen, PROT_READ, MAP_PRIVATE, im->fd, 0);
    if (im->map == MAP_FAILED) { set_err(err, errlen, "cannot read %s", path); return NULL; }
    if (parse(im, err, errlen) || map_segments(im, err, errlen)) return NULL;
    if (im->fmh->filetype == MH_EXECUTE && !main_of(im->ns)) {
        if (im->ns < TL_MAX_NS) g_ns_main[im->ns] = im;
        if (im->ns == 0 && !g_main) g_main = im;
    }
    /* Listed before its libraries load, so a cycle finds it. */
    im->next = g_images;
    g_images = im;
    if (g_norder < (int)(sizeof g_order / sizeof *g_order)) { g_order[g_norder] = im; g_norder++; }
    if (g_npending < (int)(sizeof g_pending / sizeof *g_pending)) g_pending[g_npending++] = im;
    if (g_host->verbose) tl_log("mapped %s at %p (%#llx bytes)", path, im->base, (unsigned long long)im->vmsize);
    if (load_deps(im, err, errlen)) return NULL;
    return im;
}

/* A started process's copy of a library keeps only the categories on its own classes: one on a class the process
   shares (Foundation's, Henge's AppKit's, the first copy's) would re-point that class's methods at this copy's code
   and globals, under the program that already uses them. Such a category's class is cleared, which the runtime
   skips (as for a weak class that is missing); the copy's calls reach the program's copy of the same methods. */
static void keep_own_categories(TlImage *im) {
    /* Another copy of this library (an earlier process's) has added its categories to the system's classes already:
       those stay the first copy's. A library only this process loads (Steam's web helper's CEF) adds its own. */
    int earlier_copy = 0;
    for (int i = 0; i < g_norder && !earlier_copy; i++)
        earlier_copy = g_order[i] != im && g_order[i]->ns != im->ns && !strcmp(g_order[i]->path, im->path);
    static const char *const lists[] = {"__objc_catlist", "__objc_catlist2", "__objc_nlcatlist"};
    static const char *const segs[] = {"__DATA_CONST", "__DATA"};
    for (int l = 0; l < 3; l++)
        for (int g = 0; g < 2; g++) {
            uint64_t size = 0;
            uint8_t *sect = find_section(im, segs[g], lists[l], &size);
            if (!sect) continue;
            for (uint64_t off = 0; off + 8 <= size; off += 8) {
                uint64_t *cat = *(uint64_t **)(sect + off);
                if (!cat) continue;
                void *cls = (void *)cat[1]; /* category_t: name, cls, ... */
                TlImage *owner = cls ? tl_image_containing(cls) : NULL;
                if (owner ? owner->ns == im->ns : !earlier_copy) continue;
                cat[1] = 0;
            }
        }
}

/* Loads path and what it links; fixes up, registers and initializes whatever was new. Takes g_lock itself. */
static TlImage *load_and_link(const char *path, int ns, char *err, size_t errlen) {
    pthread_mutex_lock(&g_lock);
    g_npending = 0;
    g_loading_ns = ns;
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        set_err(err, errlen, "cannot find %s", path);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    TlImage *exe = load_one(real, err, errlen);
    if (!exe) { pthread_mutex_unlock(&g_lock); return NULL; }
    int n = g_npending;
    TlImage *batch[512];
    memcpy(batch, g_pending, n * sizeof *batch);
    for (int i = 0; i < n; i++)
        if (fixup(batch[i], err, errlen) || setup_tlv(batch[i], err, errlen)) { pthread_mutex_unlock(&g_lock); return NULL; }
    register_unwinder();
    for (int i = 0; i < n; i++) {
        chromium_fixups(batch[i]);
        node_platform_fixups(batch[i]);
    }
    if (g_host->image_linked)
        for (int i = 0; i < n; i++) g_host->image_linked(batch[i]);
    for (int i = 0; i < n; i++) register_swift(batch[i]);
    for (int i = 0; i < n; i++)
        if (batch[i]->ns) keep_own_categories(batch[i]);
    const struct mach_header *mhs[512];
    const char *paths[512];
    int nobjc = 0;
    for (int i = n - 1; i >= 0; i--) /* libraries first, as dyld hands them over */
        if (has_objc(batch[i])) mhs[nobjc] = (const void *)batch[i]->base, paths[nobjc++] = batch[i]->path;
    if (nobjc) {
        void (*map)(unsigned, const char *const *, const struct mach_header *const *) = dlsym(RTLD_DEFAULT, "_objc_map_images");
        if (!map) { set_err(err, errlen, "this host's Objective-C runtime cannot take images (needs iOS 16 or macOS 13)"); pthread_mutex_unlock(&g_lock); return NULL; }
        map((unsigned)nobjc, paths, mhs);
    }
    pthread_mutex_unlock(&g_lock);
    run_initializers(exe);
    return exe;
}

TlImage *tl_load(TlHost *host, const char *path, char *err, size_t errlen) {
    g_host = host;
    return load_and_link(path, 0, err, errlen);
}

TlImage *tl_load_library(const char *path, char *err, size_t errlen) {
    if (!g_host) { set_err(err, errlen, "nothing loaded yet"); return NULL; }
    return load_and_link(path, 0, err, errlen);
}

static TlImage *load_library_in(const char *path, int ns, char *err, size_t errlen) {
    if (!g_host) { set_err(err, errlen, "nothing loaded yet"); return NULL; }
    return load_and_link(path, ns, err, errlen);
}

extern char **environ;

int tl_run_main(TlImage *exe, int argc, const char **argv, const char **envp) {
    if (!exe->has_main) return -1;
    /* As the kernel lays a program's start out: argv, NULL, the environment, NULL, the "apple" strings, NULL, in one
       vector. Go's runtime walks past argv's end for the environment and the executable's path; given argv alone it
       read whatever followed (no environment, then a crash on a bad path). An empty environment is this process's. */
    const char **env = envp && envp[0] ? envp : (const char **)environ;
    int ne = 0;
    while (env && env[ne]) ne++;
    const char **v = calloc((size_t)argc + ne + 4, sizeof *v);
    if (!v) return -1;
    memcpy(v, argv, (size_t)argc * sizeof *v);
    if (ne) memcpy(v + argc + 1, env, (size_t)ne * sizeof *v);
    char *exe_path = NULL;
    if (asprintf(&exe_path, "executable_path=%s", exe->path) > 0) v[argc + ne + 2] = exe_path;
    if (exe->ns == 0 && !g_main_argv) g_main_argc = argc, g_main_argv = (char **)v; /* _NSGetArgv's answer */
    int (*entry)(int, const char **, const char **, const char **) = (void *)(exe->base + exe->entryoff);
    return entry(argc, v, v + argc + 1, v + argc + ne + 2);
}

void *tl_image_base(const TlImage *im) { return im->base; }
size_t tl_image_size(const TlImage *im) { return im->vmsize; }
const char *tl_image_path(const TlImage *im) { return im->path; }
void *tl_image_symbol(TlImage *im, const char *sym) {
    int found = 0;
    uint64_t v = image_export(im, sym, &found);
    return found ? (void *)v : NULL;
}

/* ---- the program's view of dyld ---- */

#include <mach-o/dyld.h>
#include <spawn.h>

TlImage *tl_image_containing(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    int n = g_norder;
    for (int i = 0; i < n; i++) {
        TlImage *im = g_order[i];
        if (a >= (uintptr_t)im->base && a < (uintptr_t)im->base + im->vmsize) return im;
    }
    return NULL;
}

/* The namespace ("process") of the code at addr: 0 for the program and for anything not loaded here. */
static int ns_of(const void *addr) {
    TlImage *im = tl_image_containing(addr);
    return im ? im->ns : 0;
}
int tl_ns_of_address(const void *addr) { return ns_of(addr); }

/* The handle dlopen gives for OpenGL by path (see rt_dlopen). */
static char g_opengl_handle;
static const char *const g_opengl_name = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";

static TlImage *as_image(void *handle) {
    int n = g_norder;
    for (int i = 0; i < n; i++)
        if ((void *)g_order[i] == handle) return g_order[i];
    return NULL;
}

static __thread char g_dlerror[1024];
static __thread int g_dlerror_set;

static void rt_set_dlerror(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dlerror, sizeof g_dlerror, fmt, ap);
    va_end(ap);
    g_dlerror_set = 1;
}

static char *rt_dlerror(void) {
    if (!g_dlerror_set) {
        char *host = dlerror();
        return host;
    }
    g_dlerror_set = 0;
    return g_dlerror;
}

static int is_system_path(const char *p) {
    return !strncmp(p, "/usr/lib/", 9) || !strncmp(p, "/System/", 8);
}

static void *rt_dlopen(const char *path, int mode) {
    int ns = ns_of(__builtin_return_address(0));
    TlImage *exe = main_of(ns);
    if (!path) return exe ? (void *)exe : dlopen(NULL, mode);
    char expanded[PATH_MAX];
    if (path[0] == '@' && exe) {
        if (!expand_path(exe, path, expanded, sizeof expanded)) { rt_set_dlerror("cannot find %s", path); return NULL; }
        path = expanded;
    }
    if (!is_system_path(path)) {
        struct stat st;
        if (stat(path, &st) == 0) {
            char err[1024] = "";
            TlImage *im = load_library_in(path, ns, err, sizeof err);
            if (!im) { rt_set_dlerror("%s", err); tl_log("dlopen %s: %s", path, err); return NULL; }
            if (g_host->verbose) tl_log("dlopen %s: loaded here (namespace %d)", path, ns);
            return im;
        }
        /* A bare name ("libfoo.dylib"): look beside the program, as dyld's fallback paths would. */
        if (!strchr(path, '/') && exe) {
            char beside[PATH_MAX];
            snprintf(beside, sizeof beside, "%s/%s", exe->dir, path);
            if (stat(beside, &st) == 0) {
                char err[1024] = "";
                TlImage *im = load_library_in(beside, ns, err, sizeof err);
                if (!im) rt_set_dlerror("%s", err);
                return im;
            }
        }
    }
    char buf[PATH_MAX];
    const char *host_path = g_host->map_install_name ? g_host->map_install_name(path, buf, sizeof buf) : path;
    if (!host_path && strstr(path, "/OpenGL.framework/") && g_host->shim_lookup) {
        /* OpenGL opened by path (SDL loads libGL.dylib, GLFW the framework): the host has none, Henge's AppKit
           provides its functions; dlsym on this handle finds them there. */
        if (g_host->verbose) tl_log("dlopen %s: Henge's OpenGL", path);
        return (void *)&g_opengl_handle;
    }
    if (!host_path) {
        rt_set_dlerror("%s is not on this system", path);
        if (g_host->verbose) tl_log("dlopen %s: not on this system", path);
        return NULL;
    }
    void *h = dlopen(host_path, mode);
    if (!h && g_host->verbose) tl_log("dlopen %s: %s", path, dlerror());
    return h;
}

static int rt_dlclose(void *handle) {
    if (as_image(handle) || handle == (void *)&g_opengl_handle) return 0;
    return dlclose(handle);
}

static uint64_t image_lookup_deep(TlImage *im, const char *sym, int *found) {
    uint64_t v = image_export(im, sym, found);
    if (*found) return v;
    for (int i = 0; i < im->ndeps; i++) {
        v = resolve_in_dep(im, i + 1, sym, found);
        if (*found) return v;
    }
    return 0;
}

static void *rt_dlsym(void *handle, const char *name) {
    char sym[1024];
    snprintf(sym, sizeof sym, "_%s", name);
    int found = 0;
    if (handle == (void *)&g_opengl_handle) {
        void *s = g_host->shim_lookup(g_opengl_name, sym);
        if (!s) rt_set_dlerror("%s: symbol not found", name);
        return s;
    }
    TlImage *im = as_image(handle);
    if (im) {
        uint64_t v = image_lookup_deep(im, sym, &found);
        if (found) return (void *)v;
        rt_set_dlerror("%s: symbol not found", name);
        return NULL;
    }
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT || handle == RTLD_SELF || handle == RTLD_MAIN_ONLY) {
        int ns = ns_of(__builtin_return_address(0));
        int n = g_norder;
        for (int i = 0; i < n; i++) {
            if (g_order[i]->ns != ns) continue;
            uint64_t v = image_export(g_order[i], sym, &found);
            if (found) return (void *)v;
        }
        if (handle == RTLD_MAIN_ONLY) return NULL;
        if (g_host->shim_lookup) {
            void *s = g_host->shim_lookup("", sym);
            if (s) return s;
        }
        if (g_host->host_lacks && g_host->host_lacks("", sym)) {
            rt_set_dlerror("%s: symbol not found", name);
            return NULL;
        }
        /* A symbol of the app's own code in another process (Steam's web helper asks for its launcher's
           GetBootstrapperVersion): that process is not this one, and the system has none. */
        for (int i = 0; i < n; i++) {
            if (g_order[i]->ns == ns) continue;
            image_export(g_order[i], sym, &found);
            if (found) {
                rt_set_dlerror("%s: symbol not found", name);
                return NULL;
            }
        }
        if (getenv("TL_DLSYM_TRACE")) tl_log("dlsym(RTLD_DEFAULT, %s) from namespace %d: the system's", name, ns);
        double t0 = getenv("TL_DLSYM_TRACE") ? (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
        void *v = dlsym(RTLD_DEFAULT, name);
        if (t0) tl_log("  ... %s in %.1f ms", v ? "found" : "not found", ((double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
        return v;
    }
    return dlsym(handle, name);
}

static int rt_dladdr(const void *addr, Dl_info *info) {
    TlImage *im = tl_image_containing(addr);
    if (!im) return dladdr(addr, info);
    info->dli_fname = im->path;
    info->dli_fbase = im->base;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

static int rt_NSGetExecutablePath(char *buf, uint32_t *size) {
    TlImage *exe = main_of(ns_of(__builtin_return_address(0)));
    if (!exe) return _NSGetExecutablePath(buf, size);
    uint32_t need = (uint32_t)strlen(exe->path) + 1;
    if (*size < need) { *size = need; return -1; }
    memcpy(buf, exe->path, need);
    return 0;
}

static const struct mach_header *rt_NSGetMachExecuteHeader(void) {
    TlImage *exe = main_of(ns_of(__builtin_return_address(0)));
    return exe ? (const void *)exe->base : (const void *)_dyld_get_image_header(0);
}

/* The image list: the program in place of the host's main executable, then the host's libraries, then the
   program's own libraries in load order. */
static uint32_t rt_dyld_image_count(void) {
    int guests = g_norder;
    return _dyld_image_count() + (uint32_t)(guests > 0 ? guests - 1 : 0);
}

static TlImage *guest_at(uint32_t i) {
    uint32_t host = _dyld_image_count();
    if (i == 0) return g_main;
    if (i < host) return NULL;
    uint32_t k = i - host + 1; /* g_order[0] is the main program */
    return k < (uint32_t)g_norder ? g_order[k] : NULL;
}

static const struct mach_header *rt_dyld_get_image_header(uint32_t i) {
    TlImage *im = guest_at(i);
    if (im) return (const void *)im->base;
    return i < _dyld_image_count() ? _dyld_get_image_header(i) : NULL;
}

static const char *rt_dyld_get_image_name(uint32_t i) {
    TlImage *im = guest_at(i);
    if (im) return im->path;
    return i < _dyld_image_count() ? _dyld_get_image_name(i) : NULL;
}

static intptr_t rt_dyld_get_image_vmaddr_slide(uint32_t i) {
    TlImage *im = guest_at(i);
    if (im) return (intptr_t)im->slide;
    return i < _dyld_image_count() ? _dyld_get_image_vmaddr_slide(i) : 0;
}

static void rt_dyld_register_func_for_add_image(void (*fn)(const struct mach_header *, intptr_t)) {
    _dyld_register_func_for_add_image(fn);
    int n = g_norder;
    for (int i = 0; i < n; i++) fn((const void *)g_order[i]->base, (intptr_t)g_order[i]->slide);
}

static void rt_dyld_register_func_for_remove_image(void (*fn)(const struct mach_header *, intptr_t)) {
    _dyld_register_func_for_remove_image(fn);
}

/* ---- processes as threads ---- */

/* bootstrap_look_up is in libSystem on iOS too, without its header. */
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
#include <sys/wait.h>
#include <signal.h>


static int g_next_ns = 1;
static pthread_mutex_t g_proc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_proc_cond = PTHREAD_COND_INITIALIZER;
/* Started processes that looked up their parent's rendezvous name, in order: the parent's server takes them in the
   same order as their requests arrive. */
static pid_t g_rendezvous_fifo[64];
static int g_rendezvous_head, g_rendezvous_tail;
static pthread_mutex_t g_rendezvous_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pids of processes started here: 0x70000000 + a per-run number * 256 + the namespace. The per-run part keeps names
   programs derive from pids (named semaphores, sockets, shared memory, which outlive a crashed run) apart. */
static uint32_t pid_salt(void) {
    static uint32_t salt;
    if (!salt) salt = 1 + (uint32_t)((getpid() * 2654435761u ^ (uint32_t)time(NULL)) % 0xFFFF);
    return salt;
}
static pid_t fake_pid(int ns) { return (pid_t)(0x70000000 | (pid_salt() << 8) | (uint32_t)ns); }
static TlProc *proc_of_pid(pid_t pid) {
    if ((pid & 0x7F000000) != 0x70000000 || (((uint32_t)pid >> 8) & 0xFFFF) != pid_salt()) return NULL;
    int ns = pid & 0xFF;
    return ns > 0 && ns < TL_MAX_NS ? g_procs[ns] : NULL;
}
static pid_t pid_of_ns(int ns) { return ns > 0 ? fake_pid(ns) : getpid(); }

static char **copy_strings(char *const *v, int *count) {
    int n = 0;
    while (v && v[n]) n++;
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) out[i] = strdup(v[i]);
    if (count) *count = n;
    return out;
}

/* A program of the app's own: one we start as a process here. Inside its bundle (or beside a bare program), or a
   Mach-O in the app's home (Steam's client, which its launcher installs in Library/Application Support). */
static int is_guest_program(const char *path) {
    if (!path || is_system_path(path) || !strncmp(path, "/usr/", 5) || !strncmp(path, "/bin/", 5) || !strncmp(path, "/sbin/", 6) ||
        !strncmp(path, "/Library/Apple/", 15) || !strncmp(path, "/System/", 8))
        return 0;
    TlImage *exe = main_of(0);
    if (!exe) return 0;
    const char *app = strstr(exe->path, ".app/");
    size_t root = app ? (size_t)(app - exe->path + 5) : strlen(exe->dir) + 1;
    if (!strncmp(path, exe->path, root)) return 1;
    char home[PATH_MAX], real[PATH_MAX];
    if (!getenv("HOME") || !realpath(getenv("HOME"), home) || !realpath(path, real)) return 0;
    size_t h = strlen(home);
    if (strncmp(real, home, h) || real[h] != '/') return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint32_t magic = 0;
    ssize_t n = read(fd, &magic, sizeof magic);
    close(fd);
    return n == sizeof magic && (magic == MH_MAGIC_64 || magic == FAT_CIGAM || magic == FAT_MAGIC);
}

static void proc_finish(TlProc *p, int status) {
    pthread_mutex_lock(&g_proc_lock);
    if (p->done) { /* already ended for its parent (SIGKILL): its thread has now ended too */
        pthread_mutex_unlock(&g_proc_lock);
        return;
    }
    p->status = status;
    p->done = 1;
    int n = p->nexit_watchers;
    p->nexit_watchers = 0;
    pthread_cond_broadcast(&g_proc_cond);
    pthread_mutex_unlock(&g_proc_lock);
    for (int i = 0; i < n; i++) {
        dispatch_source_merge_data((dispatch_source_t)p->exit_watchers[i], DISPATCH_PROC_EXIT);
        dispatch_release((dispatch_source_t)p->exit_watchers[i]);
    }
    if (!getenv("TL_NO_SIGCHLD")) kill(getpid(), SIGCHLD); /* as the kernel tells a parent (libuv reaps its children on it) */
}

/* A process started here has no kernel pid: the kernel would report a watch on it as "already exited" at once (so
   Chrome's rendezvous server dropped a child's ports before the child asked for them). The watch becomes a source
   this loader fires when the process ends. */
static dispatch_source_t rt_dispatch_source_create(dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
                                                   dispatch_queue_t queue) {
    if (type == DISPATCH_SOURCE_TYPE_PROC && handle <= 0x7FFFFFFF && proc_of_pid((pid_t)handle)) {
        TlProc *p = proc_of_pid((pid_t)handle);
        if (p) {
            dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
            pthread_mutex_lock(&g_proc_lock);
            if (p->done)
                dispatch_source_merge_data(s, DISPATCH_PROC_EXIT & mask);
            else if (p->nexit_watchers < 16) {
                dispatch_retain(s);
                p->exit_watchers[p->nexit_watchers++] = s;
            }
            pthread_mutex_unlock(&g_proc_lock);
            return s;
        }
    }
    return dispatch_source_create(type, handle, mask, queue);
}

static void *proc_thread(void *arg) {
    TlProc *p = arg;
    const char *base = strrchr(p->exe->path, '/');
    char name[64];
    snprintf(name, sizeof name, "proc %d %s", p->ns, base ? base + 1 : p->exe->path);
    pthread_setname_np(name);
    tl_log("process %d (pid %#x) runs %s", p->ns, p->pid, p->exe->path);
    int rc = tl_run_main(p->exe, p->argc, (const char **)p->argv, (const char **)p->envp);
    tl_log("process %d returned %d", p->ns, rc);
    proc_finish(p, rc & 0xFF);
    return NULL;
}

/* Loads path as a new process (namespace) started by the namespace parent_ns. NULL with errno set. */
static TlProc *proc_new(int parent_ns, char *const argv[], char *const envp[]) {
    pthread_mutex_lock(&g_proc_lock);
    int ns = g_next_ns < TL_MAX_NS ? g_next_ns++ : 0;
    pthread_mutex_unlock(&g_proc_lock);
    if (!ns) { errno = EAGAIN; return NULL; }
    TlProc *p = calloc(1, sizeof *p);
    p->ns = ns, p->parent_ns = parent_ns, p->pid = fake_pid(ns);
    p->argv = copy_strings(argv, &p->argc);
    p->envp = copy_strings(envp, NULL);
    g_procs[ns] = p; /* before loading: its initializers and _NSGetArgv see its own arguments */
    return p;
}

static TlProc *proc_load(const char *path, int parent_ns, char *const argv[], char *const envp[]) {
    TlProc *p = proc_new(parent_ns, argv, envp);
    if (!p) return NULL;
    int ns = p->ns;
    char err[1024] = "";
    TlImage *exe = load_library_in(path, ns, err, sizeof err);
    if (!exe) {
        tl_log("process %d: %s", ns, err);
        g_procs[ns] = NULL;
        errno = ENOEXEC;
        return NULL;
    }
    p->exe = exe;
    if (g_host->process_started) g_host->process_started(ns, path);
    return p;
}

/* A Chromium helper's switches for running inside the app: its sandbox policy would come through an inherited file
   descriptor (--seatbelt-client=N), which an in-app child does not get, and iOS sandboxes the whole app anyway, so
   it runs with --no-sandbox instead. Other programs' arguments are passed as they are. */
static char **child_argv(char *const argv[]) {
    int n = 0, helper = 0, unsandboxed = 0;
    while (argv && argv[n]) {
        if (!strncmp(argv[n], "--type=", 7)) helper = 1;
        if (!strcmp(argv[n], "--no-sandbox")) unsandboxed = 1;
        n++;
    }
    char **out = calloc((size_t)n + 2, sizeof *out);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!helper || strncmp(argv[i], "--seatbelt-client", 17)) out[k++] = argv[i];
    if (helper && !unsandboxed) out[k++] = "--no-sandbox";
    return out;
}

static int spawn_in_process(pid_t *pid, const char *path, int parent_ns, char *const argv_in[], char *const envp[]) {
    char **argv = child_argv(argv_in);
    TlProc *p = proc_load(path, parent_ns, argv, envp);
    free(argv); /* proc_new copies the strings */
    if (!p) return errno ? errno : ENOEXEC;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20); /* a process's main thread gets a big stack */
    int rc = pthread_create(&p->thread, &attr, proc_thread, p);
    pthread_attr_destroy(&attr);
    if (rc) return rc;
    if (pid) *pid = p->pid;
    return 0;
}

/* malloc zones in a started process: the program's allocator zones are already installed by the program, and a
   second copy of its allocator must not install its own (one malloc per process): a copy sees none of the program's
   zones of those names, and its zone (un)registration does nothing. Its code then mallocs through the program's. */
#include <malloc/malloc.h>
static int zone_is_programs(malloc_zone_t *z) {
    const char *n = z ? malloc_get_zone_name(z) : NULL;
    return n && (!strcmp(n, "PartitionAlloc") || !strcmp(n, "DelegatingDefaultZoneForPartitionAlloc"));
}
/* Each started process sees the zones it registered first, in its own order (the allocator checks its zone came
   out first), then the real ones without the program's; nothing it does reaches the real zone list. */
static malloc_zone_t *g_ns_zones[TL_MAX_NS][8];
static int g_ns_nzones[TL_MAX_NS];
static void ns_zone_remove(int ns, malloc_zone_t *z) {
    int n = g_ns_nzones[ns], k = 0;
    for (int i = 0; i < n; i++)
        if (g_ns_zones[ns][i] != z) g_ns_zones[ns][k++] = g_ns_zones[ns][i];
    g_ns_nzones[ns] = k;
}
static kern_return_t rt_malloc_get_all_zones(task_t task, memory_reader_t reader, vm_address_t **addresses, unsigned *count) {
    kern_return_t kr = malloc_get_all_zones(task, reader, addresses, count);
    int ns = ns_of(__builtin_return_address(0));
    if (kr || !ns || ns >= TL_MAX_NS || !addresses || !*addresses) return kr;
    static __thread vm_address_t view[80];
    unsigned n = 0;
    for (int i = 0; i < g_ns_nzones[ns] && n < 80; i++) view[n++] = (vm_address_t)g_ns_zones[ns][i];
    for (unsigned i = 0; i < *count && n < 80; i++) {
        malloc_zone_t *z = (malloc_zone_t *)(*addresses)[i];
        int mine = 0;
        for (int j = 0; j < g_ns_nzones[ns]; j++) mine |= g_ns_zones[ns][j] == z;
        if (!mine && !zone_is_programs(z)) view[n++] = (*addresses)[i];
    }
    *addresses = view;
    *count = n;
    return kr;
}
static void rt_malloc_zone_register(malloc_zone_t *zone) {
    int ns = ns_of(__builtin_return_address(0));
    if (!ns || ns >= TL_MAX_NS) { malloc_zone_register(zone); return; }
    ns_zone_remove(ns, zone);
    if (g_ns_nzones[ns] < 8) g_ns_zones[ns][g_ns_nzones[ns]++] = zone;
}
static void rt_malloc_zone_unregister(malloc_zone_t *zone) {
    int ns = ns_of(__builtin_return_address(0));
    if (!ns || ns >= TL_MAX_NS) { malloc_zone_unregister(zone); return; }
    ns_zone_remove(ns, zone);
}

/* Which programs start as processes here: Chromium's child processes (--type=renderer, utility, gpu-process...).
   Anything else a program starts (a notification helper app, an updater) is refused as on iOS. */
static int wanted_process(char *const argv[]) {
    if (getenv("TL_ALL_PROCESSES")) return 1; /* a test switch */
    /* Each started process holds its own copy of the program's code (Chrome's is 245 MB): only the helpers a
       window needs. Renderers and the GPU process draw; of Chromium's utility services, storage (kept out of the
       browser unless it runs as one process), network and audio. Others (unzipping, macOS notifications, ...) are
       refused as on iOS, and Chromium copes. */
    static const char *const utilities[] = {"storage.mojom.StorageService", "network.mojom.NetworkService",
                                            "audio.mojom.AudioService", "node.mojom.NodeService" /* Electron's */, NULL};
    int utility = 0, helper = 0;
    const char *sub = NULL;
    const char *base = argv && argv[0] ? strrchr(argv[0], '/') : NULL;
    base = base ? base + 1 : argv && argv[0] ? argv[0] : "";
    if (strstr(base, "crashpad") || strstr(base, "crash_handler") || strstr(base, "crashhandler"))
        return 0; /* crash reporters watch the process from outside: in it they would catch its signals */
    static const char *const updaters[] = {"ShipIt", "Autoupdate", "Updater", "updater", "Squirrel", "Sparkle", "Installer",
                                           "installer", NULL};
    for (int i = 0; updaters[i]; i++)
        if (strstr(base, updaters[i])) return 0; /* an app's updater replaces the app's files: never from here */
    for (int i = 1; argv && argv[i]; i++) {
        if (!strncmp(argv[i], "--type=", 7)) helper = 1;
        if (!strcmp(argv[i], "--type=renderer") || !strcmp(argv[i], "--type=gpu-process")) return 1;
        if (!strcmp(argv[i], "--type=utility")) utility = 1;
        if (!strncmp(argv[i], "--utility-sub-type=", 19)) sub = argv[i] + 19;
    }
    if (utility && sub)
        for (int i = 0; utilities[i]; i++)
            if (!strcmp(sub, utilities[i])) return 1;
    return !helper; /* a program of the app's own (Steam's web helper), not a Chromium helper */
}

/* A started process's own command line and environment (its libraries read them through _NSGetArgv). */
static int *rt__NSGetArgc(void) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *p = ns > 0 && ns < TL_MAX_NS ? g_procs[ns] : NULL;
    return p ? &p->argc : g_main_argv ? &g_main_argc : _NSGetArgc();
}
static char ***rt__NSGetArgv(void) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *p = ns > 0 && ns < TL_MAX_NS ? g_procs[ns] : NULL;
    return p ? &p->argv : g_main_argv ? &g_main_argv : _NSGetArgv();
}
static char ***rt__NSGetEnviron(void) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *p = ns > 0 && ns < TL_MAX_NS ? g_procs[ns] : NULL;
    return p ? &p->envp : _NSGetEnviron();
}

/* Seatbelt: a program sandboxing "itself" here would sandbox the whole app (and Henge); iOS apps are sandboxed
   already and cannot change it. Every call succeeds and does nothing. */
static int rt_sandbox_ok(void) {
    static int said;
    if (!said++) tl_log("the program asked for a sandbox (ignored: the app is iOS's sandbox already)");
    return 0;
}

static pid_t rt_getpid(void) { return pid_of_ns(ns_of(__builtin_return_address(0))); }
static pid_t rt_getppid(void) {
    int ns = ns_of(__builtin_return_address(0));
    if (!ns) return getppid();
    TlProc *p = g_procs[ns];
    return p ? pid_of_ns(p->parent_ns) : getpid();
}

static int rt_kill(pid_t pid, int sig) {
    TlProc *p = proc_of_pid(pid);
    if (!p) return kill(pid, sig);
    if (p->done) { errno = ESRCH; return -1; }
    if (sig) {
        p->kill_requested = sig; /* a thread cannot be killed; it is left to end on its own */
        tl_log("process %d asked to stop (signal %d)", p->ns, sig);
        /* SIGKILL cannot be ignored: for its parent (waiting in waitpid, as Chromium does after it) the process has
           ended, as the kernel would make it; its thread finishes when it can. */
        if (sig == SIGKILL) proc_finish(p, 128 + SIGKILL);
    }
    return 0;
}

/* TL_IPC_TRACE=1 (a test switch): named semaphores and Unix socket paths a program uses, with its namespace. */
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/un.h>
static sem_t *rt_sem_open(const char *name, int oflag, ...) {
    unsigned mode = 0, value = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, unsigned);
        value = va_arg(ap, unsigned);
        va_end(ap);
    }
    sem_t *s = (oflag & O_CREAT) ? sem_open(name, oflag, mode, value) : sem_open(name, oflag);
    tl_log("sem_open %s (flags %#x, value %u) from namespace %d: %s", name, oflag, value, ns_of(__builtin_return_address(0)),
           s == SEM_FAILED ? strerror(errno) : "ok");
    return s;
}
static int log_sockaddr(const char *what, int fd, const struct sockaddr *a, socklen_t len, int rc, void *ret) {
    if (a && a->sa_family == AF_UNIX)
        tl_log("%s fd %d %s from namespace %d: %s", what, fd, ((const struct sockaddr_un *)a)->sun_path, ns_of(ret),
               rc ? strerror(errno) : "ok");
    return rc;
}
static int rt_connect(int fd, const struct sockaddr *a, socklen_t len) {
    return log_sockaddr("connect", fd, a, len, connect(fd, a, len), __builtin_return_address(0));
}
static int rt_bind(int fd, const struct sockaddr *a, socklen_t len) {
    return log_sockaddr("bind", fd, a, len, bind(fd, a, len), __builtin_return_address(0));
}

/* launchd's job submission (launch_msg, which Steam uses to start its ipcserver): never the system's launchd (an iOS
   app has none to reach, and a Mac runner must not leave jobs behind). A submitted job whose program is the app's own
   starts as a process inside the app, where its bootstrap_check_in lands in the in-app name table; removing a job
   succeeds; anything else fails. */
/* liblaunch's data calls, found at run time (iOS's SDK has no header for them). */
typedef struct _launch_data *launch_data_t;
enum { TL_LD_DICT = 1, TL_LD_ARRAY = 2, TL_LD_ERRNO = 9 };
#define LD(ret, name, args) static ret(*name) args
LD(int, ld_type, (launch_data_t));
LD(launch_data_t, ld_lookup, (launch_data_t, const char *));
LD(size_t, ld_count, (launch_data_t));
LD(launch_data_t, ld_index, (launch_data_t, size_t));
LD(const char *, ld_string, (launch_data_t));
LD(launch_data_t, ld_alloc, (int));
LD(int, ld_set_errno, (launch_data_t, int));
static int ld_bind(void) {
    if (ld_type) return 1;
    ld_lookup = dlsym(RTLD_DEFAULT, "launch_data_dict_lookup");
    ld_count = dlsym(RTLD_DEFAULT, "launch_data_array_get_count");
    ld_index = dlsym(RTLD_DEFAULT, "launch_data_array_get_index");
    ld_string = dlsym(RTLD_DEFAULT, "launch_data_get_string");
    ld_alloc = dlsym(RTLD_DEFAULT, "launch_data_alloc");
    ld_set_errno = dlsym(RTLD_DEFAULT, "launch_data_set_errno");
    ld_type = dlsym(RTLD_DEFAULT, "launch_data_get_type");
    return ld_type && ld_lookup && ld_count && ld_index && ld_string && ld_alloc && ld_set_errno;
}
static launch_data_t ld_errno(int e) {
    launch_data_t d = ld_alloc(TL_LD_ERRNO);
    if (d) ld_set_errno(d, e);
    return d;
}
static launch_data_t rt_launch_msg(launch_data_t msg) {
    int ns = ns_of(__builtin_return_address(0));
    if (!ld_bind() || !msg || ld_type(msg) != TL_LD_DICT) {
        errno = ENOSYS;
        return NULL;
    }
    launch_data_t job = ld_lookup(msg, "SubmitJob");
    if (job && ld_type(job) == TL_LD_DICT) {
        launch_data_t label = ld_lookup(job, "Label"), program = ld_lookup(job, "Program");
        launch_data_t args = ld_lookup(job, "ProgramArguments");
        char *argv[32] = {0};
        size_t n = args && ld_type(args) == TL_LD_ARRAY ? ld_count(args) : 0;
        for (size_t i = 0; i < n && i < 31; i++) argv[i] = (char *)ld_string(ld_index(args, i));
        const char *path = program ? ld_string(program) : argv[0];
        if (!argv[0]) argv[0] = (char *)path;
        tl_log("launchd job %s: %s", label ? ld_string(label) : "?", path ? path : "?");
        pid_t pid = 0;
        extern char **environ;
        if (path && g_host->processes_as_threads && is_guest_program(path) && wanted_process(argv) &&
            !spawn_in_process(&pid, path, ns, argv, environ))
            return ld_errno(0);
        tl_log("  not started (not the app's own program)");
        return ld_errno(EPERM);
    }
    if (ld_lookup(msg, "RemoveJob")) return ld_errno(0);
    errno = ENOSYS;
    return NULL;
}

/* waitid on a process started here (Steam's tier0 asks whether its web helper still runs with WNOHANG|WNOWAIT). */
static int rt_waitid(idtype_t idtype, id_t id, siginfo_t *info, int options) {
    TlProc *p = idtype == P_PID ? proc_of_pid((pid_t)id) : NULL;
    if (!p) return waitid(idtype, id, info, options);
    pthread_mutex_lock(&g_proc_lock);
    while (!p->done && !(options & WNOHANG)) pthread_cond_wait(&g_proc_cond, &g_proc_lock);
    if (info) memset(info, 0, sizeof *info);
    if (p->done) {
        if (info) info->si_signo = SIGCHLD, info->si_code = CLD_EXITED, info->si_pid = p->pid, info->si_status = p->status;
        if (!(options & WNOWAIT)) p->done = 2;
    }
    pthread_mutex_unlock(&g_proc_lock);
    return 0;
}

static pid_t rt_waitpid(pid_t pid, int *status, int options) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *only = pid > 0 ? proc_of_pid(pid) : NULL;
    if (pid > 0 && !only) return waitpid(pid, status, options);
    pthread_mutex_lock(&g_proc_lock);
    for (;;) {
        int children = 0;
        for (int i = 1; i < TL_MAX_NS; i++) {
            TlProc *p = g_procs[i];
            if (!p || (only && p != only) || (!only && p->parent_ns != ns)) continue;
            children++;
            if (p->done && p->done != 2) {
                p->done = 2; /* reaped */
                if (status) *status = W_EXITCODE(p->status, 0);
                pid_t r = p->pid;
                pthread_mutex_unlock(&g_proc_lock);
                return r;
            }
        }
        if (!children) {
            pthread_mutex_unlock(&g_proc_lock);
            if (!only) return waitpid(pid, status, options); /* real children, if any */
            errno = ECHILD;
            return -1;
        }
        if (options & WNOHANG) {
            pthread_mutex_unlock(&g_proc_lock);
            return 0;
        }
        pthread_cond_wait(&g_proc_cond, &g_proc_lock);
    }
}

/* exit in a started process ends its thread, not the app. */
static void rt_exit(int code) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *p = ns ? g_procs[ns] : NULL;
    if (!p) exit(code);
    tl_log("process %d called exit(%d)", ns, code);
    fflush(NULL);
    proc_finish(p, code & 0xFF);
    pthread_exit(NULL);
}
static void rt__exit(int code) {
    int ns = ns_of(__builtin_return_address(0));
    TlProc *p = ns ? g_procs[ns] : NULL;
    if (!p) _exit(code);
    proc_finish(p, code & 0xFF);
    pthread_exit(NULL);
}

/* Mach bootstrap names within the app: a server checking in a name and a client looking it up both live here. */
typedef struct TlBootName { char name[128]; mach_port_t port; } TlBootName;
static TlBootName g_boot_names[64];
static int g_nboot;
static pthread_mutex_t g_boot_lock = PTHREAD_MUTEX_INITIALIZER;

static mach_port_t boot_find(const char *name) {
    for (int i = 0; i < g_nboot; i++)
        if (!strcmp(g_boot_names[i].name, name)) return g_boot_names[i].port;
    return MACH_PORT_NULL;
}

static kern_return_t rt_bootstrap_check_in(mach_port_t bp, const char *name, mach_port_t *sp) {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr) return kr;
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND); /* the table's send right */
    pthread_mutex_lock(&g_boot_lock);
    if (g_nboot < 64) {
        snprintf(g_boot_names[g_nboot].name, sizeof g_boot_names[0].name, "%s", name);
        g_boot_names[g_nboot++].port = port;
    }
    pthread_mutex_unlock(&g_boot_lock);
    if (g_host->verbose || getenv("TL_BOOT_TRACE")) tl_log("bootstrap name %s checked in here", name);
    *sp = port;
    return KERN_SUCCESS;
}

static kern_return_t rt_bootstrap_register(mach_port_t bp, const char *name, mach_port_t sp) {
    mach_port_mod_refs(mach_task_self(), sp, MACH_PORT_RIGHT_SEND, 1);
    pthread_mutex_lock(&g_boot_lock);
    if (g_nboot < 64) {
        snprintf(g_boot_names[g_nboot].name, sizeof g_boot_names[0].name, "%s", name);
        g_boot_names[g_nboot++].port = sp;
    }
    pthread_mutex_unlock(&g_boot_lock);
    return KERN_SUCCESS;
}

static kern_return_t rt_bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp) {
    pthread_mutex_lock(&g_boot_lock);
    mach_port_t port = boot_find(name);
    pthread_mutex_unlock(&g_boot_lock);
    if (getenv("TL_BOOT_TRACE")) tl_log("bootstrap_look_up %s (namespace %d): %s", name, ns_of(__builtin_return_address(0)), port ? "here" : "system");
    if (port) {
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
        if (strstr(name, "MachPortRendezvousServer")) {
            pthread_mutex_lock(&g_rendezvous_lock);
            g_rendezvous_fifo[g_rendezvous_tail++ % 64] = pid_of_ns(ns_of(__builtin_return_address(0)));
            pthread_mutex_unlock(&g_rendezvous_lock);
        }
        *sp = port;
        return KERN_SUCCESS;
    }
    /* iOS's sandbox lets an app reach Apple's services, not other programs' (a Mac runner shaped like iOS must not
       reach the Mac's own apps either: a second Steam would hand over to the user's). */
    if (g_host->ios_kernel_rules && strncmp(name, "com.apple.", 10)) {
        if (g_host->verbose || getenv("TL_BOOT_TRACE")) tl_log("bootstrap_look_up %s refused (not Apple's)", name);
        return 1102; /* BOOTSTRAP_UNKNOWN_SERVICE */
    }
    return bootstrap_look_up(bp, name, sp);
}

/* A message from this process names this process as its sender; a parent's rendezvous server asks which started
   process it is: the one that just looked it up. */
static pid_t rt_audit_token_to_pid(audit_token_t token) {
    pid_t pid = (pid_t)token.val[5]; /* audit_token_to_pid's own reading (libbsm, which this need not link) */
    if (pid != getpid()) return pid;
    pthread_mutex_lock(&g_rendezvous_lock);
    if (g_rendezvous_head < g_rendezvous_tail) pid = g_rendezvous_fifo[g_rendezvous_head++ % 64];
    pthread_mutex_unlock(&g_rendezvous_lock);
    if (getenv("TL_BOOT_TRACE")) tl_log("audit_token_to_pid -> %#x", pid);
    return pid;
}

/* A program's crash reporter (Breakpad, Crashpad) taking the task's exception ports would take every thread's
   faults, Henge's included, and freeze the app writing its dump. The program's own threads may set theirs. */
static kern_return_t rt_task_set_exception_ports(mach_port_t task, unsigned mask, mach_port_t port, int behavior, int flavor) {
    static int said;
    if (!said++) tl_log("the program asked for the app's exception ports (ignored)");
    return KERN_SUCCESS;
}
static kern_return_t rt_task_swap_exception_ports(mach_port_t task, unsigned mask, mach_port_t port, int behavior, int flavor,
                                                  unsigned *masks, unsigned *count, mach_port_t *ports, int *behaviors,
                                                  int *flavors) {
    if (count) *count = 0;
    return rt_task_set_exception_ports(task, mask, port, behavior, flavor);
}

static int rt_refuse(void) {
    errno = EPERM;
    return -1;
}

/* What a program's spawn file actions make of the child's standard output and error (dup2 onto 1 and 2), for the
   system tools answered here. */
typedef struct { const void *fa; int to[3]; } TlSpawnFds;
static TlSpawnFds g_spawn_fds[32];
static pthread_mutex_t g_spawn_fds_lock = PTHREAD_MUTEX_INITIALIZER;
static TlSpawnFds *spawn_fds(const void *fa, int create) {
    TlSpawnFds *free_slot = NULL;
    for (int i = 0; i < 32; i++) {
        if (g_spawn_fds[i].fa == fa) return &g_spawn_fds[i];
        if (!g_spawn_fds[i].fa && !free_slot) free_slot = &g_spawn_fds[i];
    }
    if (create && free_slot) *free_slot = (TlSpawnFds){fa, {-1, -1, -1}};
    return create ? free_slot : NULL;
}
static int rt_posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
    if (newfd >= 0 && newfd < 3) {
        pthread_mutex_lock(&g_spawn_fds_lock);
        TlSpawnFds *f = spawn_fds(fa ? *fa : NULL, 1);
        if (f) f->to[newfd] = fd;
        pthread_mutex_unlock(&g_spawn_fds_lock);
    }
    return posix_spawn_file_actions_adddup2(fa, fd, newfd);
}
static int rt_posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    pthread_mutex_lock(&g_spawn_fds_lock);
    TlSpawnFds *f = spawn_fds(fa ? *fa : NULL, 0);
    if (f) f->fa = NULL;
    pthread_mutex_unlock(&g_spawn_fds_lock);
    return posix_spawn_file_actions_destroy(fa);
}

/* System tools a program runs for a line of facts, answered here (iOS runs no tools): sw_vers. The output goes to
   where the program pointed the tool's standard output; the "process" has ended by the time it is returned. */
/* The output of a system tool answered here (sw_vers, uname; "sh -c 'uname -m'" counts as uname), or -1. */
static int builtin_output(const char *path, char *const argv[], char *out, size_t outlen) {
    if (!path) return -1;
    char version[64] = "15.0", build[64] = "24A335", release[64] = "24.0.0";
    size_t n = sizeof version;
    sysctlbyname("kern.osproductversion", version, &n, NULL, 0);
    n = sizeof build;
    sysctlbyname("kern.osversion", build, &n, NULL, 0);
    n = sizeof release;
    sysctlbyname("kern.osrelease", release, &n, NULL, 0);
    /* A shell running one of these tools ("sh -c 'uname -m'") is answered as the tool. */
    char cmd[128] = "";
    const char *tool = path, *arg = argv && argv[0] && argv[1] ? argv[1] : "";
    if ((!strcmp(path, "/bin/sh") || !strcmp(path, "/bin/bash") || !strcmp(path, "/bin/zsh")) && !strcmp(arg, "-c") && argv[2]) {
        snprintf(cmd, sizeof cmd, "%s", argv[2]);
        char *sp = strchr(cmd, ' ');
        arg = sp ? (*sp = 0, sp + 1) : "";
        tool = cmd;
    }
    if (!strcmp(tool, "/usr/bin/sw_vers") || !strcmp(tool, "sw_vers")) {
        if (!strcmp(arg, "-productVersion") || !strcmp(arg, "--productVersion")) snprintf(out, outlen, "%s\n", version);
        else if (!strcmp(arg, "-buildVersion") || !strcmp(arg, "--buildVersion")) snprintf(out, outlen, "%s\n", build);
        else if (!strcmp(arg, "-productName") || !strcmp(arg, "--productName")) snprintf(out, outlen, "macOS\n");
        else snprintf(out, outlen, "ProductName:\t\tmacOS\nProductVersion:\t\t%s\nBuildVersion:\t\t%s\n", version, build);
    } else if (!strcmp(tool, "/usr/bin/uname") || !strcmp(tool, "uname")) {
        if (!strcmp(arg, "-m") || !strcmp(arg, "-p")) snprintf(out, outlen, "%s\n", !strcmp(arg, "-m") ? "arm64" : "arm");
        else if (!strcmp(arg, "-r")) snprintf(out, outlen, "%s\n", release);
        else if (!*arg || !strcmp(arg, "-s")) snprintf(out, outlen, "Darwin\n");
        else return -1;
    } else
        return -1;
    return 0;
}

static int builtin_tool(pid_t *pid, const char *path, const void *fa, int parent_ns, char *const argv[], char *const envp[]) {
    char out[256];
    if (builtin_output(path, argv, out, sizeof out)) return -1;
    pthread_mutex_lock(&g_spawn_fds_lock);
    TlSpawnFds *f = fa ? spawn_fds(*(const void *const *)fa, 0) : NULL;
    int fd = f && f->to[1] >= 0 ? f->to[1] : -1;
    pthread_mutex_unlock(&g_spawn_fds_lock);
    if (fd >= 0) (void)!write(fd, out, strlen(out));
    TlProc *p = proc_new(parent_ns, argv, envp);
    if (!p) return EAGAIN;
    tl_log("answered %s here", path);
    if (pid) *pid = p->pid;
    proc_finish(p, 0);
    return 0;
}

/* system() and popen() start /bin/sh inside libc, past the spawn hooks: answered here. A command that is one of the
   tools above gets its output; any other is refused, as on iOS (which has no shell). */
int tl_plist_job(const char *path, char **argv, int max); /* tl_bundle.m */
/* "launchctl load [-S X] [-w] PATH": the job's program, if it is the app's own, starts as a process inside the app
   (Steam starts its ipcserver this way); "launchctl remove LABEL" succeeds. Returns -1 for anything else. */
static int launchctl_command(const char *command, int ns) {
    if (strncmp(command, "launchctl ", 10)) return -1;
    if (!strncmp(command + 10, "remove ", 7) || !strncmp(command + 10, "unload ", 7)) return 0;
    if (strncmp(command + 10, "load ", 5)) return -1;
    char path[PATH_MAX] = "";
    const char *q = strchr(command, '\'') ? strchr(command, '\'') : strchr(command, '"');
    if (q) {
        const char *e = strchr(q + 1, *q);
        if (!e) return -1;
        snprintf(path, sizeof path, "%.*s", (int)(e - q - 1), q + 1);
    } else {
        const char *last = strrchr(command, ' ');
        snprintf(path, sizeof path, "%s", last ? last + 1 : "");
    }
    char *argv[32];
    if (tl_plist_job(path, argv, 32) < 1) return -1;
    pid_t pid = 0;
    extern char **environ;
    int rc = g_host->processes_as_threads && is_guest_program(argv[0]) && wanted_process(argv)
                 ? spawn_in_process(&pid, argv[0], ns, argv, environ)
                 : -1;
    tl_log("launchctl load %s: %s", path, rc ? "not started (not the app's own program)" : "started inside the app");
    for (int i = 0; argv[i]; i++) free(argv[i]);
    return rc ? -1 : 0;
}

static int rt_system(const char *command) {
    if (!command) return 0; /* "no shell available" */
    if (!launchctl_command(command, ns_of(__builtin_return_address(0)))) return 0;
    tl_log("the program tried to run a shell command: %.200s (refused, as on iOS)", command);
    errno = EPERM;
    return -1;
}
static FILE *g_popen_files[16];
static FILE *rt_popen(const char *command, const char *mode) {
    char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
    char out[256];
    if (command && mode && mode[0] == 'r' && !builtin_output("/bin/sh", argv, out, sizeof out)) {
        int fds[2];
        if (!pipe(fds)) {
            (void)!write(fds[1], out, strlen(out));
            close(fds[1]);
            FILE *f = fdopen(fds[0], "r");
            for (int i = 0; f && i < 16; i++)
                if (!g_popen_files[i]) { g_popen_files[i] = f; break; }
            tl_log("answered %.120s here", command);
            return f;
        }
    }
    tl_log("the program tried to run a shell command: %.200s (refused, as on iOS)", command ? command : "?");
    errno = EPERM;
    return NULL;
}
static int rt_pclose(FILE *f) {
    for (int i = 0; i < 16; i++)
        if (f && g_popen_files[i] == f) {
            g_popen_files[i] = NULL;
            fclose(f);
            return 0;
        }
    return pclose(f);
}

/* For the host's NSTask (Henge's AppKit): a tool answered here writes its output to out_fd and ends at once (0);
   a program of the app's own starts as a process inside (its pid); anything else is refused (-1). */
pid_t tl_task_launch(const char *path, char *const argv[], int out_fd, const void *caller) {
    char out[256];
    int ns = ns_of(caller);
    if (!builtin_output(path, argv, out, sizeof out)) {
        if (out_fd >= 0) (void)!write(out_fd, out, strlen(out));
        tl_log("answered %s here (NSTask)", path);
        return 0;
    }
    pid_t pid = 0;
    extern char **environ;
    if (g_host && g_host->processes_as_threads && is_guest_program(path) && wanted_process(argv) &&
        !spawn_in_process(&pid, path, ns, argv, environ))
        return pid;
    tl_log("the program tried to start another process: %s (NSTask; refused, as on iOS)", path ? path : "?");
    return -1;
}
/* Whether an in-app process has ended, and how (for NSTask). */
int tl_task_status(pid_t pid, int *status) {
    TlProc *p = proc_of_pid(pid);
    if (!p) return 1;
    if (status) *status = p->status;
    return p->done != 0;
}

static int rt_posix_spawn(pid_t *pid, const char *path, const void *fa, const void *attr, char *const argv[], char *const envp[]) {
    (void)attr;
    if (getenv("TL_SPAWN_TRACE")) {
        tl_log("posix_spawn %s", path ? path : "?");
        for (int i = 0; argv && argv[i] && i < 6; i++) tl_log("  argv[%d] %.120s", i, argv[i]);
        tl_log("  guest %d", is_guest_program(path));
        tl_log("  wanted %d", wanted_process(argv));
    }
    if (g_host->processes_as_threads && !getenv("TL_NO_SPAWN_PATH") && is_guest_program(path) && wanted_process(argv))
        return spawn_in_process(pid, path, ns_of(__builtin_return_address(0)), argv, envp);
    if (g_host->processes_as_threads) {
        int rc = builtin_tool(pid, path, fa, ns_of(__builtin_return_address(0)), argv, envp);
        if (rc >= 0) return rc;
    }
    tl_log("the program tried to start another process: %s (refused, as on iOS)", path ? path : "?");
    return EPERM;
}

/* Valve's tier0 (Steam) starts every child through CreateSimpleProcess(command, flags, env, dir): fork, then in the
   child setsid, chdir and exec (flags 8: execv of an argv, 16: execvp, 4: a shell command, none: system()). Fork
   cannot be had here, so the whole call is answered: an argv naming a program of the app's own starts it as a
   process inside the app (the shared working directory stays as it is); anything else is refused, as a failed fork
   (0). */
static pid_t rt_CreateSimpleProcess(void *command, unsigned flags, char **env, const char *dir) {
    if (flags & 0x20) return 0;
    if (flags & (8 | 16)) {
        char **argv = command;
        const char *path = argv && argv[0] ? argv[0] : NULL;
        if (getenv("TL_SPAWN_TRACE")) {
            tl_log("CreateSimpleProcess %s (flags %#x, dir %s)", path ? path : "?", flags, dir ? dir : "-");
            for (int i = 1; argv && argv[i] && i < 8; i++) tl_log("  argv[%d] %.120s", i, argv[i]);
        }
        if (path && g_host->processes_as_threads && is_guest_program(path) && wanted_process(argv)) {
            pid_t pid = 0;
            extern char **environ;
            int rc = spawn_in_process(&pid, path, ns_of(__builtin_return_address(0)), argv, env ? env : environ);
            if (!rc) return pid;
            tl_log("CreateSimpleProcess %s: %s", path, strerror(rc));
            return 0;
        }
        tl_log("the program tried to start another process: %s (refused, as on iOS)", path ? path : "?");
        return 0;
    }
    tl_log("the program tried to run a shell command: %.200s (refused, as on iOS)", command ? (const char *)command : "?");
    return 0;
}

/* Functions of an app's own libraries answered here instead (imports from other libraries only). */
static void *guest_override(const char *install_name, const char *sym) {
    if (!g_host || !g_host->processes_as_threads || !install_name) return NULL;
    size_t n = strlen(install_name);
    if (n >= 16 && !strcmp(install_name + n - 16, "libtier0_s.dylib") && !strcmp(sym, "_CreateSimpleProcess"))
        return (void *)rt_CreateSimpleProcess;
    return NULL;
}

/* exec of the program's own Mach-O: the new program is loaded as a process of its own and runs on this thread; this
   one's code is never returned to. */
static int exec_in(int ns, const char *path, char *const argv[], char *const envp[]) {
    if (g_host->processes_as_threads && is_guest_program(path)) {
        TlProc *p = proc_load(path, ns, argv, envp);
        if (!p) return rt_refuse();
        tl_log("exec %s as process %d on this thread", path, p->ns);
        p->thread = pthread_self();
        int rc = tl_run_main(p->exe, p->argc, (const char **)p->argv, (const char **)p->envp);
        proc_finish(p, rc & 0xFF);
        if (!ns) exit(rc);
        pthread_exit(NULL);
    }
    tl_log("the program tried to exec %s (refused, as on iOS)", path ? path : "?");
    return rt_refuse();
}
/* execl, execlp, execle: their arguments gathered, then as execv/execve. */
static int rt_execl(const char *path, const char *arg0, ...) {
    char *argv[64] = {(char *)arg0};
    va_list ap;
    va_start(ap, arg0);
    for (int i = 1; i < 63 && argv[i - 1]; i++) argv[i] = va_arg(ap, char *);
    va_end(ap);
    extern char **environ;
    return exec_in(ns_of(__builtin_return_address(0)), path, argv, environ);
}
static int rt_execle(const char *path, const char *arg0, ...) {
    char *argv[64] = {(char *)arg0};
    va_list ap;
    va_start(ap, arg0);
    int i = 1;
    for (; i < 63 && argv[i - 1]; i++) argv[i] = va_arg(ap, char *);
    char **envp = argv[i - 1] ? NULL : va_arg(ap, char **);
    va_end(ap);
    return exec_in(ns_of(__builtin_return_address(0)), path, argv, envp);
}

static int rt_execve(const char *path, char *const argv[], char *const envp[]) {
    return exec_in(ns_of(__builtin_return_address(0)), path, argv, envp);
}
extern char **environ;
static int rt_execv(const char *path, char *const argv[]) { /* execv and execvp: the current environment */
    return exec_in(ns_of(__builtin_return_address(0)), path, argv, environ);
}

static pid_t rt_fork(void) {
    tl_log("the program tried to fork (refused, as on iOS)");
    return rt_refuse();
}

/* TL_MAX_MAP_GB=N (a test switch for Mac runs): the program's single mappings and VM allocations larger than N GB
   fail, to find how little address space it needs (iOS gives much less than a Mac). */
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
#endif
static uint64_t max_map_bytes(void) {
    static uint64_t v = (uint64_t)-1;
    if (v == (uint64_t)-1) {
        const char *e = getenv("TL_MAX_MAP_GB");
        v = e ? (uint64_t)atoll(e) << 30 : 0;
    }
    return v;
}
/* What occupies the high address space (0x7000000000 up), every region of 16 MB or more: where the room went. */
static void log_high_space(void) {
    vm_address_t addr = 0x7000000000ull;
    tl_log("the high address space, regions of 16 MB or more:");
    for (int i = 0; i < 4096; i++) {
        vm_size_t size = 0;
        vm_address_t a = addr;
        natural_t depth = 0;
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
        if (size >= ((vm_size_t)16 << 20))
            tl_log("  %#llx-%#llx %.0f MB prot %d/%d tag %u", (unsigned long long)a, (unsigned long long)(a + size),
                   (double)size / (1 << 20), info.protection, info.max_protection, info.user_tag);
        addr = a + size;
    }
}

/* The free stretches of address space of 1 GB or more, logged once a large mapping fails: how much room the
   system leaves a program's allocator (Chromium's reserves 32 GB at once). */
static void log_free_address_space(void) {
    static int said;
    if (said++ >= 3) return;
    vm_address_t addr = 0, prev_end = 0;
    uint64_t total = 0, largest = 0;
    for (int i = 0; i < 100000; i++) {
        vm_size_t size = 0;
        vm_address_t a = addr;
        natural_t depth = 0;
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) break;
        if (a > prev_end && a - prev_end >= ((vm_address_t)1 << 30)) {
            tl_log("  free %#llx-%#llx (%.1f GB)", (unsigned long long)prev_end, (unsigned long long)a, (double)(a - prev_end) / (1 << 30));
            total += a - prev_end;
            if (a - prev_end > largest) largest = a - prev_end;
        }
        if (size >= ((vm_size_t)256 << 20) && a >= 0x7000000000ull)
            tl_log("  used %#llx-%#llx (%.2f GB, prot %d/%d, tag %u)", (unsigned long long)a, (unsigned long long)(a + size),
                   (double)size / (1 << 30), info.protection, info.max_protection, info.user_tag);
        if (a + size > prev_end) prev_end = a + size;
        addr = a + size;
    }
    tl_log("  address space ends at %#llx; free stretches of 1 GB or more: %.1f GB, the largest %.1f GB",
           (unsigned long long)prev_end, (double)total / (1 << 30), (double)largest / (1 << 30));
    /* And what the system actually grants in one piece (room above the last mapping is not listed above). */
    for (int gb = 1024; gb >= 1; gb /= 2) {
        void *p = mmap(NULL, (size_t)gb << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        munmap(p, (size_t)gb << 30);
        tl_log("  the largest single reservation granted: %d GB (at %p)", gb, p);
        break;
    }
}

/* Large reservations in the host's held room (TlHost.high_room: iOS gives an app 64 GB of high address space, and
   Chromium's Mac build asks for more than that at start). Each goes at the alignment its size implies (the largest
   power of two in it, at most 32 GB), first fit from the bottom, mapped over the host's hold with MAP_FIXED. Three
   rules make Chromium fit, all from the fact that each of these blocks is only ever used from the start of its used
   part:
   - the 16 GB block PartitionAlloc reserves right after its 32 GB pools (their metadata) goes over the top half of
     the first pool and the bottom half of the second (as long as neither pool grows past 8 GB, far more than a phone
     holds);
   - a second 32 GB block (Blink's heap, Oilpan, which over-reserves twice its 16 GB cage and uses the upper half)
     gets its address but only its upper half is mapped: the lower half stays for others (V8, the host's JIT view);
   - smaller blocks go where nothing is mapped yet, which may be such an unused lower half. */
typedef struct { uint8_t *at; size_t len; } TlRoomUse;
static TlRoomUse g_room_used[32];
static int g_room_nused;
static uint8_t *g_room_pools; /* where the first 32 GB block went: the next 16 GB block overlaps it */
/* Never-used tops of the big blocks (PartitionAlloc's second pool, Oilpan's cage), for small reservations once the
   held room is full: their owners use them from the bottom, and reach their top half only past 8 GB. */
static TlRoomUse g_room_spare[4];
static int g_room_nspare;
static pthread_mutex_t g_room_lock = PTHREAD_MUTEX_INITIALIZER;

static int room_held(uint8_t *at, size_t len) {
    for (int i = 0; i < g_host->high_rooms; i++) {
        uint8_t *lo = g_host->high_room[i], *hi = lo + g_host->high_room_bytes[i];
        if (at >= lo && at + len <= hi) return 1;
    }
    return 0;
}
static int room_unused(uint8_t *at, size_t len) {
    for (int i = 0; i < g_room_nused; i++)
        if (at < g_room_used[i].at + g_room_used[i].len && g_room_used[i].at < at + len) return 0;
    return 1;
}
static void room_use(uint8_t *at, size_t len) {
    if (g_room_nused < 32) g_room_used[g_room_nused++] = (TlRoomUse){at, len};
}

/* Whether [at, at + len) is still nothing but inaccessible reservation (the host's hold, or one placed here): never
   map over anything else. */
static int room_still_held(uint8_t *at, size_t len) {
    vm_address_t a = (vm_address_t)at;
    while (a < (vm_address_t)(at + len)) {
        vm_address_t r = a;
        vm_size_t size = 0;
        natural_t depth = 0;
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &r, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS)
            return 0;
        if (r > a || info.protection != VM_PROT_NONE) return 0; /* a hole (someone unmapped it) or something in use */
        a = r + size;
    }
    return 1;
}

/* Ranges a program will unmap that it never got: the slack of an over-reservation placed exactly (below). */
static TlRoomUse g_room_slack[16];
static int g_room_nslack;

static void *room_place_prot(size_t len, int prot);
static void *room_place(size_t len) { return room_place_prot(len, PROT_NONE); }

static void *room_place_prot(size_t len, int prot) {
    /* V8 over-reserves its sandbox for alignment, asking size + 4 GB - a page and unmapping what lies outside the
       aligned block: the block alone is placed, and the unmap of the slack after it is ignored (it is not V8's). */
    size_t slack = 0;
    if (len > ((size_t)4 << 30)) {
        size_t core = len + TL_PAGE - ((size_t)4 << 30);
        if (core >= ((size_t)64 << 20) && !(core & (core - 1))) slack = len - core, len = core; /* 4 GB alignment */
    }
    /* The alignment its size implies (the largest power of two in it, at most 32 GB); 4 GB for V8's padded requests. */
    size_t align = TL_PAGE;
    while (align * 2 <= len && align < ((size_t)32 << 30)) align *= 2;
    if (slack && align < ((size_t)4 << 30)) align = (size_t)4 << 30;
    uint8_t *lo = g_host->high_room[0], *hi = lo;
    for (int i = 0; i < g_host->high_rooms; i++) {
        if ((uint8_t *)g_host->high_room[i] < lo) lo = g_host->high_room[i];
        if ((uint8_t *)g_host->high_room[i] + g_host->high_room_bytes[i] > hi) hi = (uint8_t *)g_host->high_room[i] + g_host->high_room_bytes[i];
    }
    uint8_t *at = NULL, *map = NULL;
    size_t maplen = len;
    const char *how = "";
    pthread_mutex_lock(&g_room_lock);
    uint8_t *first = (uint8_t *)(((uintptr_t)lo + align - 1) & ~(uintptr_t)(align - 1));
    if (len == ((size_t)16 << 30) && g_room_pools) {
        at = map = g_room_pools + ((size_t)8 << 30);
        g_room_pools = NULL;
        how = " (over the pools' unused halves)";
    } else if (len == ((size_t)32 << 30)) {
        for (uint8_t *c = first; c + len <= hi && !at; c += align) {
            if (room_held(c, len) && room_unused(c, len)) {
                at = map = c;
                if (!g_room_pools) {
                    g_room_pools = c;
                    if (g_room_nspare < 4) g_room_spare[g_room_nspare++] = (TlRoomUse){c + len * 3 / 4, len / 4};
                }
                room_use(c, len);
            } else if (room_held(c + len / 2, len / 2) && room_unused(c + len / 2, len / 2)) {
                at = c, map = c + len / 2, maplen = len / 2; /* only the upper half: the lower stays for others */
                room_use(map, maplen);
                if (g_room_nspare < 4) g_room_spare[g_room_nspare++] = (TlRoomUse){c + len * 3 / 4, len / 4};
                how = " (upper half mapped)";
                /* Oilpan gives its lower half back at once: that is others' (the hold, the JIT view), not its. */
                if (g_room_nslack < 16) g_room_slack[g_room_nslack++] = (TlRoomUse){c, len / 2};
            }
        }
    } else {
        for (uint8_t *c = first; c + len <= hi && !at; c += align)
            if (room_held(c, len) && room_unused(c, len)) {
                at = map = c;
                room_use(c, len);
            }
        static TlRoomUse spare_used[16];
        static int nspare_used;
        for (int z = 0; z < g_room_nspare && !at && len < ((size_t)4 << 30) && !slack; z++) {
            uint8_t *zlo = g_room_spare[z].at, *zhi = zlo + g_room_spare[z].len;
            size_t a = align > ((size_t)256 << 20) ? ((size_t)256 << 20) : align;
            if (len < ((size_t)256 << 20) && a > ((size_t)16 << 20)) a = (size_t)16 << 20;
            for (uint8_t *c = (uint8_t *)(((uintptr_t)zlo + a - 1) & ~(uintptr_t)(a - 1)); c + len <= zhi && !at; c += a) {
                int clash = 0;
                for (int i = 0; i < nspare_used && !clash; i++)
                    clash = c < spare_used[i].at + spare_used[i].len && spare_used[i].at < c + len;
                if (!clash) {
                    at = map = c;
                    if (nspare_used < 16) spare_used[nspare_used++] = (TlRoomUse){c, len};
                    how = " (over a big block's never-used top)";
                }
            }
        }
    }
    pthread_mutex_unlock(&g_room_lock);
    if (!at && slack) {
        /* The held room is full: an aligned address the system confirms free, from the bottom of the high space up.
           vm_allocate at a fixed address fails if any of it is in use, so nothing is touched (iOS ignores mmap's
           address hint here). */
        for (uint8_t *c = (uint8_t *)(((uintptr_t)lo + align - 1) & ~(uintptr_t)(align - 1)); c + len <= hi + ((size_t)8 << 30); c += align) {
            vm_address_t a = (vm_address_t)c;
            if (vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED) != KERN_SUCCESS) continue;
            vm_protect(mach_task_self(), a, len, FALSE, VM_PROT_NONE);
            void *r = (void *)a;
            if (r == c) {
                tl_log("a %.2f GB reservation at %p, free space past the held room (its alignment slack left out): placed",
                       (double)len / (1 << 30), c);
                pthread_mutex_lock(&g_room_lock);
                if (g_room_nslack < 16) g_room_slack[g_room_nslack++] = (TlRoomUse){c + len, slack};
                pthread_mutex_unlock(&g_room_lock);
                return c;
            }
        }
    }
    if (!at) {
        tl_log("a %.1f GB reservation does not fit the held room", (double)len / (1 << 30));
        return NULL;
    }
    if (!room_still_held(map, maplen)) {
        tl_log("a %.1f GB reservation: the held room at %p is no longer empty; not placed", (double)len / (1 << 30), map);
        return NULL;
    }
    void *r = mmap(map, maplen, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    tl_log("a %.3f GB reservation at %p in the held room%s%s: %s", (double)len / (1 << 30), at, how,
           slack ? " (its alignment slack left out)" : "", r == map ? "placed" : strerror(errno));
    if (r == map && slack) {
        pthread_mutex_lock(&g_room_lock);
        if (g_room_nslack < 16) g_room_slack[g_room_nslack++] = (TlRoomUse){at + len, slack};
        pthread_mutex_unlock(&g_room_lock);
    }
    return r == map ? at : NULL;
}

static int rt_munmap(void *addr, size_t len) {
    if (g_host && g_host->high_rooms) {
        pthread_mutex_lock(&g_room_lock);
        for (int i = 0; i < g_room_nslack; i++)
            if ((uint8_t *)addr >= g_room_slack[i].at && (uint8_t *)addr + len <= g_room_slack[i].at + g_room_slack[i].len) {
                pthread_mutex_unlock(&g_room_lock);
                return 0; /* the slack of a reservation placed exactly: never mapped for it */
            }
        pthread_mutex_unlock(&g_room_lock);
    }
    return munmap(addr, len);
}

static void *rt_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (g_host && g_host->ios_kernel_rules && (prot & PROT_EXEC)) {
        tl_log("refused an executable mapping (iOS)");
        errno = EPERM;
        return MAP_FAILED;
    }
    if (g_host && g_host->high_rooms && len >= ((size_t)4 << 30) && prot == PROT_NONE && (flags & MAP_ANON) &&
        !(flags & MAP_FIXED)) {
        void *r = room_place(len);
        if (r) return r;
    }
    uint64_t max = max_map_bytes();
    if (max && len > max) {
        tl_log("refused a %llu GB mapping (TL_MAX_MAP_GB)", (unsigned long long)(len >> 30));
        errno = ENOMEM;
        return MAP_FAILED;
    }
    /* TL_ROOM_ONLY=1 (a test switch for Mac runs): no room for large reservations outside the held room, as on the
       phone once Chromium has started. */
    int room_only = g_host && g_host->high_rooms && getenv("TL_ROOM_ONLY") && len >= ((size_t)256 << 20) && prot == PROT_NONE &&
                    (flags & MAP_ANON) && !(flags & MAP_FIXED);
    /* ... and, as iOS does, no mapping hinted past the end of the 64 GB (the lowest held block + 64 GB). */
    int past_end = g_host && g_host->high_rooms && getenv("TL_ROOM_ONLY") && addr && !(flags & MAP_FIXED) &&
                   (uint8_t *)addr >= (uint8_t *)g_host->high_room[0] + ((size_t)64 << 30);
    void *r = room_only || past_end ? (errno = ENOMEM, MAP_FAILED) : mmap(addr, len, prot, flags, fd, off);
    if (r == MAP_FAILED && addr && !(flags & MAP_FIXED) && errno == ENOMEM && !room_only) {
        /* A hint is only a hint (macOS places the mapping elsewhere); iOS fails a mapping hinted past its 64 GB, and
           Chromium's allocator takes that for running out. Again without it. */
        static int said;
        if (!said++) tl_log("a mapping hinted at %p failed: again without the hint (and from now on)", addr);
        r = mmap(NULL, len, prot, flags, fd, off);
    }
    if (r == MAP_FAILED && g_host && g_host->high_rooms && len >= ((size_t)1 << 20) && (flags & MAP_ANON) &&
        !(flags & MAP_FIXED)) {
        /* No room left anywhere (iOS's 64 GB run out with Chromium in it): the held room's spare slots, or the
           never-used tops of the big blocks, with the access asked for. */
        static int listed;
        if (len < ((size_t)256 << 20) && !listed++) log_high_space();
        void *h = room_place_prot(len, prot);
        if (h) return h;
        tl_log("no room for %.3f GB anywhere:", (double)len / (1 << 30));
        log_free_address_space();
        errno = ENOMEM;
    }
    if (r == MAP_FAILED && len < ((size_t)1 << 30) && g_host && g_host->high_rooms) {
        static int said;
        if (said++ < 20) tl_log("a %.1f MB mapping (prot %d, flags %#x, at %p) failed: %s", (double)len / (1 << 20), prot, flags, addr, strerror(errno));
    }
    if (len >= ((size_t)1 << 30)) {
        if (r == MAP_FAILED) {
            int e = errno;
            tl_log("a %.1f GB mapping (prot %d, flags %#x, at %p) failed: %s", (double)len / (1 << 30), prot, flags, addr, strerror(e));
            log_free_address_space();
            errno = e;
        } else if (getenv("TL_MAP_TRACE") || len >= ((size_t)8 << 30))
            tl_log("a %.1f GB mapping at %p", (double)len / (1 << 30), r);
    }
    return r;
}
#if TARGET_OS_OSX
static kern_return_t rt_mach_vm_allocate(vm_map_t map, mach_vm_address_t *addr, mach_vm_size_t size, int flags) {
    uint64_t max = max_map_bytes();
    if (max && size > max) {
        tl_log("refused a %llu GB VM allocation (TL_MAX_MAP_GB)", (unsigned long long)(size >> 30));
        return KERN_NO_SPACE;
    }
    return mach_vm_allocate(map, addr, size, flags);
}
static kern_return_t rt_mach_vm_map(vm_map_t map, mach_vm_address_t *addr, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                                    mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur, vm_prot_t max_prot,
                                    vm_inherit_t inh) {
    uint64_t max = max_map_bytes();
    if (max && size > max) {
        tl_log("refused a %llu GB VM map (TL_MAX_MAP_GB)", (unsigned long long)(size >> 30));
        return KERN_NO_SPACE;
    }
    return mach_vm_map(map, addr, size, mask, flags, obj, off, copy, cur, max_prot, inh);
}
#endif

static int rt_mprotect(void *addr, size_t len, int prot) {
    if (g_host && g_host->ios_kernel_rules && (prot & PROT_EXEC)) {
        tl_log("refused making memory executable (iOS)");
        errno = EPERM;
        return -1;
    }
    int r = mprotect(addr, len, prot);
    if (r && g_host && g_host->high_rooms) { /* where the phone's address space runs short: every failure, for the log */
        static int said;
        if (said++ < 20) tl_log("mprotect %p +%zu prot %d failed: %s", addr, len, prot, strerror(errno));
    }
    return r;
}

/* bootstrap_check_in, bootstrap_register, bootstrap_register2: an app on iOS may not offer Mach services. */
static kern_return_t rt_bootstrap_refused(void) {
    tl_log("refused a Mach service registration (iOS)");
    return 1100; /* BOOTSTRAP_NOT_PRIVILEGED */
}

/* Carbon's HIToolbox looked up by bundle identifier, as GLFW does for its keyboard-layout functions
   (TISCopyCurrentKeyboardLayoutInputSource, kTISPropertyUnicodeKeyLayoutData, LMGetKbdType): iOS has no such
   bundle, and on the Mac test the real one must not answer either (it works only with the real AppKit). So the
   identifier answers a bundle that is always there (CoreFoundation's), and names looked up in it are answered by
   Henge's Carbon stand-ins first. */
#include <CoreFoundation/CoreFoundation.h>
static CFBundleRef g_hitoolbox;
static const char *const g_hitoolbox_name =
    "/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/HIToolbox";

static CFBundleRef rt_CFBundleGetBundleWithIdentifier(CFStringRef ident) {
    if (ident && (CFEqual(ident, CFSTR("com.apple.HIToolbox")) || CFEqual(ident, CFSTR("com.apple.Carbon")))) {
        if (!g_hitoolbox) g_hitoolbox = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.CoreFoundation"));
        return g_hitoolbox;
    }
    return CFBundleGetBundleWithIdentifier(ident);
}

static void *hitoolbox_symbol(CFBundleRef bundle, CFStringRef name) {
    if (!bundle || bundle != g_hitoolbox || !name || !g_host || !g_host->shim_lookup) return NULL;
    char buf[256] = "_";
    if (!CFStringGetCString(name, buf + 1, sizeof buf - 1, kCFStringEncodingUTF8)) return NULL;
    return g_host->shim_lookup(g_hitoolbox_name, buf);
}

static void *rt_CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef name) {
    void *p = hitoolbox_symbol(bundle, name);
    return p ? p : CFBundleGetFunctionPointerForName(bundle, name);
}

static void *rt_CFBundleGetDataPointerForName(CFBundleRef bundle, CFStringRef name) {
    void *p = hitoolbox_symbol(bundle, name);
    return p ? p : CFBundleGetDataPointerForName(bundle, name);
}

void *tl_cf_main_bundle(int ns); /* tl_bundle.m */
static int ns_of(const void *addr);
static CFBundleRef rt_CFBundleGetMainBundle(void) {
    CFBundleRef b = tl_cf_main_bundle(ns_of(__builtin_return_address(0)));
    return b ? b : CFBundleGetMainBundle();
}

/* CoreFoundation's functions answered here (imports from CoreFoundation). */
static void *cf_override(const char *sym) {
    if (!strcmp(sym, "_CFBundleGetMainBundle")) return (void *)rt_CFBundleGetMainBundle;
    if (!strcmp(sym, "_CFBundleGetBundleWithIdentifier")) return (void *)rt_CFBundleGetBundleWithIdentifier;
    if (!strcmp(sym, "_CFBundleGetFunctionPointerForName")) return (void *)rt_CFBundleGetFunctionPointerForName;
    if (!strcmp(sym, "_CFBundleGetDataPointerForName")) return (void *)rt_CFBundleGetDataPointerForName;
    return NULL;
}

void *tl_runtime_lookup(const char *sym) {
    static const struct { const char *name; void *fn; } table[] = {
        {"_dlopen", (void *)rt_dlopen},
        {"_mmap", (void *)rt_mmap},
        {"_mprotect", (void *)rt_mprotect},
        {"_munmap", (void *)rt_munmap},
#if TARGET_OS_OSX
        {"_mach_vm_allocate", (void *)rt_mach_vm_allocate},
        {"_mach_vm_map", (void *)rt_mach_vm_map},
#endif
        {"_dlclose", (void *)rt_dlclose},
        {"_dlsym", (void *)rt_dlsym},
        {"_dladdr", (void *)rt_dladdr},
        {"_dlerror", (void *)rt_dlerror},
        {"__NSGetExecutablePath", (void *)rt_NSGetExecutablePath},
        {"__NSGetMachExecuteHeader", (void *)rt_NSGetMachExecuteHeader},
        {"__dyld_image_count", (void *)rt_dyld_image_count},
        {"__dyld_get_image_header", (void *)rt_dyld_get_image_header},
        {"__dyld_get_image_name", (void *)rt_dyld_get_image_name},
        {"__dyld_get_image_vmaddr_slide", (void *)rt_dyld_get_image_vmaddr_slide},
        {"__dyld_register_func_for_add_image", (void *)rt_dyld_register_func_for_add_image},
        {"__dyld_register_func_for_remove_image", (void *)rt_dyld_register_func_for_remove_image},
    };
    static const struct { const char *name; void *fn; } procs[] = {
        {"_posix_spawn", (void *)rt_posix_spawn},
        {"_posix_spawnp", (void *)rt_posix_spawn},
        {"_execve", (void *)rt_execve},
        {"_execv", (void *)rt_execv},
        {"_execvp", (void *)rt_execv},
        {"_execl", (void *)rt_execl},
        {"_execlp", (void *)rt_execl},
        {"_execle", (void *)rt_execle},
        {"_fork", (void *)rt_fork},
        {"_vfork", (void *)rt_fork},
        {"_system", (void *)rt_system},
        {"_popen", (void *)rt_popen},
        {"_pclose", (void *)rt_pclose},
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(sym, table[i].name)) return table[i].fn;
    if (g_host && (g_host->processes_as_threads || g_host->ios_kernel_rules)) {
        static const char *const sandbox[] = {"_sandbox_init", "_sandbox_init_with_parameters", "_sandbox_init_with_extensions",
                                              "_sandbox_apply", "_sandbox_apply_container"};
        for (size_t i = 0; i < sizeof sandbox / sizeof *sandbox; i++)
            if (!strcmp(sym, sandbox[i])) return (void *)rt_sandbox_ok;
    }
    if (g_host && g_host->processes_as_threads) {
        if (!strcmp(sym, "__NSGetArgc")) return (void *)rt__NSGetArgc;
        if (!strcmp(sym, "__NSGetArgv")) return (void *)rt__NSGetArgv;
        if (!strcmp(sym, "__NSGetEnviron")) return (void *)rt__NSGetEnviron;
    }
    /* Mach bootstrap names: kept inside the app wherever iOS's rules apply (an app may not register services there,
       and only the app itself can reach them anyway). */
    if (g_host && (g_host->processes_as_threads || g_host->ios_kernel_rules || g_host->no_processes) && !strcmp(sym, "_launch_msg"))
        return (void *)rt_launch_msg;
    if (g_host && (g_host->processes_as_threads || g_host->ios_kernel_rules)) {
        if (!strcmp(sym, "_bootstrap_check_in")) return (void *)rt_bootstrap_check_in;
        if (!strcmp(sym, "_bootstrap_look_up")) return (void *)rt_bootstrap_look_up;
        if (!strcmp(sym, "_bootstrap_register")) return (void *)rt_bootstrap_register;
    }
    if (g_host && g_host->processes_as_threads) {
        static const struct { const char *name; void *fn; } pat[] = {
            {"_getpid", (void *)rt_getpid}, {"_getppid", (void *)rt_getppid}, {"_kill", (void *)rt_kill},
            {"_waitpid", (void *)rt_waitpid}, {"_waitid", (void *)rt_waitid}, {"_exit", (void *)rt_exit}, {"__exit", (void *)rt__exit},
            {"__Exit", (void *)rt__exit}, {"_bootstrap_check_in", (void *)rt_bootstrap_check_in},
            {"_bootstrap_look_up", (void *)rt_bootstrap_look_up}, {"_bootstrap_register", (void *)rt_bootstrap_register},
            {"_audit_token_to_pid", (void *)rt_audit_token_to_pid},
            {"_dispatch_source_create", (void *)rt_dispatch_source_create},
            {"_task_set_exception_ports", (void *)rt_task_set_exception_ports},
            {"_posix_spawn_file_actions_adddup2", (void *)rt_posix_spawn_file_actions_adddup2},
            {"_posix_spawn_file_actions_destroy", (void *)rt_posix_spawn_file_actions_destroy},
            {"_task_swap_exception_ports", (void *)rt_task_swap_exception_ports},
            {"_malloc_get_all_zones", (void *)rt_malloc_get_all_zones},
            {"_malloc_zone_register", (void *)rt_malloc_zone_register},
            {"_malloc_zone_unregister", (void *)rt_malloc_zone_unregister},
        };
        if (getenv("TL_IPC_TRACE")) {
            if (!strcmp(sym, "_sem_open")) return (void *)rt_sem_open;
            if (!strcmp(sym, "_connect")) return (void *)rt_connect;
            if (!strcmp(sym, "_bind")) return (void *)rt_bind;
        }
        const char *skip = getenv("TL_PAT_SKIP"); /* a test switch: symbols not to answer here */
        for (size_t i = 0; i < sizeof pat / sizeof *pat; i++)
            if (!strcmp(sym, pat[i].name) && !(skip && strstr(skip, pat[i].name))) return pat[i].fn;
    }
    if (g_host && g_host->ios_kernel_rules &&
        (!strcmp(sym, "_bootstrap_check_in") || !strcmp(sym, "_bootstrap_register") || !strcmp(sym, "_bootstrap_register2")))
        return (void *)rt_bootstrap_refused;
    if (g_host && g_host->no_processes)
        for (size_t i = 0; i < sizeof procs / sizeof *procs; i++)
            if (!strcmp(sym, procs[i].name)) return procs[i].fn;
    return NULL;
}

/* ---- install names on iOS ---- */

static const char *const g_mac_only[] = {
    "AppKit", "Cocoa", "Carbon", "ApplicationServices", "OpenGL", "AGL", "AppleScriptObjC", "Quartz", "QuickLookUI",
    "ScreenSaver", "InputMethodKit", "PreferencePanes", "ServiceManagement", "OSAKit", "Automator", "DiskArbitration",
    "SecurityInterface", "ExceptionHandling", "IOBluetoothUI", "InstallerPlugins", "SyncServices", "DirectoryService",
};

const char *tl_ios_map_install_name(const char *name, char *buf, size_t len) {
    if (!strncmp(name, "/System/iOSSupport/", 19)) name += 18; /* keep the leading '/' of /System/Library/... */
    const char *fw = strstr(name, "/System/Library/Frameworks/");
    if (fw == name || !strncmp(name, "/System/Library/PrivateFrameworks/", 34)) {
        /* .../X.framework/Versions/A/X -> .../X.framework/X */
        const char *dot = strstr(name, ".framework/");
        if (!dot) return name;
        const char *start = name;
        for (const char *p = dot; p > name; p--)
            if (p[-1] == '/') { start = p; break; }
        size_t stem = (size_t)(dot - start);
        for (size_t i = 0; i < sizeof g_mac_only / sizeof *g_mac_only; i++)
            if (strlen(g_mac_only[i]) == stem && !strncmp(start, g_mac_only[i], stem)) return NULL;
        const char *leaf = strrchr(name, '/') + 1;
        if (snprintf(buf, len, "%.*s.framework/%s", (int)(dot - name), name, leaf) >= (int)len) return NULL;
        return buf;
    }
    return name; /* /usr/lib/... is where it is on iOS too */
}
