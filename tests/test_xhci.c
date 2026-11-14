/* The xHCI keyboard, and with HV_USB_TABLET=1 the absolute pointer on port 2 (0.3.60: its own device), driven
   the way a guest's driver does: rings and contexts in guest memory, registers through the BAR, events read back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "gicv3.h"
#include "pcie.h"
#include "xhci.h"

#define RAM 0x40000000ull
#define DCBAA (RAM + 0x0000)
#define OUTCTX (RAM + 0x1000)
#define INCTX (RAM + 0x2000)
#define CMDRING (RAM + 0x3000)
#define EVRING (RAM + 0x4000)
#define ERST (RAM + 0x5000)
#define EP0RING (RAM + 0x6000)
#define EP2RING (RAM + 0x7000)
#define BUF (RAM + 0x8000)
#define REPORT (RAM + 0x9000)
#define OUTCTX2 (RAM + 0xa000)
#define EP0RING2 (RAM + 0xb000)

static uint8_t *ram;
static int checks, failures;
static uint32_t ev_index;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) { printf("  FAIL " __VA_ARGS__); printf("\n"); failures++; }                                     \
    } while (0)

static uint8_t *at(uint64_t pa) { return ram + (pa - RAM); }
static void put32(uint64_t pa, uint32_t v) { memcpy(at(pa), &v, 4); }
static void put64(uint64_t pa, uint64_t v) { memcpy(at(pa), &v, 8); }
static uint32_t get32(uint64_t pa) { uint32_t v; memcpy(&v, at(pa), 4); return v; }

static void trb(uint64_t pa, uint64_t param, uint32_t status, uint32_t control) {
    put64(pa, param), put32(pa + 8, status), put32(pa + 12, control | 1); /* cycle 1 */
}

/* The next event the controller wrote (cycle 1, first pass of the ring); its type, or -1. */
static int next_event(uint32_t ev[4]) {
    uint64_t pa = EVRING + 16ull * ev_index;
    for (int i = 0; i < 4; i++) ev[i] = get32(pa + 4 * i);
    if (!(ev[3] & 1)) return -1;
    ev_index++;
    return (int)((ev[3] >> 10) & 0x3f);
}

static void w(HvXhci *x, uint64_t off, uint64_t v, unsigned size) { x->pci.bar_write(&x->pci, 0, off, v, size); }

static int command(HvXhci *x, uint32_t *n, uint64_t param, uint32_t control) {
    trb(CMDRING + 16ull * (*n)++, param, 0, control);
    w(x, 0x2000, 0, 4); /* doorbell 0 */
    uint32_t ev[4];
    int t;
    while ((t = next_event(ev)) >= 0)
        if (t == 33) return (int)(ev[2] >> 24) == 1 ? (int)(ev[3] >> 24) : -(int)(ev[2] >> 24);
    return -100;
}

/* A control IN transfer on endpoint 0: SETUP, DATA into BUF, STATUS. Returns the bytes received or -1. */
static uint64_t ep0ring = EP0RING;

static int control_in(HvXhci *x, int slot, uint32_t *n, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len) {
    const uint64_t EP0 = ep0ring;
    uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    uint64_t s;
    memcpy(&s, setup, 8);
    memset(at(BUF), 0, 256);
    trb(EP0 + 16ull * (*n)++, s, 8, 2u << 10 | 1u << 6 | 3u << 16);           /* SETUP, IDT, IN data stage */
    trb(EP0 + 16ull * (*n)++, BUF, len, 3u << 10 | 1u << 16 | 1u << 2 | 1u << 5); /* DATA, IN, ISP, IOC */
    trb(EP0 + 16ull * (*n)++, 0, 0, 4u << 10 | 1u << 5);                       /* STATUS, IOC */
    w(x, 0x2000 + 4u * (unsigned)slot, 1, 4);
    uint32_t ev[4];
    int t, got = -1;
    while ((t = next_event(ev)) >= 0)
        if (t == 32 && ((ev[3] >> 16) & 0x1f) == 1 && ev[0] == (uint32_t)(EP0 + 16ull * (*n - 2)))
            got = (int)len - (int)(ev[2] & 0xffffff);
    return got;
}

