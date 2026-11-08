/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Texel formats the CPU path writes (clears) and the screen reads, by MTLPixelFormat number. */
#include "replay.h"

#include <math.h>
#include <string.h>

enum {
    A8Unorm = 1, R8Unorm = 10, R8Unorm_sRGB = 11, R8Snorm = 12, R8Uint = 13, R8Sint = 14,
    R16Unorm = 20, R16Snorm = 22, R16Uint = 23, R16Sint = 24, R16Float = 25,
    RG8Unorm = 30, RG8Unorm_sRGB = 31, RG8Snorm = 32, RG8Uint = 33, RG8Sint = 34,
    B5G6R5Unorm = 40, A1BGR5Unorm = 41, ABGR4Unorm = 42, BGR5A1Unorm = 43,
    R32Uint = 53, R32Sint = 54, R32Float = 55, RG16Unorm = 60, RG16Snorm = 62, RG16Uint = 63, RG16Sint = 64, RG16Float = 65,
    RGBA8Unorm = 70, RGBA8Unorm_sRGB = 71, RGBA8Snorm = 72, RGBA8Uint = 73, RGBA8Sint = 74,
    BGRA8Unorm = 80, BGRA8Unorm_sRGB = 81,
    RGB10A2Unorm = 90, RGB10A2Uint = 91, RG11B10Float = 92, RGB9E5Float = 93, BGR10A2Unorm = 94,
    RG32Uint = 103, RG32Sint = 104, RG32Float = 105,
    RGBA16Unorm = 110, RGBA16Snorm = 112, RGBA16Uint = 113, RGBA16Sint = 114, RGBA16Float = 115,
    RGBA32Uint = 123, RGBA32Sint = 124, RGBA32Float = 125,
    Depth16Unorm = 250, Depth32Float = 252, Stencil8 = 253, Depth24Stencil8 = 255, Depth32FloatStencil8 = 260,
    BGRA10_XR = 552, BGRA10_XR_sRGB = 553, BGR10_XR = 554, BGR10_XR_sRGB = 555,
};

unsigned px_bytes(uint32_t f) {
    switch (f) {
    case A8Unorm: case R8Unorm: case R8Unorm_sRGB: case R8Snorm: case R8Uint: case R8Sint: case Stencil8: return 1;
    case R16Unorm: case R16Snorm: case R16Uint: case R16Sint: case R16Float: case RG8Unorm: case RG8Unorm_sRGB:
    case RG8Snorm: case RG8Uint: case RG8Sint: case B5G6R5Unorm: case A1BGR5Unorm: case ABGR4Unorm: case BGR5A1Unorm:
    case Depth16Unorm: return 2;
    case R32Uint: case R32Sint: case R32Float: case RG16Unorm: case RG16Snorm: case RG16Uint: case RG16Sint: case RG16Float:
    case RGBA8Unorm: case RGBA8Unorm_sRGB: case RGBA8Snorm: case RGBA8Uint: case RGBA8Sint: case BGRA8Unorm: case BGRA8Unorm_sRGB:
    case RGB10A2Unorm: case RGB10A2Uint: case RG11B10Float: case RGB9E5Float: case BGR10A2Unorm: case Depth32Float:
    case Depth24Stencil8: case BGR10_XR: case BGR10_XR_sRGB: return 4;
    case RG32Uint: case RG32Sint: case RG32Float: case RGBA16Unorm: case RGBA16Snorm: case RGBA16Uint: case RGBA16Sint:
    case RGBA16Float: case Depth32FloatStencil8: case BGRA10_XR: case BGRA10_XR_sRGB: return 8;
    case RGBA32Uint: case RGBA32Sint: case RGBA32Float: return 16;
    default: return 0; /* compressed and packed video formats */
    }
}

static double clamp01(double v) { return v != v ? 0 : v < 0 ? 0 : v > 1 ? 1 : v; }
static double to_srgb(double v) { v = clamp01(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055; }
static uint8_t u8(double v) { return (uint8_t)lrint(clamp01(v) * 255); }
static uint16_t u16(double v) { return (uint16_t)lrint(clamp01(v) * 65535); }

static uint16_t half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;
    if (e == 0xff) return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    int ne = (int)e - 127 + 15;
    if (ne >= 31) return (uint16_t)(sign | 0x7c00);
    if (ne <= 0) {
        if (ne < -10) return (uint16_t)sign;
        m |= 0x800000;
        return (uint16_t)(sign | ((m >> (14 - ne)) + ((m >> (13 - ne)) & 1)));
    }
    uint32_t r = sign | (uint32_t)ne << 10 | m >> 13;
    if ((m >> 12) & 1) r++; /* round to nearest */
    return (uint16_t)r;
}

