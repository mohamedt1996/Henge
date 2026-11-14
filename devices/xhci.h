#ifndef HENGEVM_XHCI_H
#define HENGEVM_XHCI_H

#include <pthread.h>

#include "pcie.h"

/* An xHCI USB host controller (xHCI 1.0, one interrupter, 32-byte contexts,
   INTx) with a USB HID boot keyboard on port 1: what Virtualization.framework
   gives a macOS guest as its "Virtual USB Keyboard", and so the keyboard path
   macOS takes. Key changes come from the host UI on any thread
   (hv_xhci_key); the CPU thread delivers them (hv_xhci_poll).
   With HV_USB_TABLET=1 in the environment port 2 has a second device, an absolute pointer: QEMU's
   usb-tablet (its IDs, descriptors and report, which macOS drives with its generic HID pointer driver), so
   a host pointer can put the Mac's exactly where it is (hv_xhci_pointer) instead of steering a relative
   mouse. 0.3.59 had it as interface 1 of the keyboard, which macOS left alone. */
#define HV_XHCI_SLOTS 8
#define HV_XHCI_PORTS 2 /* port 1 the keyboard, port 2 the tablet (connected with HV_USB_TABLET=1) */
#define HV_XHCI_INTRS 4
#define HV_XHCI_EPS 32 /* device context index 1..31 */
#define HV_XHCI_REPORTS 256

typedef struct HvXhciRing {
    uint64_t dequeue; /* guest PA of the next TRB */
    int ccs;          /* consumer cycle state */
} HvXhciRing;

/* A device on a root port: its slot and endpoint rings. */
typedef struct HvXhciDev {
    int slot;                     /* slot it was addressed in, or 0 */
    int configured;               /* SET_CONFIGURATION */
    HvXhciRing ep[HV_XHCI_EPS];
    int ep_running[HV_XHCI_EPS];
} HvXhciDev;

typedef struct HvXhci {
    HvPciDev pci;
    HvMachine *m;
    /* operational */
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t dcbaap;
    HvXhciRing cmd;
    int cmd_running;
    uint32_t portsc[HV_XHCI_PORTS];
    /* interrupters and their event rings (macOS's driver wants at least three) */
    struct HvXhciIntr {
        uint32_t iman, imod, erstsz;
        uint64_t erstba, erdp;
        uint64_t seg_base;  /* current event ring segment */
        uint32_t seg_size, seg_index, index;
        int pcs;
    } intr[HV_XHCI_INTRS];
    int nports;            /* 1, or 2 with the tablet */
    /* device slots: dev[0] the keyboard (port 1), dev[1] the tablet (port 2) */
    int slot_used[HV_XHCI_SLOTS + 1];
    int slot_dev[HV_XHCI_SLOTS + 1]; /* 1 + the device addressed in the slot, or 0 */
    HvXhciDev dev[HV_XHCI_PORTS];
    /* keyboard reports waiting for an IN transfer (8 bytes each) */
    pthread_mutex_t mu;
    uint8_t reports[HV_XHCI_REPORTS][8];
    uint32_t rhead, rtail;
    uint8_t state[8];      /* modifiers, reserved, six keys: the current report */
    uint64_t delivered;
    /* the absolute pointer (port 2, endpoint 1 IN), when HV_USB_TABLET=1: buttons, x, y (0..32767), wheel */
    int tablet;
    int ptr_posted;        /* the guest has posted a transfer for reports: it is reading them */
    uint8_t preports[HV_XHCI_REPORTS][6];
    uint32_t phead, ptail;
    uint8_t pstate[6];
    uint64_t pdelivered;
} HvXhci;

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus);
/* Any thread: a key went down or up (HID usage page 7 usage, e.g. 4 = A; 0xe0-0xe7 are modifiers). */
void hv_xhci_key(HvXhci *x, uint8_t usage, int down);
/* The same, by Linux input key code (what the host UI produces). */
void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down);
/* Any thread, HV_USB_TABLET=1 only: the absolute pointer at x, y (0..32767 across the Mac's screen) with
   buttons (bit 0 left, 1 right, 2 middle). */
void hv_xhci_pointer(HvXhci *x, int ax, int ay, int buttons);
/* Any thread: the tablet is there and macOS reads its reports (until then a host pointer steers the mouse). */
int hv_xhci_pointer_live(HvXhci *x);
/* CPU thread: hand waiting reports to the guest. */
void hv_xhci_poll(HvXhci *x);

#endif
