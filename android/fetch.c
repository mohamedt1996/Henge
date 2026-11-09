/* Android: what Henge's Swift side does on iOS before an install — fetch the boot
   ROM from Apple's macOS 14.6.1 restore image (one ranged request, LZFSE block,
   APFS resource fork in LZVN pieces; see ios/Henge/BootROM.swift), download the
   macOS restore image, and lay out a new Mac's folder. */
#include "fetch.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lzfse.h"

static const char *ROM_SOURCE = "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/"
                                "932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw";
static const unsigned long long rom_block_offset = 5349700747ull;
enum { ROM_BLOCK_LENGTH = 859848, ROM_BLOCK_UNPACKED = 1048576, FORK_OFFSET = 876544, FORK_LENGTH = 116335,
       ROM_LENGTH = 214904 };
static const char *ROM_SHA256 = "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91";

/* No /etc/resolv.conf (Android): names resolve over HTTPS. Alternate resolvers between attempts, so a
   blocked or slow one does not stall the install. */
static void pick_dns(CURL *c, int attempt) {
    if (getenv("HV_DOH") || access("/etc/resolv.conf", R_OK) == 0) return;
    curl_easy_setopt(c, CURLOPT_DOH_URL, attempt % 2 ? "https://1.1.1.1/dns-query" : "https://8.8.8.8/dns-query");
}

/* Fail fast on a dead connection instead of waiting forever; resume handles the rest. */
static void timeouts(CURL *c) {
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
}

static int rom_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)u, (void)total, (void)ut, (void)un;
    static time_t last;
    time_t t = time(NULL);
    if (t != last) {
        last = t;
        fprintf(stderr, "\rhenge: boot ROM %lld / %d KB (%d%%)   ", (long long)now >> 10, ROM_BLOCK_LENGTH >> 10,
                (int)(100 * now / ROM_BLOCK_LENGTH));
    }
    return 0;
}

static const HvRelease releases[] = {
    {"15.6.1", "macOS Sequoia 15.6.1",
     "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/"
     "UniversalMac_15.6.1_24G90_Restore.ipsw"},
    {"26.6.2", "macOS Tahoe 26.6.2 (untested)",
     "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/"
     "UniversalMac_26.6.2_25G83_Restore.ipsw"},
    {"27.2", "macOS 27.2 (untested)",
     "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw"},
};

const HvRelease *hv_release_find(const char *id) {
    for (size_t i = 0; i < sizeof releases / sizeof *releases; i++)
        if (!strcmp(releases[i].id, id)) return &releases[i];
    return NULL;
}

void hv_release_list(FILE *f) {
    for (size_t i = 0; i < sizeof releases / sizeof *releases; i++) fprintf(f, "  %-8s %s\n", releases[i].id, releases[i].name);
}

/* ---- HTTP ---- */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static size_t to_buf(char *d, size_t sz, size_t n, void *u) {
    Buf *b = u;
    size_t k = sz * n;
    if (b->n + k > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1 << 20;
        while (cap < b->n + k) cap *= 2;
        uint8_t *np = realloc(b->p, cap);
        if (!np) return 0;
        b->p = np, b->cap = cap;
    }
    memcpy(b->p + b->n, d, k);
    b->n += k;
    return k;
}

/* ---- LZVN (as ios/Henge/BootROM.swift) ---- */

