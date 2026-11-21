/* host/dt_fixup.c: the identity rewrite, on crafted device trees. The blob
   builder lays out a v17 tree the way the firmware's comes: root `model`,
   /product/product-name, and neighbours that must not move. */
#include "dt_fixup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t blob[8192];
static uint32_t spos, stroff, strpos, last_len_off;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void b_reset(void) {
    memset(blob, 0, sizeof blob);
    spos = 40 + 16; /* header, then one empty reservation entry */
    stroff = 4096;
    strpos = 0;
}
static void b_begin(const char *name) {
    put32(blob + spos, 1);
    spos += 4;
    uint32_t n = (uint32_t)strlen(name) + 1;
    memcpy(blob + spos, name, n);
    spos += (n + 3) & ~3u;
}
static void b_end(void) {
    put32(blob + spos, 2);
    spos += 4;
}
static void b_prop_slot(const char *name, const char *value, uint32_t slot) {
    uint32_t nameoff = strpos, nn = (uint32_t)strlen(name) + 1;
    memcpy(blob + stroff + strpos, name, nn);
    strpos += nn;
    uint32_t vn = (uint32_t)strlen(value) + 1;
    last_len_off = spos + 4;
    put32(blob + spos, 3);
    put32(blob + spos + 4, slot);
    put32(blob + spos + 8, nameoff);
    memcpy(blob + spos + 12, value, vn);
    memset(blob + spos + 12 + vn, 0, slot - vn);
    spos += 12 + ((slot + 3) & ~3u);
}
static void b_prop(const char *name, const char *value) {
    b_prop_slot(name, value, (uint32_t)strlen(value) + 1);
}
static void b_finish(void) {
    put32(blob + spos, 9);
    spos += 4;
    put32(blob + 0, 0xd00dfeed);
    put32(blob + 4, stroff + strpos); /* the strings block sits past the struct, inside totalsize */
    put32(blob + 8, 40 + 16);
    put32(blob + 12, stroff);
    put32(blob + 16, 40);
    put32(blob + 20, 17);
    put32(blob + 24, 16);
    put32(blob + 28, 0);
    put32(blob + 32, strpos);
    put32(blob + 36, spos - (40 + 16));
}
/* The firmware's tree, as far as identity goes. name_off receives the offset of
   product-name's length field, so the test can check it never moves. */
static void b_vmapple(const char *product_name, uint32_t *name_len_off) {
    b_reset();
    b_begin("");
    b_prop("model", "VirtualMac2,1");
    b_prop("compatible", "VMA2MACOSAP");
    b_begin("product");
    b_prop("product-name", product_name);
    if (name_len_off) *name_len_off = last_len_off;
    b_prop("product-soc-name", "Apple M1 (Virtual)");
    b_prop("product-description", "virtual machine for ARM-v8.4 with Apple Silicon extensions");
    b_end();
    b_begin("chosen");
    b_prop("boot-args", "");
    b_end();
    b_end();
    b_finish();
}

static void test_both(void) {
    uint32_t len_off = 0;
    b_vmapple("Apple Virtual Machine 1", &len_off);
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 2);
    CHECK(memcmp(blob + len_off + 8, "iPhone 17 Pro\0", 14) == 0);
    CHECK(get32(blob + len_off) == 24);                    /* the property keeps its length */
    for (uint32_t i = 14; i < 24; i++) CHECK(blob[len_off + 8 + i] == 0); /* NUL-padded */
    CHECK(!memmem(blob, get32(blob + 4), "Apple Virtual Machine 1", 24));
    CHECK(!memmem(blob, get32(blob + 4), "VirtualMac2,1", 12));
    CHECK(memmem(blob, get32(blob + 4), "iPhone18,1", 10));
    CHECK(memmem(blob, get32(blob + 4), "Apple M1 (Virtual)", 17)); /* the chip is the cfg region's business */
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);       /* a second pass changes nothing */
}

static void test_name_only(void) {
    b_vmapple("Apple Virtual Machine 1", NULL);
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "iPhone 17 Pro", NULL) == 1);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 1);
    CHECK(memmem(blob, get32(blob + 4), "VirtualMac2,1", 12));
}

