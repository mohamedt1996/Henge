#include "dt_fixup.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* See dt_fixup.h. A background thread (hv_dt_fixup_run) scans guest RAM while
   iBoot runs, so a pass over even 8 GB never stalls the guest's CPU. It looks
   two ways: the tree's magic (page-aligned anywhere, 16-byte aligned in the low
   heap), and the exact address boot_args names (DeviceTreeP) — the final blob
   the kernel takes, wherever iBoot put it. Once boot_args names it, that one
   blob is re-patched every millisecond until the caller stops us at the first
   GPU frame, by which point the kernel has the tree in its registry. */

#define STOCK_NAME "Apple Virtual Machine 1"
#define STOCK_MODEL "VirtualMac2,1"
#define SCAN_WINDOW (256ull << 20)
#define SCAN_EVERY 0.05
#define GIVE_UP 120.0

static uint8_t *ram;
static uint64_t ram_size, ram_base;
static char want_name[32], want_model[32];
static _Atomic int active; /* the scan thread reads it; stop() clears it from the guest thread */
static int total_changed, diag;
static double started, last_pass;
static uint64_t tracked[8];
static int ntracked;
static uint64_t final_off, final_len; /* the tree boot_args names, once known */
static int have_final, final_adt; /* final_adt: Apple's magic-less ADT, not an FDT blob */
static uint64_t passes, magic_seen, magic_valid, string_hits; /* what the scan saw, for the stop summary */
static int magic_logged, ba_logged;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A flattened device tree (v17 header), whole within avail. */
static int is_dtb(const uint8_t *b, uint64_t avail) {
    if (avail < 40 || be32(b) != 0xd00dfeed) return 0;
    uint32_t total = be32(b + 4), off_struct = be32(b + 8), off_strings = be32(b + 12);
    uint32_t version = be32(b + 20), size_strings = be32(b + 32), size_struct = be32(b + 36);
    if (version < 16 || version > 18 || total < 40 || total > (1u << 20) || total > avail) return 0;
    return off_struct < total && off_strings < total && size_struct <= total - off_struct &&
           size_strings <= total - off_strings;
}

/* A shorter string is NUL-padded to the property's length: the tree is edited
   in place, and device-tree strings are read as C strings. The slot may be
   larger than the stock string (a fixed-size field), so only its prefix needs
   to match. Only the stock value is replaced, so another pass changes nothing,
   and a value iBoot put back is replaced again. */
static int replace(uint8_t *data, uint32_t len, const char *stock, const char *want) {
    uint32_t sl = (uint32_t)strlen(stock), n = (uint32_t)strlen(want) + 1;
    if (n > len || len < sl || memcmp(data, stock, sl)) return 0;
    if (len > sl && data[sl] != 0) return 0; /* the stock string must end here */
    memcpy(data, want, n);
    memset(data + n, 0, len - n);
    return 1;
}

/* One mismatch is worth the whole story: the slot's real size and value say
   whether the firmware differs from what this expects. A value already rewritten
   is not a mismatch. */
static void mismatch(const char *prop, const uint8_t *data, uint32_t len, const char *stock, const char *want) {
    if (diag) return;
    uint32_t wl = (uint32_t)strlen(want) + 1;
    if (len >= wl && memcmp(data, want, wl) == 0) return;
    diag = 1;
    fprintf(stderr, "hengevm: device tree: %s is %u bytes \"%.*s\" (stock \"%s\", wanted \"%s\"); leaving it\n",
            prop, len, (int)(len < 60 ? len : 60), (const char *)data, stock, want);
}

/* Apple's ADT (m1n1 proxyclient/m1n1/adt.py): no magic. A node is {u32 prop_count,
   u32 child_count, prop_count properties, child_count child nodes}; a property is
   {char name[32], u32 size, u8 value[size & 0x7fffffff]}, each property 4-aligned.
   Patch identity properties by name and stock value — "model" by name leaves the
   "compatible" array (driver matching) alone. Returns bytes consumed, 0 on error. */