static int lzvn(const uint8_t *s, size_t n, uint8_t *out, size_t expected) {
    size_t i = 0, o = 0, distance = 0;
    while (i < n) {
        unsigned op = s[i];
        size_t literal = 0, match = 0, length = 1;
        if (op == 0x06) return o == expected ? 0 : -1;
        if (op == 0x0e || op == 0x16) { i++; continue; }
        if (op == 0x1e || op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || (op >= 0x70 && op <= 0x7f) ||
            (op >= 0xd0 && op <= 0xdf))
            return -1;
        if (op == 0xe0) {
            if (i + 1 >= n) return -1;
            literal = s[i + 1] + 16u, length = 2;
        } else if (op >= 0xe1 && op <= 0xef) {
            literal = op & 15;
        } else if (op == 0xf0) {
            if (i + 1 >= n) return -1;
            match = s[i + 1] + 16u, length = 2;
        } else if (op >= 0xf1) {
            match = op & 15;
        } else if (op >= 0xa0 && op <= 0xbf) {
            if (i + 2 >= n) return -1;
            unsigned w = s[i + 1] | s[i + 2] << 8;
            literal = (op >> 3) & 3;
            match = (((op & 7) << 2) | (w & 3)) + 3;
            distance = w >> 2, length = 3;
        } else {
            literal = (op >> 6) & 3;
            match = ((op >> 3) & 7) + 3;
            if ((op & 7) == 6) {
            } else if ((op & 7) == 7) {
                if (i + 2 >= n) return -1;
                distance = s[i + 1] | s[i + 2] << 8, length = 3;
            } else {
                if (i + 1 >= n) return -1;
                distance = (op & 7) << 8 | s[i + 1], length = 2;
            }
        }
        i += length;
        if (i + literal > n || o + literal + match > expected) return -1;
        memcpy(out + o, s + i, literal);
        o += literal, i += literal;
        if (match) {
            if (!distance || distance > o) return -1;
            for (size_t k = 0; k < match; k++, o++) out[o] = out[o - distance];
        }
    }
    return o == expected ? 0 : -1;
}

static int file_from_fork(const uint8_t *fork, size_t n, uint8_t *rom) {
#define WORD(at) ((size_t)fork[at] | (size_t)fork[(at) + 1] << 8 | (size_t)fork[(at) + 2] << 16 | (size_t)fork[(at) + 3] << 24)
    if (n < 4) return -1;
    size_t table = WORD(0), o = 0;
    if (table < 8 || table % 4 || table > n) return -1;
    for (size_t piece = 0; piece < table / 4 - 1; piece++) {
        size_t start = WORD(piece * 4), end = WORD((piece + 1) * 4);
        if (start >= end || end > n || piece * 65536 >= ROM_LENGTH) return -1;
        size_t want = ROM_LENGTH - piece * 65536 < 65536 ? ROM_LENGTH - piece * 65536 : 65536;
        if (fork[start] == 0x06) {
            if (end - start - 1 != want) return -1;
            memcpy(rom + o, fork + start + 1, want);
        } else if (lzvn(fork + start, end - start, rom + o, want)) {
            return -1;
        }
        o += want;
    }
    return o == ROM_LENGTH ? 0 : -1;
#undef WORD
}

static int genuine(const uint8_t *rom, size_t n) {
    if (n != ROM_LENGTH) return 0;
    uint8_t h[32];
    char hex[65];
    mbedtls_sha256(rom, n, h, 0);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    return !strcmp(hex, ROM_SHA256);
}

int hv_fetch_boot_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t have[ROM_LENGTH + 1];
        size_t n = fread(have, 1, sizeof have, f);
        fclose(f);
        if (genuine(have, n)) return 0;
    }
    fprintf(stderr, "henge: fetching the boot ROM from Apple (under 1 MB)\n");
    char range[64];
    snprintf(range, sizeof range, "%llu-%llu", rom_block_offset, rom_block_offset + ROM_BLOCK_LENGTH - 1);
    for (int attempt = 1; attempt <= 6; attempt++) {
        Buf b = {0};
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, ROM_SOURCE);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_buf);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, rom_progress);
        timeouts(c);
        pick_dns(c, attempt);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK || status != 206 || b.n != ROM_BLOCK_LENGTH) {
            fprintf(stderr, "\nhenge: boot ROM request failed (%s, HTTP %ld, %zu bytes)\n", curl_easy_strerror(rc), status, b.n);
            free(b.p);
            sleep((unsigned)attempt);
            continue;
        }
        fprintf(stderr, "\rhenge: boot ROM %d / %d KB (100%%)   \n", ROM_BLOCK_LENGTH >> 10, ROM_BLOCK_LENGTH >> 10);
        uint8_t *whole = malloc(ROM_BLOCK_UNPACKED), rom[ROM_LENGTH];
        size_t got = lzfse_decode_buffer(whole, ROM_BLOCK_UNPACKED, b.p, b.n, NULL);
        int ok = got == ROM_BLOCK_UNPACKED && file_from_fork(whole + FORK_OFFSET, FORK_LENGTH, rom) == 0 && genuine(rom, ROM_LENGTH);
        free(whole);
        free(b.p);
        if (!ok) {
            fprintf(stderr, "henge: the boot ROM from Apple wasn't what was expected; not used\n");
            return -1;
        }
        f = fopen(path, "wb");
        if (!f || fwrite(rom, 1, ROM_LENGTH, f) != ROM_LENGTH) {
            if (f) fclose(f);
            return -1;
        }
        fclose(f);
        return 0;
    }
    return -1;
}