static void test_too_long(void) {
    uint32_t len_off = 0;
    b_vmapple("Apple Virtual Machine 1", &len_off);
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "A name far too long for the slot", NULL) == 0); /* nothing fits: nothing to do */
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);
    CHECK(memcmp(blob + len_off + 8, "Apple Virtual Machine 1", 24) == 0);
}

static void test_other_tree(void) {
    b_vmapple("Some Other Machine", NULL);
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 1); /* the model is stock; the name is not ours to touch */
    CHECK(memmem(blob, get32(blob + 4), "Some Other Machine", 19));
}

static void test_garbage(void) {
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    b_reset();
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);       /* all zero */
    CHECK(hv_dt_fixup_blob(blob, 10) == 0);                /* truncated header */
    b_vmapple("Apple Virtual Machine 1", NULL);
    CHECK(hv_dt_fixup_blob(blob, 40) == 0);                /* header only */
    put32(blob + 4, 0xffffffff);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);       /* totalsize lies */
    b_vmapple("Apple Virtual Machine 1", NULL);
    put32(blob + 8, 0xffff0000);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);       /* struct offset lies */
    b_vmapple("Apple Virtual Machine 1", NULL);
    put32(blob + (40 + 16) + 4 + 4 + 4, 0xffffff00);         /* first property's length lies (BEGIN_NODE, name "", PROP) */
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 0);
}

