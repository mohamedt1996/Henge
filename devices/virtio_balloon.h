#ifndef HENGEVM_VIRTIO_BALLOON_H
#define HENGEVM_VIRTIO_BALLOON_H

#include <stdint.h>

#include "virtio.h"

/* virtio memory balloon (device type 5), which macOS drives with AppleVirtIOBalloon. The host asks for a
   number of pages; the guest's kernel takes that many from its own free memory (dropping caches if it must)
   and says which; their host pages go back to the OS. Guest RAM is real memory on the phone, outside Henge's
   footprint, so this is the one way to get it back from a running Mac when iOS runs short.
   Pages are 4 KB in the protocol (num_pages, actual and the page numbers), 16 KB on the host: a host page is
   given back once all four of its quarters are in the balloon. */
typedef struct HvVirtioBalloon {
    HvVirtio vio;
    uint8_t *ram;
    uint64_t ram_pa, ram_size;
    int ram_fd;                  /* the file under guest RAM (holes are punched in it), or -1: madvise */
    uint32_t target;             /* 4 KB pages wanted, set from any thread; applied under the device lock */
    uint32_t applied;            /* the target the guest was last told */
    /* Counters (16 KB host pages unless said otherwise) */
    uint64_t inflated_pfns, deflated_pfns, given_back, taken_back, partial, out_of_ram, failed;
    int told_pattern;
} HvVirtioBalloon;

/* ram_fd: the guest RAM's file, or -1 for anonymous memory. */
HvVirtioBalloon *hv_virtio_balloon_create(HvMachine *m, HvPcie *bus, uint8_t *ram, uint64_t ram_pa, uint64_t ram_size, int ram_fd);
/* Any thread: how many bytes the guest should hand over (0 = all back). */
void hv_virtio_balloon_set_target(HvVirtioBalloon *b, uint64_t bytes);
/* Bytes the guest says it has handed over (its "actual"). */
uint64_t hv_virtio_balloon_actual(const HvVirtioBalloon *b);
/* Device lock held (the machine tick): tells the guest about a new target. */
void hv_virtio_balloon_poll(HvVirtioBalloon *b);

#endif