/* ---- the restore image: resumable, with progress ---- */

typedef struct { FILE *f; curl_off_t base, mark; time_t last, mark_t; double rate; } Dl;

static size_t to_file(char *d, size_t sz, size_t n, void *u) { return fwrite(d, sz, n, ((Dl *)u)->f) * sz; }

static int dl_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)ut, (void)un;
    Dl *dl = u;
    time_t t = time(NULL);
    if (total > 0 && t != dl->last) {
        dl->last = t;
        double all = (double)(total + dl->base), at = (double)(now + dl->base);
        if (!dl->mark_t) dl->mark_t = t, dl->mark = now;
        else if (t - dl->mark_t >= 3) { /* smoothed over a few seconds */
            double r = (double)(now - dl->mark) / (double)(t - dl->mark_t);
            dl->rate = dl->rate ? dl->rate * 0.6 + r * 0.4 : r;
            dl->mark_t = t, dl->mark = now;
        }
        fprintf(stderr, "\rhenge: downloading macOS %.1f / %.1f GB (%.0f%%) %.1f MB/s   ", at / 1e9, all / 1e9, 100 * at / all,
                dl->rate / 1e6);
    }
    return 0;
}

/* ---- parallel: the image in 32 MB chunks over several connections (one connection to Apple's CDN
   is often held to a fraction of the line). Progress per chunk is kept in <path>.part.map, so it
   resumes; a .part from the single-connection download counts as its first bytes. ---- */

enum { CHUNK = 32 << 20 };
typedef struct { char magic[4]; uint32_t chunks; uint64_t total, chunk; } MapHead;
typedef struct Par Par;
typedef struct { Par *p; CURL *e; int chunk, checked, attempt; off_t pos, end; } Slot;
struct Par {
    int fd, mapfd, conns;
    uint64_t total, *done;
    uint32_t chunks;
    uint8_t *active;
    time_t *retry_at;
    int failed; /* the server refused the range: fall back */
};

static uint64_t chunk_len(const Par *p, uint32_t i) {
    uint64_t start = (uint64_t)i * CHUNK;
    return p->total - start < CHUNK ? p->total - start : CHUNK;
}

static void save_map(Par *p) {
    MapHead h = {{'H', 'D', 'L', '1'}, p->chunks, p->total, CHUNK};
    fdatasync(p->fd); /* the bytes before the record that says they are there */
    if (pwrite(p->mapfd, &h, sizeof h, 0) == (ssize_t)sizeof h)
        (void)!pwrite(p->mapfd, p->done, p->chunks * sizeof *p->done, sizeof h);
}

static size_t par_write(char *d, size_t sz, size_t n, void *u) {
    Slot *s = u;
    size_t k = sz * n;
    if (!s->checked) { /* 200 would be the whole file from byte 0: never write that into a chunk */
        long status = 0;
        curl_easy_getinfo(s->e, CURLINFO_RESPONSE_CODE, &status);
        if (status != 206) { s->p->failed = 1; return 0; }
        s->checked = 1;
    }
    if (s->pos + (off_t)k > s->end) return 0;
    for (size_t w = 0; w < k;) {
        ssize_t r = pwrite(s->p->fd, d + w, k - w, s->pos + (off_t)w);
        if (r <= 0) return 0;
        w += (size_t)r;
    }
    s->pos += (off_t)k;
    s->p->done[s->chunk] = (uint64_t)s->pos - (uint64_t)s->chunk * CHUNK;
    return k;
}

