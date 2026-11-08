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

static void ca_setup(CURL *c) {
    /* Android keeps its CAs as a hashed directory; Termux and desktop Linux have a bundle. */
    const char *bundle = getenv("CURL_CA_BUNDLE");
    if (!bundle || !*bundle) bundle = NULL;
    if (!bundle && access("/data/data/com.termux/files/usr/etc/tls/cert.pem", R_OK) == 0)
        bundle = "/data/data/com.termux/files/usr/etc/tls/cert.pem";
    if (!bundle && access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) bundle = "/etc/ssl/certs/ca-certificates.crt";
    if (bundle) {
        curl_easy_setopt(c, CURLOPT_CAINFO, bundle);
        curl_easy_setopt(c, CURLOPT_CAPATH, NULL); /* the built-in default is Android's directory */
    } else {
        curl_easy_setopt(c, CURLOPT_CAPATH, "/system/etc/security/cacerts");
    }
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
    for (int attempt = 1; attempt <= 3; attempt++) {
        Buf b = {0};
        CURL *c = curl_easy_init();
        ca_setup(c);
        curl_easy_setopt(c, CURLOPT_URL, ROM_SOURCE);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_buf);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK || status != 206 || b.n != ROM_BLOCK_LENGTH) {
            fprintf(stderr, "henge: boot ROM request failed (%s, HTTP %ld, %zu bytes)\n", curl_easy_strerror(rc), status, b.n);
            free(b.p);
            sleep((unsigned)attempt);
            continue;
        }
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

typedef struct { FILE *f; curl_off_t base; time_t last; } Dl;

static size_t to_file(char *d, size_t sz, size_t n, void *u) { return fwrite(d, sz, n, ((Dl *)u)->f) * sz; }

static int dl_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)ut, (void)un;
    Dl *dl = u;
    time_t t = time(NULL);
    if (total > 0 && t != dl->last) {
        dl->last = t;
        double all = (double)(total + dl->base), at = (double)(now + dl->base);
        fprintf(stderr, "\rhenge: downloading macOS %.1f / %.1f GB (%.0f%%)   ", at / 1e9, all / 1e9, 100 * at / all);
    }
    return 0;
}

int hv_download(const char *url, const char *path) {
    char part[1100];
    snprintf(part, sizeof part, "%s.part", path);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return 0;
    for (int attempt = 1; attempt <= 20; attempt++) {
        Dl dl = {0};
        dl.f = fopen(part, "ab");
        if (!dl.f) return -1;
        dl.base = ftello(dl.f);
        CURL *c = curl_easy_init();
        ca_setup(c);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, dl.base);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &dl);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &dl);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
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
