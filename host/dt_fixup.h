#ifndef HENGEVM_DT_FIXUP_H
#define HENGEVM_DT_FIXUP_H

#include <stdint.h>

/* The guest's device tree hardcodes two identity strings the config region
   cannot reach: /product/product-name ("Apple Virtual Machine 1", the About
   This Mac title and System Information's Model Name) and the root `model`
   ("VirtualMac2,1", hw.model and the Model Identifier). iBoot verifies the
   tree's signature once when it loads it, then fixes it up in place (serial,
   MACs, memory: the firmware's placeholders are sized for exactly that) and
   hands the same buffer to the kernel. While iBoot runs, this rewrites those
   two strings in guest RAM the same way. Nothing on disk changes.

   The window is iBoot's run: once the kernel has copied the tree into the
   IORegistry, editing the blob no longer reaches userspace. Two ways in: a
   magic scan of guest RAM early on, and — precise — the exact tree boot_args
   names (hv_dt_fixup_tree), re-patched every tick until the GPU draws, which
   catches a tree iBoot only finalises at the handoff. */

/* Copies the strings (NULL or empty: leave that one alone). Returns 1 if there
   is anything to do. ram is the guest's RAM (ram_base its physical base, for
   reading boot_args pointers), which the scan reads and writes. */
int hv_dt_fixup_start(uint8_t *ram, uint64_t ram_size, uint64_t ram_base, const char *product_name, const char *model_id);
/* Start the background scan thread (does not stall the guest's CPU). */
void hv_dt_fixup_run(void);
/* One synchronous pass (tests; the thread calls this too). */
void hv_dt_fixup_poll(void);
/* The exact tree boot_args names, as a RAM offset and length: the final blob
   iBoot hands the kernel. Re-patched every poll until hv_dt_fixup_stop.
   Returns 1 if it took the address (a valid tree is there). */
int hv_dt_fixup_tree(uint64_t offset, uint64_t len);
/* The guest's GPU has drawn: the kernel has the tree in its registry now.
   Logs what became of the rewrite. */
void hv_dt_fixup_stop(void);
/* Rewrites one blob in place; returns how many strings changed. (Also tests.) */
int hv_dt_fixup_blob(uint8_t *blob, uint64_t avail);

#endif