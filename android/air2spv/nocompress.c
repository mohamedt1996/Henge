/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The tablet's air2spv links LLVM statically without zlib or zstd: LLVM's Support library refers to
   them for compressed sections and archives, which reading AIR bitcode never uses. Every call fails
   as an unavailable compressor would; crc32 is real (LLVM's JamCRC uses it). */
#include <stddef.h>
#include <stdint.h>

unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    if (!buf) return 0;
    uint32_t c = (uint32_t)crc ^ 0xffffffffu;
    while (len--) c = table[(c ^ *buf++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

unsigned long compressBound(unsigned long n) { return n + n / 1000 + 64; }
int compress2(unsigned char *d, unsigned long *dn, const unsigned char *s, unsigned long sn, int level) {
    (void)d, (void)dn, (void)s, (void)sn, (void)level;
    return -6; /* Z_VERSION_ERROR: no zlib here */
}
int uncompress(unsigned char *d, unsigned long *dn, const unsigned char *s, unsigned long sn) {
    (void)d, (void)dn, (void)s, (void)sn;
    return -6;
}

#define ZSTD_ERROR ((size_t)-1)
size_t ZSTD_compressBound(size_t n) { return n + (n >> 8) + 64; }
unsigned ZSTD_isError(size_t code) { return code > (size_t)-120; }
const char *ZSTD_getErrorName(size_t code) { (void)code; return "zstd is not in this build"; }
void *ZSTD_createCCtx(void) { return NULL; }
size_t ZSTD_freeCCtx(void *c) { (void)c; return 0; }
size_t ZSTD_CCtx_setParameter(void *c, int p, int v) { (void)c, (void)p, (void)v; return ZSTD_ERROR; }
size_t ZSTD_compress2(void *c, void *d, size_t dn, const void *s, size_t sn) { (void)c, (void)d, (void)dn, (void)s, (void)sn; return ZSTD_ERROR; }
size_t ZSTD_decompress(void *d, size_t dn, const void *s, size_t sn) { (void)d, (void)dn, (void)s, (void)sn; return ZSTD_ERROR; }