/* A controller running, its ports' changes read. */
static void controller_up(HvXhci *x) {
    memset(ram, 0, 0x10000);
    ev_index = 0;
    put64(ERST, EVRING), put32(ERST + 8, 64);
    w(x, 0x1000 + 0x20 + 0x08, 1, 4);    /* ERSTSZ */
    w(x, 0x1000 + 0x20 + 0x10, ERST, 8); /* ERSTBA */
    w(x, 0x1000 + 0x20 + 0x00, 2, 4);    /* IMAN: IE */
    w(x, 0x40 + 0x30, DCBAA, 8);         /* DCBAAP */
    w(x, 0x40 + 0x18, CMDRING | 1, 8);   /* CRCR */
    w(x, 0x40 + 0x00, 1 | 4, 4);         /* run, INTE */
    uint32_t ev[4];
    while (next_event(ev) >= 0) {}
}

/* The device on a root port given a slot and an address. Returns the slot. */
static int bring_up(HvXhci *x, int port, uint64_t out, uint64_t ep0, uint32_t *cmd_n) {
    int slot = command(x, cmd_n, 0, 9u << 10);
    CHECK(slot >= 1, "port %d: enable slot: %d", port, slot);
    if (slot < 1) return -1;
    put64(DCBAA + 8ull * slot, out);
    memset(at(INCTX), 0, 0x400);
    put32(INCTX + 4, 3);                          /* add: slot, EP0 */
    at(INCTX + 32)[6] = (uint8_t)port;            /* root port */
    put64(INCTX + 64 + 8, ep0 | 1);               /* EP0 dequeue */
    int r = command(x, cmd_n, INCTX, 11u << 10 | (uint32_t)slot << 24);
    CHECK(r == slot, "port %d: address device: %d", port, r);
    return slot;
}

static uint32_t rd_bar(HvXhci *x, uint64_t off) { return (uint32_t)x->pci.bar_read(&x->pci, 0, off, 4); }