static uint64_t adt_walk(uint8_t *p, uint64_t avail, int depth, int *changed) {
    if (depth > 64 || avail < 8) return 0;
    uint32_t nprop = le32(p), nchild = le32(p + 4);
    if (nprop > 4096 || nchild > 4096) return 0;
    uint64_t off = 8;
    for (uint32_t i = 0; i < nprop; i++) {
        if (off + 36 > avail) return 0;
        const char *name = (const char *)(p + off);
        uint32_t size = le32(p + off + 32) & 0x7fffffff;
        off += 36;
        if (size > avail - off) return 0;
        if (memchr(name, 0, 32)) {
            if (want_model[0] && !strcmp(name, "model"))
                *changed += replace(p + off, size, STOCK_MODEL, want_model);
            else if (want_name[0] && !strcmp(name, "product-name"))
                *changed += replace(p + off, size, STOCK_NAME, want_name);
        }
        off += (size + 3) & ~3u;
    }
    for (uint32_t i = 0; i < nchild; i++) {
        if (off >= avail) return 0;
        uint64_t used = adt_walk(p + off, avail - off, depth + 1, changed);
        if (!used) return 0;
        off += used;
    }
    return off;
}

int hv_dt_fixup_blob(uint8_t *b, uint64_t avail) {
    if (!is_dtb(b, avail)) return 0;
    uint32_t off_struct = be32(b + 8), off_strings = be32(b + 12);
    uint32_t size_strings = be32(b + 32), size_struct = be32(b + 36);
    const uint8_t *strings = b + off_strings, *stop = b + off_struct + size_struct;
    const uint8_t *p = b + off_struct;
    int depth = 0, in_product = 0, changed = 0;
    while (p + 4 <= stop) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 9) break;    /* FDT_END */
        if (tok == 4) continue; /* FDT_NOP */
        if (tok == 1) {         /* FDT_BEGIN_NODE: name, NUL-terminated, padded to 4 */
            const uint8_t *name = p;
            while (p < stop && *p) p++;
            if (p >= stop) break;
            p++;
            p += (4 - ((uintptr_t)p & 3)) & 3;
            if (p > stop) break;
            depth++;
            if (depth == 2) in_product = !strcmp((const char *)name, "product");
            continue;
        }
        if (tok == 2) { /* FDT_END_NODE */
            if (depth == 2) in_product = 0;
            depth--;
            continue;
        }
        if (tok != 3 || p + 8 > stop) break; /* FDT_PROP: {len, nameoff, data} */
        uint32_t len = be32(p), nameoff = be32(p + 4);
        p += 8;
        if (len > (uint64_t)(stop - p) || nameoff >= size_strings ||
            !memchr(strings + nameoff, 0, size_strings - nameoff)) break;
        uint8_t *data = (uint8_t *)p;
        p += (len + 3) & ~3u;
        if (p > stop) break;
        const char *pname = (const char *)(strings + nameoff);
        if (depth == 1 && want_model[0] && !strcmp(pname, "model")) {
            int r = replace(data, len, STOCK_MODEL, want_model);
            if (r) changed += r;
            else mismatch("model", data, len, STOCK_MODEL, want_model);
        } else if (depth == 2 && in_product && want_name[0] && !strcmp(pname, "product-name")) {
            int r = replace(data, len, STOCK_NAME, want_name);
            if (r) changed += r;
            else mismatch("product-name", data, len, STOCK_NAME, want_name);
        }
    }
    return changed;
}