static void slot_start(CURLM *m, Slot *s, const char *url, int chunk) {
    Par *p = s->p;
    s->chunk = chunk, s->checked = 0;
    s->pos = (off_t)((uint64_t)chunk * CHUNK + p->done[chunk]);
    s->end = (off_t)((uint64_t)chunk * CHUNK + chunk_len(p, (uint32_t)chunk));
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)s->pos, (long long)s->end - 1);
    s->e = curl_easy_init();
    curl_easy_setopt(s->e, CURLOPT_URL, url);
    curl_easy_setopt(s->e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s->e, CURLOPT_RANGE, range);
    curl_easy_setopt(s->e, CURLOPT_WRITEFUNCTION, par_write);
    curl_easy_setopt(s->e, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(s->e, CURLOPT_PRIVATE, s);
    timeouts(s->e);
    pick_dns(s->e, ++s->attempt);
    p->active[chunk] = 1;
    curl_multi_add_handle(m, s->e);
}

static size_t probe_header(char *d, size_t sz, size_t n, void *u) {
    size_t k = sz * n;
    char line[256];
    size_t l = k < sizeof line - 1 ? k : sizeof line - 1;
    memcpy(line, d, l), line[l] = 0;
    const char *slash;
    if (!strncasecmp(line, "content-range:", 14) && (slash = strchr(line, '/'))) *(uint64_t *)u = strtoull(slash + 1, NULL, 10);
    return k;
}
static size_t discard(char *d, size_t sz, size_t n, void *u) { (void)d, (void)u; return sz * n; }

/* The image's size (from a one-byte range) and where its redirects end, or 0 when ranges aren't offered. */
static uint64_t probe(const char *url, char *final, size_t cap) {
    for (int attempt = 1; attempt <= 6; attempt++) {
        uint64_t total = 0;
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &total);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
        timeouts(c);
        pick_dns(c, attempt);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        char *eff = NULL;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        snprintf(final, cap, "%s", eff ? eff : url);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK) return status == 206 ? total : 0;
        sleep((unsigned)attempt);
    }
    return 0;
}