static void test_scan(void) {
    uint64_t size = 2 << 20;
    uint8_t *ram = calloc(1, size);
    CHECK(ram != NULL);
    if (!ram) return;
    b_vmapple("Apple Virtual Machine 1", NULL);
    memcpy(ram + 0x10000, blob, get32(blob + 4));
    CHECK(hv_dt_fixup_start(ram, size, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    hv_dt_fixup_poll(); /* the first pass scans at once */
    CHECK(memmem(ram, size, "iPhone 17 Pro", 13));
    CHECK(memmem(ram, size, "iPhone18,1", 10));
    hv_dt_fixup_poll(); /* throttled: a no-op, and safe */
    CHECK(hv_dt_fixup_start(ram, size, 0, NULL, NULL) == 0);
    hv_dt_fixup_poll();
    free(ram);
}

static void test_oversized_slot(void) {
    b_reset();
    b_begin("");
    b_prop("model", "VirtualMac2,1");
    b_begin("product");
    b_prop_slot("product-name", "Apple Virtual Machine 1", 64); /* a fixed-size slot, NUL-padded */
    uint32_t len_off = last_len_off;
    b_end();
    b_end();
    b_finish();
    CHECK(hv_dt_fixup_start(blob, sizeof blob, 0, "iPhone 17 Pro", NULL) == 1);
    CHECK(hv_dt_fixup_blob(blob, sizeof blob) == 1);
    CHECK(get32(blob + len_off) == 64); /* the slot keeps its size */
    CHECK(memcmp(blob + len_off + 8, "iPhone 17 Pro\0", 14) == 0);
    for (uint32_t i = 14; i < 64; i++) CHECK(blob[len_off + 8 + i] == 0);
}

static void test_final_tree(void) {
    uint64_t size = 2 << 20;
    uint8_t *ram = calloc(1, size);
    CHECK(ram != NULL);
    if (!ram) return;
    b_vmapple("Apple Virtual Machine 1", NULL);
    uint32_t total = get32(blob + 4);
    memcpy(ram + 0x8000, blob, total);
    CHECK(hv_dt_fixup_start(ram, size, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    CHECK(hv_dt_fixup_tree(0x8000, total) == 1); /* boot_args named it */
    CHECK(hv_dt_fixup_tree(0, 0) == 1);          /* already held */
    hv_dt_fixup_poll();                          /* patches the final tree at once, no throttle */
    CHECK(memmem(ram, size, "iPhone 17 Pro", 13));
    CHECK(memmem(ram, size, "iPhone18,1", 10));
    hv_dt_fixup_stop();
    hv_dt_fixup_poll();                          /* inactive: safe */
    CHECK(hv_dt_fixup_start(ram, size, 0, "iPhone 17 Pro", NULL) == 1);
    CHECK(hv_dt_fixup_tree(0x9000, 100) == 0);   /* no tree at that address */
    free(ram);
}

static void test_string_fallback(void) {
    uint64_t size = 2 << 20;
    uint8_t *ram = calloc(1, size);
    CHECK(ram != NULL);
    if (!ram) return;
    memcpy(ram + 0x50000, "Apple Virtual Machine 1\0", 24); /* the string, but no tree magic anywhere */
    CHECK(hv_dt_fixup_start(ram, size, 0, "iPhone 17 Pro", NULL) == 1);
    hv_dt_fixup_poll();
    CHECK(memmem(ram, size, "iPhone 17 Pro", 13));
    CHECK(!memmem(ram, size, "Apple Virtual Machine 1", 24));
    free(ram);
}

static void test_string_tree(void) {
    uint64_t size = 2 << 20;
    uint8_t *ram = calloc(1, size);
    CHECK(ram != NULL);
    if (!ram) return;
    b_vmapple("Apple Virtual Machine 1", NULL);
    uint32_t total = get32(blob + 4);
    memcpy(ram + 0x10004, blob, total); /* 4-byte aligned: neither the page nor the 16-byte scan sees it */
    CHECK(hv_dt_fixup_start(ram, size, 0, "iPhone 17 Pro", "iPhone18,1") == 1);
    hv_dt_fixup_poll(); /* the string anchor walks back to the magic and patches */
    CHECK(memmem(ram, size, "iPhone 17 Pro", 13));
    CHECK(memmem(ram, size, "iPhone18,1", 10));
    free(ram);
}

/* Apple's ADT: nodes of {u32 prop_count, u32 child_count}, properties of
   {char name[32], u32 size, value}. No magic. (m1n1 adt.py.) */
static uint8_t adt[8192];
static uint32_t apos;
static void a_u32(uint32_t v) {
    adt[apos] = (uint8_t)v;
    adt[apos + 1] = (uint8_t)(v >> 8);
    adt[apos + 2] = (uint8_t)(v >> 16);
    adt[apos + 3] = (uint8_t)(v >> 24);
    apos += 4;
}
static void a_prop(const char *name, const char *value, uint32_t size) {
    memset(adt + apos, 0, 32);
    memcpy(adt + apos, name, strlen(name));
    apos += 32;
    a_u32(size);
    memcpy(adt + apos, value, size);
    apos += (size + 3) & ~3u;
}
static void build_adt(void) {
    apos = 0;
    memset(adt, 0, sizeof adt);
    a_u32(2); /* root: two properties */
    a_u32(1); /* one child */
    a_prop("model", "VirtualMac2,1\0", 14);
    a_prop("compatible", "VirtualMac2,1\0AppleVirtualPlatformARM\0", 38); /* must NOT change */
    a_u32(1); /* product child: one property */
    a_u32(0);
    a_prop("product-name", "Apple Virtual Machine 1\0", 24);
}

static void test_adt(void) {
    build_adt();
    CHECK(hv_dt_fixup_start(adt, sizeof adt, 0, "iPhone 18 Pro Max", "iPhone19,7") == 1);
    CHECK(hv_dt_fixup_tree(0, apos) == 1); /* parses as an ADT, patches at once */
    CHECK(memmem(adt, apos, "iPhone19,7", 10));            /* model rewritten */
    CHECK(memmem(adt, apos, "iPhone 18 Pro Max", 17));     /* product-name rewritten */
    CHECK(!memmem(adt, apos, "Apple Virtual Machine 1", 23));
    CHECK(memmem(adt, apos, "VirtualMac2,1", 13));         /* compatible keeps it: driver matching safe */
    CHECK(memmem(adt, apos, "AppleVirtualPlatformARM", 23));
    int again = 0;
    CHECK(hv_dt_fixup_tree(0, apos) == 1);                 /* already held */
    (void)again;
}

int main(void) {
    test_both();
    test_name_only();
    test_too_long();
    test_other_tree();
    test_garbage();
    test_scan();
    test_oversized_slot();
    test_final_tree();
    test_string_fallback();
    test_string_tree();
    test_adt();
    if (failures) fprintf(stderr, "test_dt_fixup: %d failure%s\n", failures, failures == 1 ? "" : "s");
    else printf("test_dt_fixup: all pass\n");
    return failures ? 1 : 0;
}