#pragma once
/* Linux/Android stand-in for Apple's libcompression, as host/snapshot.c uses it: one-shot buffers with
   COMPRESSION_LZ4. Backed by the LZ4 block format (third_party/lz4). An instant-resume file is only ever read by the
   build that wrote it (snapshot.c checks), so the format need not match Apple's framed LZ4. */
#include <stddef.h>
#include <stdint.h>

#include "../../third_party/lz4/lz4.h"

typedef int compression_algorithm;
#define COMPRESSION_LZ4 0x100

static inline size_t compression_encode_scratch_buffer_size(compression_algorithm a) { (void)a; return (size_t)LZ4_sizeofState(); }
static inline size_t compression_decode_scratch_buffer_size(compression_algorithm a) { (void)a; return 1; }

/* 0 if it does not fit in dst_size (the caller then stores the data raw), as Apple's does. */
static inline size_t compression_encode_buffer(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size, void *scratch,
                                               compression_algorithm a) {
    (void)a;
    if (src_size > (size_t)LZ4_MAX_INPUT_SIZE || dst_size > 0x7fffffff) return 0;
    int n = scratch ? LZ4_compress_fast_extState(scratch, (const char *)src, (char *)dst, (int)src_size, (int)dst_size, 1)
                    : LZ4_compress_default((const char *)src, (char *)dst, (int)src_size, (int)dst_size);
    return n > 0 ? (size_t)n : 0;
}

static inline size_t compression_decode_buffer(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size, void *scratch,
                                               compression_algorithm a) {
    (void)scratch, (void)a;
    if (src_size > 0x7fffffff || dst_size > 0x7fffffff) return 0;
    int n = LZ4_decompress_safe((const char *)src, (char *)dst, (int)src_size, (int)dst_size);
    return n > 0 ? (size_t)n : 0;
}