static float from_half(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (!e) {
        if (!m) x = sign;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) m <<= 1, e--; x = sign | e << 23 | (m & 0x3ff) << 13; }
    } else if (e == 31) x = sign | 0x7f800000 | m << 13;
    else x = sign | (e + 127 - 15) << 23 | m << 13;
    float f;
    memcpy(&f, &x, 4);
    return f;
}

unsigned px_encode(uint32_t f, const double c[4], uint8_t o[16]) {
    double r = c[0], g = c[1], b = c[2], a = c[3];
    switch (f) {
    case A8Unorm: o[0] = u8(a); return 1;
    case R8Unorm: o[0] = u8(r); return 1;
    case R8Unorm_sRGB: o[0] = u8(to_srgb(r)); return 1;
    case RG8Unorm: o[0] = u8(r), o[1] = u8(g); return 2;
    case RG8Unorm_sRGB: o[0] = u8(to_srgb(r)), o[1] = u8(to_srgb(g)); return 2;
    case R16Unorm: { uint16_t v = u16(r); memcpy(o, &v, 2); return 2; }
    case R16Float: { uint16_t v = half((float)r); memcpy(o, &v, 2); return 2; }
    case RG16Unorm: { uint16_t v[2] = {u16(r), u16(g)}; memcpy(o, v, 4); return 4; }
    case RG16Float: { uint16_t v[2] = {half((float)r), half((float)g)}; memcpy(o, v, 4); return 4; }
    case R32Float: { float v = (float)r; memcpy(o, &v, 4); return 4; }
    case RG32Float: { float v[2] = {(float)r, (float)g}; memcpy(o, v, 8); return 8; }
    case RGBA8Unorm: o[0] = u8(r), o[1] = u8(g), o[2] = u8(b), o[3] = u8(a); return 4;
    case RGBA8Unorm_sRGB: o[0] = u8(to_srgb(r)), o[1] = u8(to_srgb(g)), o[2] = u8(to_srgb(b)), o[3] = u8(a); return 4;
    case BGRA8Unorm: o[0] = u8(b), o[1] = u8(g), o[2] = u8(r), o[3] = u8(a); return 4;
    case BGRA8Unorm_sRGB: o[0] = u8(to_srgb(b)), o[1] = u8(to_srgb(g)), o[2] = u8(to_srgb(r)), o[3] = u8(a); return 4;
    case RGB10A2Unorm: case BGR10A2Unorm: {
        uint32_t x = (uint32_t)lrint(clamp01(f == RGB10A2Unorm ? r : b) * 1023), y = (uint32_t)lrint(clamp01(g) * 1023),
                 z = (uint32_t)lrint(clamp01(f == RGB10A2Unorm ? b : r) * 1023), w = (uint32_t)lrint(clamp01(a) * 3);
        uint32_t v = x | y << 10 | z << 20 | w << 30;
        memcpy(o, &v, 4);
        return 4;
    }
    case RGBA16Unorm: { uint16_t v[4] = {u16(r), u16(g), u16(b), u16(a)}; memcpy(o, v, 8); return 8; }
    case RGBA16Float: { uint16_t v[4] = {half((float)r), half((float)g), half((float)b), half((float)a)}; memcpy(o, v, 8); return 8; }
    case RGBA32Float: { float v[4] = {(float)r, (float)g, (float)b, (float)a}; memcpy(o, v, 16); return 16; }
    case Depth32Float: { float v = (float)r; memcpy(o, &v, 4); return 4; }
    default: return 0;
    }
}

static uint8_t srgb8_of_linear(float v) { return u8(to_srgb(v)); }

/* The scanout's fourcc, as the descriptor holds it (desc[7..4] read as characters). */
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

void px_row_to_rgba8(uint32_t fourcc, const uint8_t *s, uint8_t *d, uint32_t w) {
    if (fourcc == FOURCC('R', 'G', 'h', 'A')) { /* RGBA16Float, linear: encoded for the panel */
        static uint8_t lut[65536];
        static int ready;
        if (!ready) {
            for (uint32_t i = 0; i < 65536; i++) lut[i] = srgb8_of_linear(from_half((uint16_t)i));
            ready = 1;
        }
        const uint16_t *p = (const uint16_t *)s;
        for (uint32_t x = 0; x < w; x++, p += 4, d += 4) d[0] = lut[p[0]], d[1] = lut[p[1]], d[2] = lut[p[2]], d[3] = 255;
        return;
    }
    if (fourcc == FOURCC('l', '1', '0', 'r')) { /* ARGB2101010, little-endian */
        const uint32_t *p = (const uint32_t *)s;
        for (uint32_t x = 0; x < w; x++, d += 4) {
            uint32_t v = p[x];
            d[0] = (uint8_t)(((v >> 20) & 0x3ff) >> 2), d[1] = (uint8_t)(((v >> 10) & 0x3ff) >> 2), d[2] = (uint8_t)((v & 0x3ff) >> 2), d[3] = 255;
        }
        return;
    }
    for (uint32_t x = 0; x < w; x++, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255; /* BGRA */
}