int hv_dt_fixup_start(uint8_t *r, uint64_t size, uint64_t base, const char *product_name, const char *model_id) {
    ram = r;
    ram_size = size;
    ram_base = base;
    want_name[0] = want_model[0] = 0;
    active = total_changed = ntracked = have_final = final_adt = diag = 0;
    final_off = final_len = 0;
    passes = magic_seen = magic_valid = string_hits = 0;
    magic_logged = ba_logged = 0;
    started = last_pass = 0;
    if (!r) return 0;
    if (product_name && *product_name) {
        if (strlen(product_name) > strlen(STOCK_NAME))
            fprintf(stderr, "hengevm: device tree: \"%s\" is longer than the name slot; leaving it\n", product_name);
        else snprintf(want_name, sizeof want_name, "%s", product_name);
    }
    if (model_id && *model_id) {
        if (strlen(model_id) > strlen(STOCK_MODEL))
            fprintf(stderr, "hengevm: device tree: \"%s\" is longer than the model slot; leaving it\n", model_id);
        else snprintf(want_model, sizeof want_model, "%s", model_id);
    }
    active = want_name[0] || want_model[0];
    if (active) {
        started = now();
        fprintf(stderr, "hengevm: device tree: watching for the guest's tree (name \"%s\", model \"%s\")\n",
                want_name[0] ? want_name : "-", want_model[0] ? want_model : "-");
    }
    return active;
}

/* Rewrite the held final tree in its own format. */
static int patch_final(void) {
    if (final_adt) {
        int ch = 0;
        adt_walk(ram + final_off, final_len, 0, &ch);
        return ch;
    }
    return hv_dt_fixup_blob(ram + final_off, final_len);
}

int hv_dt_fixup_tree(uint64_t offset, uint64_t len) {
    if (!active || have_final) return have_final;
    if (offset + 40 > ram_size || len < 40 || len > (1u << 20) || offset + len > ram_size) return 0;
    uint8_t *b = ram + offset;
    int adt = 0, n = 0;
    if (is_dtb(b, len)) {
        n = hv_dt_fixup_blob(b, len);
    } else {
        int ch = 0;
        if (!adt_walk(b, len, 0, &ch)) return 0; /* neither an FDT blob nor an ADT */
        adt = 1;
        n = ch;
    }
    have_final = 1;
    final_off = offset;
    final_len = len;
    final_adt = adt;
    fprintf(stderr, "hengevm: device tree: holding the final tree at RAM +%llx (%llu bytes, %s)\n",
            (unsigned long long)offset, (unsigned long long)len, adt ? "Apple ADT" : "FDT");
    if (n) {
        fprintf(stderr, "hengevm: device tree (final) at RAM +%llx: rewrote %d string%s\n",
                (unsigned long long)offset, n, n == 1 ? "" : "s");
        total_changed += n;
    }
    return 1;
}

/* Patch a tree found by the scan, remembering it so a value iBoot puts back is
   replaced again. */
static void consider(uint64_t off, uint64_t avail) {
    if (!is_dtb(ram + off, avail)) return;
    int known = 0;
    for (int i = 0; i < ntracked; i++) known |= tracked[i] == off;
    if (!known && ntracked < 8) tracked[ntracked++] = off;
    int n = hv_dt_fixup_blob(ram + off, avail);
    if (n) {
        fprintf(stderr, "hengevm: device tree at RAM +%llx: rewrote %d identity string%s\n",
                (unsigned long long)off, n, n == 1 ? "" : "s");
        total_changed += n;
    }
}

/* boot_args names the final tree's exact address: DeviceTreeP at +96 (q[12]),
   length at +104 (q[13]); try +88 (q[11]) too in case this revision has no
   machineType. DeviceTreeP is a kASLR-slid VIRTUAL address (0xfffffe…): physical
   is vaddr - virt_base + phys_base, as m1n1's boot protocol documents. The magic
   check decides which candidate is right. */
static uint64_t dt_phys(uint64_t v, uint64_t virt_base, uint64_t phys_base) {
    if (v >> 40 == 0xfffffe && virt_base) return v - virt_base + phys_base;
    return v; /* already physical */
}

/* A quick gate before the full parse: an FDT magic, or an ADT root with sane
   nonzero counts. Keeps the walker off obvious garbage. */
static int looks_like_tree(const uint8_t *b, uint64_t avail) {
    if (avail < 40) return 0;
    if (be32(b) == 0xd00dfeed) return 1;
    uint32_t nprop = le32(b), nchild = le32(b + 4);
    return nprop > 0 && nprop <= 4096 && nchild <= 4096;
}