/* 1 done, 0 not possible here (use one connection), -1 failed. */
static int download_parallel(const char *url, const char *path, const char *part) {
    const char *env = getenv("HV_DL_CONNECTIONS");
    int conns = env ? atoi(env) : 8;
    if (conns > 16) conns = 16;
    char where[2048], mapname[1100];
    snprintf(mapname, sizeof mapname, "%s.map", part);
    uint64_t total = conns > 1 ? probe(url, where, sizeof where) : 0;
    if (!total) {
        /* one connection from here: a chunked .part has holes, so it can't be appended to */
        if (access(mapname, F_OK) == 0) unlink(part), unlink(mapname);
        return 0;
    }
    Par p = {.conns = conns, .total = total, .chunks = (uint32_t)((total + CHUNK - 1) / CHUNK)};
    p.done = calloc(p.chunks, sizeof *p.done);
    p.active = calloc(p.chunks, 1);
    p.retry_at = calloc(p.chunks, sizeof *p.retry_at);
    Slot *slots = calloc((size_t)conns, sizeof *slots);
    p.fd = open(part, O_RDWR | O_CREAT, 0644);
    p.mapfd = open(mapname, O_RDWR | O_CREAT, 0644);
    int result = -1;
    if (p.fd < 0 || p.mapfd < 0 || !p.done || !p.active || !p.retry_at || !slots) goto out;
    MapHead h;
    struct stat st;
    fstat(p.fd, &st);
    if (pread(p.mapfd, &h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h.magic, "HDL1", 4) && h.total == total &&
        h.chunk == CHUNK && h.chunks == p.chunks) {
        if (pread(p.mapfd, p.done, p.chunks * sizeof *p.done, sizeof h) != (ssize_t)(p.chunks * sizeof *p.done))
            memset(p.done, 0, p.chunks * sizeof *p.done);
    } else { /* a single-connection .part: its bytes are the start of the file */
        uint64_t have = (uint64_t)st.st_size < total ? (uint64_t)st.st_size : total;
        for (uint32_t i = 0; i < p.chunks; i++) {
            uint64_t start = (uint64_t)i * CHUNK;
            p.done[i] = have <= start ? 0 : have - start < chunk_len(&p, i) ? have - start : chunk_len(&p, i);
        }
    }
    for (uint32_t i = 0; i < p.chunks; i++) if (p.done[i] > chunk_len(&p, i)) p.done[i] = 0;
    if ((uint64_t)st.st_size != total && ftruncate(p.fd, (off_t)total) != 0) goto out;
    save_map(&p);
    fprintf(stderr, "henge: downloading over %d connections\n", conns);

    CURLM *m = curl_multi_init();
    curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, (long)conns);
    for (int i = 0; i < conns; i++) slots[i].p = &p;
    uint32_t next = 0;
    int running = 0, stuck = 0;
    time_t last = 0, mark_t = 0, saved = time(NULL);
    uint64_t mark = 0;
    double rate = 0;
    for (;;) {
        time_t now = time(NULL);
        /* idle slots take the next unfinished chunk that isn't in flight or waiting out a failure */
        for (int i = 0; i < conns; i++) {
            if (slots[i].e) continue;
            for (uint32_t k = 0; k < p.chunks; k++) {
                uint32_t c = (next + k) % p.chunks;
                if (p.done[c] < chunk_len(&p, c) && !p.active[c] && p.retry_at[c] <= now) {
                    slot_start(m, &slots[i], where, (int)c);
                    next = c + 1;
                    break;
                }
            }
        }
        uint64_t got = 0;
        int left = 0;
        for (uint32_t c = 0; c < p.chunks; c++) {
            got += p.done[c];
            left += p.done[c] < chunk_len(&p, c);
        }
        if (!left) break;
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int q;
        while ((msg = curl_multi_info_read(m, &q))) {
            if (msg->msg != CURLMSG_DONE) continue;
            Slot *s;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&s);
            long status = 0;
            curl_easy_getinfo(s->e, CURLINFO_RESPONSE_CODE, &status);
            CURLcode rc = msg->data.result;
            curl_multi_remove_handle(m, s->e);
            curl_easy_cleanup(s->e);
            s->e = NULL;
            p.active[s->chunk] = 0;
            if (p.failed) goto stop;
            if (rc == CURLE_OK && status == 206 && s->pos == s->end) { stuck = 0; continue; }
            if (status >= 400 && status != 416 && status != 429 && status < 500) {
                fprintf(stderr, "\nhenge: download failed (HTTP %ld)\n", status);
                goto stop;
            }
            if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
                fprintf(stderr, "\nhenge: download failed (%s)\n", curl_easy_strerror(rc));
                goto stop;
            }
            fprintf(stderr, "\nhenge: download interrupted (%s, HTTP %ld); resuming\n", curl_easy_strerror(rc), status);
            p.retry_at[s->chunk] = now + 2;
            if (++stuck > 40 * conns) { fprintf(stderr, "henge: download keeps failing; run again to resume\n"); goto stop; }
        }
        if (now != last) {
            last = now;
            if (!mark_t) mark_t = now, mark = got;
            else if (now - mark_t >= 3) {
                double r = (double)(got - mark) / (double)(now - mark_t);
                rate = rate ? rate * 0.6 + r * 0.4 : r;
                if (got > mark) stuck = 0;
                mark_t = now, mark = got;
            }
            fprintf(stderr, "\rhenge: downloading macOS %.1f / %.1f GB (%.0f%%) %.1f MB/s   ", got / 1e9, total / 1e9,
                    100.0 * (double)got / (double)total, rate / 1e6);
            if (now - saved >= 2) save_map(&p), saved = now;
        }
        curl_multi_poll(m, NULL, 0, 250, NULL);
    }
    fprintf(stderr, "\rhenge: downloading macOS %.1f / %.1f GB (100%%)   \n", total / 1e9, total / 1e9);
    result = fsync(p.fd) == 0 ? 1 : -1;
    goto done;