int main(void) {
    static HvMachine m;
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = aligned_alloc(65536, 0x10000);
    hv_machine_add_ram(&m, RAM, 0x10000, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x50000000, 0x10000000, 0x60000000, 0x10000000, 35);

    /* The keyboard alone (no HV_USB_TABLET): the configuration of before, one port. */
    unsetenv("HV_USB_TABLET");
    HvXhci *k = hv_xhci_create(&m, bus);
    uint32_t cn = 0, en = 0;
    controller_up(k);
    CHECK((rd_bar(k, 4) >> 24) == 1, "keyboard alone: %u ports, 1 expected", rd_bar(k, 4) >> 24);
    int slot = bring_up(k, 1, OUTCTX, EP0RING, &cn);
    int n = control_in(k, slot, &en, 0x80, 6, 0x0200, 0, 255);
    CHECK(n == 34 && at(BUF)[4] == 1, "keyboard alone: configuration %d bytes, %u interfaces", n, at(BUF)[4]);
    hv_xhci_pointer(k, 100, 100, 1); /* no tablet: nothing */
    CHECK(k->ptail == 0 && !hv_xhci_pointer_live(k), "keyboard alone took a pointer report");
    int s2 = command(k, &cn, 0, 9u << 10);
    memset(at(INCTX), 0, 0x400);
    put32(INCTX + 4, 3);
    at(INCTX + 32)[6] = 2;
    put64(DCBAA + 8ull * s2, OUTCTX2);
    CHECK(command(k, &cn, INCTX, 11u << 10 | (uint32_t)s2 << 24) == -5, "keyboard alone: a device addressed on port 2");

    /* With the tablet: two ports; the keyboard on port 1 as before, the tablet its own device on port 2. */
    setenv("HV_USB_TABLET", "1", 1);
    HvXhci *x = hv_xhci_create(&m, bus);
    cn = en = 0;
    controller_up(x);
    CHECK((rd_bar(x, 4) >> 24) == 2 && !(rd_bar(x, 0x440 + 16) & 1), "tablet: %u ports, port 2 connected at once %u", rd_bar(x, 4) >> 24,
          rd_bar(x, 0x440 + 16) & 1);
    CHECK(x->ptr_plug_at != 0, "tablet: no time set to plug it in once the controller runs");
    x->ptr_plug_at = 1; /* due: plugged in at the next poll, as the driver would see a device plugged in */
    hv_xhci_poll(x);
    uint32_t pev[4];
    int pt, port2 = 0;
    while ((pt = next_event(pev)) >= 0)
        if (pt == 34 && (pev[0] >> 24) == 2) port2 = 1;
    CHECK((rd_bar(x, 0x440 + 16) & 1) && port2, "tablet: port 2 plugged in (connected %u, its change event %d)", rd_bar(x, 0x440 + 16) & 1, port2);
    int ks = bring_up(x, 1, OUTCTX, EP0RING, &cn);
    n = control_in(x, ks, &en, 0x80, 6, 0x0200, 0, 255);
    CHECK(n == 34 && at(BUF)[4] == 1, "tablet: the keyboard's configuration unchanged (%d bytes)", n);
    n = control_in(x, ks, &en, 0x80, 6, 0x0100, 0, 18);
    CHECK(n == 18 && at(BUF)[8] == 0xac && at(BUF)[9] == 0x05, "tablet: the keyboard is still Apple's");

    uint32_t en2 = 0;
    ep0ring = EP0RING2;
    slot = bring_up(x, 2, OUTCTX2, EP0RING2, &cn);
    CHECK(slot != ks, "tablet: its own slot (%d, keyboard %d)", slot, ks);
    n = control_in(x, slot, &en2, 0x80, 6, 0x0100, 0, 18);
    CHECK(n == 18 && at(BUF)[8] == 0x27 && at(BUF)[9] == 0x06 && at(BUF)[10] == 1, "tablet: QEMU's tablet IDs");
    n = control_in(x, slot, &en2, 0x80, 6, 0x0200, 0, 255);
    CHECK(n == 34 && at(BUF)[4] == 1 && at(BUF)[9 + 5] == 3 && at(BUF)[9 + 7] == 0 && at(BUF)[27 + 2] == 0x81,
          "tablet: configuration %d bytes, one HID interface, endpoint %02x", n, at(BUF)[29]);
    n = control_in(x, slot, &en2, 0x81, 6, 0x2200, 0, 255);
    CHECK(n == 74 && at(BUF)[0] == 0x05 && at(BUF)[3] == 0x02 && at(BUF)[n - 1] == 0xc0, "tablet: report descriptor %d bytes", n);
    n = control_in(x, slot, &en2, 0x81, 6, 0x2100, 0, 9);
    CHECK(n == 9 && at(BUF)[1] == 0x21 && at(BUF)[7] == 74, "tablet: HID descriptor");
    n = control_in(x, slot, &en2, 0x80, 6, 0x0305, 0, 255);
    CHECK(n == 2 + 2 * 15 && at(BUF)[2] == 'Q', "tablet: product string (%d bytes)", n);
    ep0ring = EP0RING;
    n = control_in(x, ks, &en, 0x81, 6, 0x2200, 0, 255);
    CHECK(n == 64 && at(BUF)[3] == 0x06, "tablet: the keyboard's report descriptor still its own (%d bytes)", n);

    /* Endpoint 1 IN configured, a transfer posted, then the pointer moves and a button goes down. */
    CHECK(!hv_xhci_pointer_live(x), "tablet: live before macOS reads it");
    memset(at(INCTX), 0, 0x400);
    put32(INCTX + 4, 1u << 3 | 1);
    put32(INCTX + 32ull * 4 + 4, 3u << 3 | 7u << 3);   /* interrupt IN */
    put64(INCTX + 32ull * 4 + 8, EP2RING | 1);
    int r = command(x, &cn, INCTX, 12u << 10 | (uint32_t)slot << 24);
    CHECK(r == slot, "configure endpoint 1: %d", r);
    uint32_t pn = 0;
    trb(EP2RING + 16ull * pn++, REPORT, 8, 1u << 10 | 1u << 5 | 1u << 2); /* NORMAL, IOC, ISP */
    w(x, 0x2000 + 4u * (unsigned)slot, 3, 4);
    CHECK(hv_xhci_pointer_live(x), "tablet: not live with a transfer posted");
    hv_xhci_pointer(x, 16384, 8192, 1);
    hv_xhci_poll(x);
    const uint8_t want[6] = {1, 0x00, 0x40, 0x00, 0x20, 0};
    CHECK(!memcmp(at(REPORT), want, 6), "tablet: report %02x %02x %02x %02x %02x %02x", at(REPORT)[0], at(REPORT)[1],
          at(REPORT)[2], at(REPORT)[3], at(REPORT)[4], at(REPORT)[5]);
    uint32_t ev[4];
    int t, seen = 0;
    while ((t = next_event(ev)) >= 0)
        if (t == 32 && ((ev[3] >> 16) & 0x1f) == 3 && (int)(ev[3] >> 24) == slot) seen = (int)(ev[2] >> 24);
    CHECK(seen == 13, "tablet: transfer event on its endpoint 1 (code %d, a short packet expected)", seen);
    CHECK(x->pdelivered == 1, "tablet: %llu reports delivered", (unsigned long long)x->pdelivered);

    /* A key goes to the keyboard's slot, not the tablet's. */
    memset(at(INCTX), 0, 0x400);
    put32(INCTX + 4, 1u << 3 | 1);
    put32(INCTX + 32ull * 4 + 4, 3u << 3 | 7u << 3);
    put64(INCTX + 32ull * 4 + 8, (RAM + 0xc000) | 1);
    r = command(x, &cn, INCTX, 12u << 10 | (uint32_t)ks << 24);
    trb(RAM + 0xc000, RAM + 0xd000, 8, 1u << 10 | 1u << 5);
    w(x, 0x2000 + 4u * (unsigned)ks, 3, 4);
    hv_xhci_key(x, 4, 1);
    hv_xhci_poll(x);
    seen = 0;
    while ((t = next_event(ev)) >= 0)
        if (t == 32 && ((ev[3] >> 16) & 0x1f) == 3) seen = (int)(ev[3] >> 24);
    CHECK(seen == ks && at(RAM + 0xd000)[2] == 4, "tablet: a key went to slot %d (keyboard %d), usage %u", seen, ks, at(RAM + 0xd000)[2]);

    /* Moves with no transfer posted: the newest replaces the one waiting; a button change queues its own. */
    hv_xhci_pointer(x, 1, 1, 1);
    hv_xhci_pointer(x, 2, 2, 1);
    hv_xhci_pointer(x, 3, 3, 1);
    hv_xhci_pointer(x, 3, 3, 0);
    CHECK(x->ptail - x->phead == 2, "tablet: %u reports waiting, 2 expected (the newest move, the release)", x->ptail - x->phead);
    hv_xhci_pointer(x, 99999, -5, 0);
    CHECK(x->pstate[1] == 0xff && x->pstate[2] == 0x7f && x->pstate[3] == 0 && x->pstate[4] == 0, "tablet: clamped to 0..32767");

    /* The slot disabled: no longer live (the mouse is steered again). */
    command(x, &cn, 0, 10u << 10 | (uint32_t)slot << 24);
    CHECK(!hv_xhci_pointer_live(x), "tablet: live after its slot went away");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