static void find_boot_args_tree(void) {
    if (have_final) return;
    for (uint64_t off = 0; off + 0x1000 <= ram_size; off += 0x4000) {
        const uint64_t *q = (const uint64_t *)(ram + off);
        const uint16_t *h = (const uint16_t *)(ram + off);
        if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue; /* revision, version */
        if (q[1] >> 40 != 0xfffffe || q[2] < ram_base || q[2] >= ram_base + ram_size || q[3] == 0 || q[3] > ram_size)
            continue;
        if (ba_logged < 3) {
            ba_logged++;
            uint64_t pa = dt_phys(q[12], q[1], q[2]);
            fprintf(stderr, "hengevm: device tree: boot_args at RAM +%llx: virt_base=%llx phys_base=%llx devtree=%llx -> phys %llx, len %u\n",
                    (unsigned long long)off, (unsigned long long)q[1], (unsigned long long)q[2],
                    (unsigned long long)q[12], (unsigned long long)pa, (uint32_t)q[13]);
            if (pa >= ram_base && pa - ram_base + 16 <= ram_size) {
                const uint8_t *d = ram + (pa - ram_base);
                fprintf(stderr, "hengevm: device tree:   bytes at the tree: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            }
        }
        const uint64_t cand[2] = {q[12], q[11]};
        const uint32_t clen[2] = {(uint32_t)q[13], (uint32_t)q[12]};
        for (int c = 0; c < 2; c++) {
            uint64_t dt_pa = dt_phys(cand[c], q[1], q[2]);
            uint32_t dt_len = clen[c];
            if (dt_pa < ram_base || dt_pa - ram_base + 40 > ram_size || dt_len < 40 || dt_len > (1u << 20) ||
                dt_pa - ram_base + dt_len > ram_size) continue;
            uint64_t dt_off = dt_pa - ram_base;
            if (looks_like_tree(ram + dt_off, dt_len) && hv_dt_fixup_tree(dt_off, dt_len)) return;
        }
    }
}

/* The surest anchor: the product-name string the kernel displays. boot_args may
   point at a wrapped tree, and the magic may sit at any alignment, so find the
   string itself, then the tree header before it. If the tree parses, both strings
   are rewritten through it; if its format is opaque, the display string is patched
   in place (safe for product-name; the model identifier is left to a real tree). */
static void patch_name_in_place(uint64_t off) {
    uint32_t sl = (uint32_t)strlen(STOCK_NAME) + 1, n = (uint32_t)strlen(want_name) + 1;
    if (!want_name[0] || n > sl || memcmp(ram + off, STOCK_NAME, sl)) return;
    memcpy(ram + off, want_name, n);
    memset(ram + off + n, 0, sl - n);
    total_changed++;
    fprintf(stderr, "hengevm: device tree: patched product-name in place at RAM +%llx\n", (unsigned long long)off);
}

static void find_by_string(void) {
    if (have_final) return;
    size_t nlen = strlen(STOCK_NAME) + 1; /* the property value, NUL included */
    uint64_t pos = 0;
    int hits = 0;
    while (pos + nlen <= ram_size) {
        const uint8_t *hit = memmem(ram + pos, ram_size - pos, STOCK_NAME, nlen);
        if (!hit) break;
        uint64_t hoff = (uint64_t)(hit - ram);
        string_hits++;
        if (hits < 3)
            fprintf(stderr, "hengevm: device tree: product-name string at RAM +%llx\n", (unsigned long long)hoff);
        hits++;
        /* The tree's magic precedes the string; walk back to the nearest valid
           header whose span reaches the string. */
        uint64_t floor = hoff > (1u << 20) ? hoff - (1u << 20) : 0;
        int found = 0;
        for (uint64_t b = hoff & ~3ull; b > floor; b -= 4) {
            const uint8_t *c = ram + b;
            if (c[0] != 0xd0 || c[1] != 0x0d || c[2] != 0xfe || c[3] != 0xed) continue;
            if (!is_dtb(c, ram_size - b) || b + be32(c + 4) <= hoff) continue;
            fprintf(stderr, "hengevm: device tree: the string sits in a tree at RAM +%llx (%u bytes)\n",
                    (unsigned long long)b, be32(c + 4));
            hv_dt_fixup_tree(b, be32(c + 4));
            found = 1;
            break;
        }
        if (found) return;
        if (hits <= 3)
            fprintf(stderr, "hengevm: device tree: no tree magic before the string at +%llx; patching the string\n",
                    (unsigned long long)hoff);
        patch_name_in_place(hoff);
        pos = hoff + nlen;
    }
}

/* One scan pass. Runs on the background thread (or, in tests, inline). */
static void scan_pass(void) {
    passes++;
    /* Trees already found may have had a value put back. */
    for (int i = 0; i < ntracked; i++) {
        if (tracked[i] + 40 > ram_size || !is_dtb(ram + tracked[i], ram_size - tracked[i])) continue;
        int n = hv_dt_fixup_blob(ram + tracked[i], ram_size - tracked[i]);
        if (n) {
            fprintf(stderr, "hengevm: device tree at RAM +%llx: rewrote %d string%s again\n",
                    (unsigned long long)tracked[i], n, n == 1 ? "" : "s");
            total_changed += n;
        }
    }
    /* Page-aligned trees anywhere in RAM: iBoot may load the tree high. */
    for (uint64_t off = 0; off + 40 <= ram_size; off += 0x1000) {
        const uint8_t *b = ram + off;
        if (b[0] != 0xd0 || b[1] != 0x0d || b[2] != 0xfe || b[3] != 0xed) continue;
        magic_seen++;
        if (!is_dtb(b, ram_size - off)) {
            if (magic_logged < 5) {
                magic_logged++;
                fprintf(stderr, "hengevm: device tree: magic at RAM +%llx, header rejected (ver %u, total %u, struct@%u, strings@%u)\n",
                        (unsigned long long)off, be32(b + 20), be32(b + 4), be32(b + 8), be32(b + 12));
            }
            continue;
        }
        magic_valid++;
        consider(off, ram_size - off);
    }
    /* Finer: iBoot's heap allocations are 16-byte aligned, not always page-aligned. */
    uint64_t limit = ram_size < SCAN_WINDOW ? ram_size : SCAN_WINDOW;
    for (uint64_t off = 0; off + 40 <= limit; off += 16) {
        const uint8_t *b = ram + off;
        if (b[0] == 0xd0 && b[1] == 0x0d && b[2] == 0xfe && b[3] == 0xed) consider(off, limit - off);
    }
    find_boot_args_tree();
    find_by_string();
}

void hv_dt_fixup_poll(void) {
    if (!active) return;
    double t = now();
    if (t - started > GIVE_UP) {
        fprintf(stderr, "hengevm: device tree: giving up after %.0f s (%d rewritten)\n", GIVE_UP, total_changed);
        active = 0;
        return;
    }
    /* The final tree, once boot_args has named it: re-patch at once, no throttle. */
    if (have_final) {
        int n = patch_final();
        if (n) {
            fprintf(stderr, "hengevm: device tree (final) at RAM +%llx: rewrote %d string%s\n",
                    (unsigned long long)final_off, n, n == 1 ? "" : "s");
            total_changed += n;
        }
        return;
    }
    if (t - last_pass < SCAN_EVERY) return;
    last_pass = t;
    scan_pass();
}

static void *scan_thread(void *arg) {
    (void)arg;
    while (active) {
        hv_dt_fixup_poll();
        usleep(1000);
    }
    return NULL;
}

void hv_dt_fixup_run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread, NULL) == 0) pthread_detach(t);
}

void hv_dt_fixup_stop(void) {
    if (!active) return;
    fprintf(stderr, "hengevm: device tree: the GPU drew after %llu passes (%llu tree magic seen, %llu valid, %llu name strings); %d string%s rewritten%s\n",
            (unsigned long long)passes, (unsigned long long)magic_seen, (unsigned long long)magic_valid,
            (unsigned long long)string_hits, total_changed, total_changed == 1 ? "" : "s",
            total_changed ? "" : ", the guest keeps its own name");
    active = 0;
}