stop:
    result = p.failed ? 0 : -1;
done:
    for (int i = 0; i < conns; i++)
        if (slots[i].e) curl_multi_remove_handle(m, slots[i].e), curl_easy_cleanup(slots[i].e);
    curl_multi_cleanup(m);
    save_map(&p);
out:
    if (p.fd >= 0) close(p.fd);
    if (p.mapfd >= 0) close(p.mapfd);
    if (result == 1) {
        unlink(mapname);
        result = rename(part, path) == 0 ? 1 : -1;
    } else if (result == 0) {
        unlink(mapname); /* back to one connection; a map-less .part is read as contiguous, so start clean */
        if (p.chunks && p.done) {
            int contiguous = 1;
            for (uint32_t i = 0; i + 1 < p.chunks; i++) if (p.done[i] < chunk_len(&p, i) && p.done[i + 1]) contiguous = 0;
            if (!contiguous) unlink(part);
        }
    }
    free(p.done), free(p.active), free(p.retry_at), free(slots);
    return result;
}

int hv_download(const char *url, const char *path) {
    char part[1100];
    snprintf(part, sizeof part, "%s.part", path);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return 0;
    int par = download_parallel(url, path, part);
    if (par != 0) return par == 1 ? 0 : -1;
    for (int attempt = 1; attempt <= 20; attempt++) {
        Dl dl = {0};
        dl.f = fopen(part, "ab");
        if (!dl.f) return -1;
        dl.base = ftello(dl.f);
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, dl.base);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &dl);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &dl);
        timeouts(c);
        pick_dns(c, attempt);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        fclose(dl.f);
        if (rc == CURLE_OK && (status == 200 || status == 206)) {
            fprintf(stderr, "\n");
            return rename(part, path);
        }
        if (status == 416) return rename(part, path); /* already whole */
        if (rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_PEER_FAILED_VERIFICATION || (status >= 400 && status != 416)) {
            fprintf(stderr, "\nhenge: download failed (%s, HTTP %ld)\n", curl_easy_strerror(rc), status);
            return -1;
        }
        fprintf(stderr, "\nhenge: download interrupted (%s, HTTP %ld); resuming (%d)\n", curl_easy_strerror(rc), status, attempt);
        sleep((unsigned)(attempt < 10 ? attempt : 10));
    }
    return -1;
}

/* ---- a new Mac's folder ---- */

/* VZMacAuxiliaryStorage's new file: a 64-byte header, the rest zero (32 MB + 16 KB). */
static int make_aux(const char *path) {
    static const unsigned char header[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27, 1, 0, 0, 0, 0, 0, 0, 0,
                                             1, 0, 0, 0, 0, 0, 0, 0};
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, header, sizeof header) == (ssize_t)sizeof header && ftruncate(fd, 33570816) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int hv_new_mac(const char *dir, const char *name, uint64_t ram_mb, uint64_t disk_gb, uint64_t *ecid_out) {
    char p[1100];
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(p, sizeof p, "%s/disk.img", dir);
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)(disk_gb << 30)) != 0) { /* sparse: takes space as macOS writes */
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    snprintf(p, sizeof p, "%s/auxiliary.bin", dir);
    if (make_aux(p)) return -1;
    uint64_t ecid = 0;
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0 || read(rnd, &ecid, sizeof ecid) != sizeof ecid) ecid = (uint64_t)time(NULL) * 2654435761ull;
    if (rnd >= 0) close(rnd);
    ecid = (1ull << 40) + ecid % ((1ull << 62) - (1ull << 40)); /* as Installer.swift */
    snprintf(p, sizeof p, "%s/machine.json", dir);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "{\"name\":\"%s\",\"ecid\":%llu,\"seed\":0,\"ramMB\":%llu}\n", name, (unsigned long long)ecid,
            (unsigned long long)ram_mb);
    fclose(f);
    *ecid_out = ecid;
    return 0;
}
