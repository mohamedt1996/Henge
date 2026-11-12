/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: applegfx — a second, independent paravirtual-GPU host, so the GPU is not bet on reims alone. It
 * drives libapplegfx-vulkan (MattJackson/libapplegfx-vulkan, MIT): a clean-room host implementation of Apple's
 * ParavirtualizedGraphics with its own AIR -> SPIR-V translator and a Vulkan backend.
 *
 * Built with HAVE_APPLEGFX once the library is cross-built for the tablet (android/applegfx/build.sh, a CI step
 * like reims/build.sh). vmapple.c tries this before reims; both gate on their own HV_GPU_* variable.
 *
 * ========================= The MMIO <-> PCI transport bridge (android/applegfx/BRIDGE.md) ==================
 * libapplegfx-vulkan was written against Apple's *PCI* paravirtual-GPU transport (QEMU hw/display/apple-gfx-pci.m):
 * one PGDevice behind a single PCI BAR, interrupts delivered as MSI vectors. Henge's vmapple exposes the *MMIO*
 * transport that Apple-silicon macOS actually uses (QEMU hw/display/apple-gfx-mmio.m): the SAME PGDevice register
 * window (`iomem_gfx`) plus a SECOND window and device, the PGIOSurfaceHostDevice (`iomem_iosfc`), and two GIC SPI
 * lines instead of MSI.
 *
 * Evidence (QEMU sources, read directly) and the resulting mapping:
 *   - apple-gfx-pci.m registers exactly ONE BAR: `pci_register_bar(dev, PG_PCI_BAR_MMIO, ..., &s->common.iomem_gfx)`.
 *   - apple-gfx-mmio.m exposes `s->common.iomem_gfx` as sysbus MMIO #0 (the GFX window) AND a separate
 *     `s->iomem_iosfc` (0x10000) as MMIO #1 (the IOSFC window), backed by a distinct PGIOSurfaceHostDevice.
 *   - Both variants route the GFX window through the identical `apple_gfx_ops` -> `[pgdev mmioReadAtOffset:]`.
 *     The register offsets are therefore IDENTICAL across transports. => The GFX window maps 1:1 onto the
 *     library's `lagfx_mmio_read/write(dev, offset)` with NO offset translation. (The library reserves BAR
 *     0x0..0xfff for the PCI MSI-X table and keeps the real registers at 0x1000+; on MMIO that low 4 KiB is
 *     simply never touched because interrupts come over the GIC, so the identity mapping is exact.)
 *   - apple-gfx-pci.m delivers interrupts with `msi_notify(dev, vector)`; the PGDevice's raiseInterrupt block is
 *     the ONLY interrupt source. apple-gfx-mmio.m pulses `s->irq_gfx` from that same PGDevice block, and pulses a
 *     SEPARATE `s->irq_iosfc` only from the PGIOSurfaceHostDevice's own raiseInterrupt. => every interrupt the
 *     library raises (lagfx shell `raise_interrupt`, always MSI vector 0 for stamps/displays) belongs to the GFX
 *     line. The IOSFC line is owned by the IOSurface host device, which the library does not implement.
 *
 * What is wired here:
 *   - GFX window (VMAPPLE_GFX): each guest MMIO access is forwarded verbatim to lagfx_mmio_read/write. 8-byte
 *     accesses are split into the library's native 4-byte register ops.
 *   - Interrupts: the lagfx raise_interrupt callback edges the GFX GIC line (hv_gicv3_pend), waking a 1-CPU guest.
 *   - A ~60 Hz pump thread calls lagfx_timer_tick_vblank (display online/vblank IRQ) and lagfx_display_read_frame,
 *     publishing the BGRA frame through hv_pvg_applegfx_frame() which hv_vm_boot_display() presents (the same
 *     route pvg_lite uses).
 *   - The library's shell callbacks remain backed by the real engine: guest RAM (hv_ram_lookup) for
 *     read/write/map, and the library's own memfd+mmap task primitives.
 *
 * IOSFC window (VMAPPLE_IOSFC / PGIOSurfaceHostDevice): on Apple silicon macOS drives IOSurface sharing through
 *   THIS second MMIO device (apple-gfx-mmio sets usingIOSurfaceMapper=true), not through the FIFO IOSurface
 *   opcodes (0x26-0x2a) the library implements for the PCI transport. libapplegfx-vulkan has no counterpart for
 *   it. This file now models the device end to end — reverse-engineered from steelbrain/reims-vgpu's iosurface
 *   module (the working ground truth) and QEMU's apple-gfx-mmio.m: the 64-bit register file, the command ring
 *   and doorbell, map/unmap/test, the one- and two-level descriptor page-table walk, the guest "mapped" bit
 *   write-back, and the per-command completion IRQ on the IOSFC GIC line. See the IOSFC block below and
 *   android/applegfx/BRIDGE.md.
 *
 * Renderer handoff: each successful `map` is handed to the library via lagfx_iosurface_register (and released
 *   on `unmap` via lagfx_iosurface_unregister) — the entry point stock libapplegfx-vulkan lacked, added here as
 *   a Henge-local patch (android/applegfx/patches/, applied by build.sh). It binds the surface as a TEXTURE
 *   keyed on mapping_id so the library's existing CmdLookupIOSurface path resolves it. See BRIDGE.md.
 * Freshness: a surface the guest keeps redrawing is re-uploaded from its still-mapped pages every vblank by
 *   iosfc_refresh_surfaces() -> lagfx_iosurface_refresh() (re-upload into the existing VkImage, no recreate), so
 *   the compositor framebuffer tracks the guest's drawing rather than freezing at the map-time snapshot.
 * What is STILL open (see BRIDGE.md): Vulkan is enabled only when the library is built with it (build.sh
 *   APPLEGFX_VULKAN=1, which also makes the engine link dynamically against the tablet's Vulkan driver). The
 *   default static CI build is Vulkan-off, where register/refresh record the mapping without building an image.
 *
 * Because of that, the bridge is engaged but honest: it logs its experimental status and points at Graphics:
 * reims for the known-working GPU. Set HV_GPU_APPLEGFX_PROBE_ONLY=1 to keep the old behaviour (prove the library
 * initialises on-device, then fall back to pvg_lite without touching the MMIO windows).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "machine.h"
#include "gicv3.h"
#include "vmapple.h"

#if defined(HAVE_APPLEGFX)
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "libapplegfx-vulkan.h"

/* ======================================================================================= *
 * IOSFC window: a host model of Apple's PGIOSurfaceHostDevice (vmapple's second MMIO device).
 *
 * This is reverse-engineered from steelbrain/reims-vgpu's `iosurface` module, which implements
 * the SAME vmapple apple-gfx-MMIO IOSurface device and works, cross-checked against QEMU's
 * hw/display/apple-gfx-mmio.m. See android/applegfx/BRIDGE.md for the full write-up and the
 * per-register / per-command citations (reims crates/reims-vgpu-core/src/iosurface/{regs,ring,
 * table,walk,descriptor,device}.rs).
 *
 * The device is a small MMIO register file driving a guest command ring. The guest's IOSurface
 * mapper writes a ring and a descriptor-table base, then rings a doorbell; the host drains the
 * ring, and each `map` command reads one guest IOSurface descriptor, walks its page table to the
 * guest pages that back the surface, and makes those pages a host-visible surface keyed by a
 * "mapping id". On Apple silicon the PVG kext sets usingIOSurfaceMapper=true and shares every
 * WindowServer IOSurface this way, so this device — not the GFX FIFO opcodes — is how surfaces
 * reach the renderer.
 *
 * What is implemented here: the whole MMIO/ring/descriptor protocol end to end — registers, the
 * doorbell, map/unmap/test, the one- and two-level page-table walk, the guest "mapped" bit
 * write-back, and the per-command completion IRQ on the IOSFC GIC line. mapMemory is the trivial
 * guest-physical -> host alias into the engine's single contiguous RAM mapping (ctx.ram->host).
 *
 * Handing the resulting surface to libapplegfx-vulkan: stock upstream implements IOSurface only over its
 * PCI-transport FIFO opcodes (0x26-0x2a, keyed by (surface_id, task_id) in its resource registry) and shipped
 * NO entry point for a mapping-id -> surface handoff from a separate host device. Henge adds that entry point
 * (lagfx_iosurface_register/unregister) as a local patch to the pinned rev; iosfc_map/iosfc_unmap below call it,
 * and it binds each surface as a TEXTURE keyed on mapping_id so the library's own CmdLookupIOSurface path finds
 * it. See BRIDGE.md and android/applegfx/patches/. */

#define IOSFC_PAGE_SIZE    0x4000ull  /* Apple-silicon guest: 16 KiB pages */
#define IOSFC_PAGE_SHIFT   14u
#define IOSFC_RING_CMD_SZ  16ull      /* one ring command, reims ring.rs RING_COMMAND_SIZE */
#define IOSFC_TABLE_ENTRY  4ull       /* one descriptor-table entry, reims table.rs TABLE_ENTRY_SIZE */
#define IOSFC_MAPPING_IDS  8192u      /* reims table.rs MAPPING_ID_COUNT; ids are 1..8192 */
#define IOSFC_TABLE_LEN    (IOSFC_PAGE_SIZE / IOSFC_TABLE_ENTRY) /* entries in one table page */
#define IOSFC_DESC_FIRST   0x200ull   /* first-form descriptor size */
#define IOSFC_DESC_SECOND  0x400ull   /* second-form descriptor size */

/* The MMIO register offsets (64-bit registers), reims iosurface/regs.rs. */
#define IOSFC_REG_RING_BASE        0x1000ull /* ring guest-physical base (a write unmaps the ring) */
#define IOSFC_REG_RING_SIZE        0x1008ull /* ring length in commands (needs a base; maps the ring) */
#define IOSFC_REG_DESCRIPTOR_BASE  0x1010ull /* descriptor-table guest-physical base (maps one page) */
#define IOSFC_REG_RING_WR_COUNTER  0x1018ull /* write counter: a write is the doorbell */
#define IOSFC_REG_RING_RD_COUNTER  0x1020ull /* read counter (writable only while no ring is mapped) */
#define IOSFC_REG_FEATURES         0x1028ull /* read-only: bit 0 = the second descriptor form is offered */
#define IOSFC_REG_DESC_ENABLE      0x1030ull /* descriptor form: bit 0 = second form */

/* Ring command words: command 1 map, 2 unmap, 3 test; anything else is unsupported. */
#define IOSFC_CMD_MAP    1u
#define IOSFC_CMD_UNMAP  2u
#define IOSFC_CMD_TEST   3u

/* IOReturn codes written back into each command's status word, reims ring.rs IoReturn::code. */
#define IOSFC_IO_SUCCESS       0
#define IOSFC_IO_NO_MEMORY     ((int32_t)0xe00002bd)
#define IOSFC_IO_BAD_ARGUMENT  ((int32_t)0xe00002c2)
#define IOSFC_IO_UNSUPPORTED   ((int32_t)0xe00002c7)
#define IOSFC_IO_BUSY          ((int32_t)0xe00002d5)

/* A descriptor's first word / a page-table entry: valid:1, (isTable|mapped):1, pageNumber:30
   (reims descriptor.rs IoSurfaceDescriptorPageTableEntry). */
#define IOSFC_PTE_VALID   1u
#define IOSFC_PTE_TABLE   2u   /* a page-table entry names a lower table */
#define IOSFC_PTE_MAPPED  2u   /* a descriptor's "mapped" bit (same bit, descriptor context) */

/* One mapped surface, keyed by its mapping id. For a guest-backed surface `pages` holds the host
   address of each of its `page_count` guest pages; a host-only surface lists none. The geometry is
   kept so a future renderer hook can build a texture over these pages. */
struct iosfc_surface {
    int present;
    int read_only;
    int host_only;
    uint64_t page_count;
    uint64_t *pages;              /* host addresses of each guest page, or NULL (host-only) */
    uint32_t pixel_format;
    uint64_t width, height, bytes_per_row, alloc_size, base_offset;
};

/* The IOSFC device state (modelling reims HostDeviceCore + Registers for the Henge engine). The
   guest RAM is one contiguous host mapping, so the ring, the descriptor table, the descriptor
   pages and the surface pages are aliased straight out of ctx.ram->host; there is no per-mapping
   machinery to maintain. */
struct applegfx_iosfc {
    pthread_mutex_t lock;
    /* The six writable registers (features is computed). */
    uint64_t ring_base, ring_size, descriptor_base, ring_wr, ring_rd, desc_enable;
    uint8_t *ring_host;          /* host pointer to the mapped ring, or NULL */
    uint8_t *table_host;         /* host pointer to the one mapped descriptor-table page, or NULL */
    uint64_t descriptors_per_page; /* PAGE / form-size, set by the enable register */
    struct iosfc_surface *slots; /* IOSFC_MAPPING_IDS entries, index 0 unused */
    int ready;                   /* slots allocated and lock initialised */
};

/* The engine state the library's callbacks reach back into. */
struct applegfx_ctx {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    HvRam *ram;
    int ncpus;

    lagfx_device_t *dev;
    lagfx_display_t *disp;

    /* Published frame (double-buffered BGRA). The pump fills `back`, then under `flock` swaps it to `front`;
       hv_pvg_applegfx_frame() hands back `front`. Width/height track the created/mode-changed display size. */
    pthread_mutex_t flock;
    uint8_t *front, *back;
    size_t bufsize;
    uint32_t width, height;
    atomic_uint_fast32_t cur_w, cur_h; /* last mode the display reported (frame_ready/mode_changed) */
    int have_frame;

    pthread_t pump;
    int pump_running;

    /* The IOSFC window: a host model of PGIOSurfaceHostDevice (see the big comment above). */
    struct applegfx_iosfc iosfc;
};

/* The task-memory callbacks wrap the library's own memfd+mmap primitives. */
static lagfx_task_t *applegfx_create_task(void *o, uint64_t vm_size, void **base_out) {
    (void)o;
    return lagfx_task_create(vm_size, base_out);
}
static void applegfx_destroy_task(void *o, lagfx_task_t *t) { (void)o; lagfx_task_destroy(t); }

/* Map guest-physical ranges into the task by aliasing the engine's guest-RAM mapping. */
static bool applegfx_map_memory(void *o, lagfx_task_t *t, uint64_t voff, const lagfx_physical_range_t *ranges,
                                size_t n, bool ro) {
    struct applegfx_ctx *C = o;
    uint64_t at = voff;
    for (size_t i = 0; i < n; i++) {
        uint64_t gpa = ranges[i].guest_physical_address, len = ranges[i].length;
        if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
        if (!lagfx_task_map_host_memory(t, at, C->ram->host + (gpa - C->ram->pa), len, ro)) return false;
        at += len;
    }
    return true;
}
static bool applegfx_unmap_memory(void *o, lagfx_task_t *t, uint64_t voff, uint64_t len) {
    (void)o;
    return lagfx_task_unmap(t, voff, len);
}
static bool applegfx_read_memory(void *o, uint64_t gpa, uint64_t len, void *dst) {
    struct applegfx_ctx *C = o;
    if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
    memcpy(dst, C->ram->host + (gpa - C->ram->pa), len);
    return true;
}
static bool applegfx_write_memory(void *o, uint64_t gpa, uint64_t len, const void *src) {
    struct applegfx_ctx *C = o;
    if (gpa < C->ram->pa || gpa - C->ram->pa + len > C->ram->size) return false;
    memcpy(C->ram->host + (gpa - C->ram->pa), src, len);
    return true;
}

/* The library (the PGDevice) raises MSI vector 0 for stamp/display completions; on the MMIO transport that is
   the single GFX GIC line (apple-gfx-mmio.m: desc.raiseInterrupt -> qemu_irq_pulse(irq_gfx)). hv_gicv3_pend is an
   edge, matching qemu_irq_pulse. The IOSFC line belongs to the IOSurface host device below, which raises it
   itself (iosfc_raise_interrupt). */
static void applegfx_raise_interrupt(void *o, uint32_t vector) {
    struct applegfx_ctx *C = o;
    (void)vector; /* every lagfx-raised interrupt is a PGDevice interrupt -> the GFX line */
    hv_gicv3_pend(C->gic, C->gfx_irq);
    if (C->ncpus == 1) hv_machine_wake(C->m);
}

/* Display callbacks: record the live mode so the pump knows the frame geometry. */
static void applegfx_frame_ready(void *o) { (void)o; }
static void applegfx_mode_changed(void *o, uint32_t w, uint32_t h) {
    struct applegfx_ctx *C = o;
    atomic_store(&C->cur_w, w);
    atomic_store(&C->cur_h, h);
}

/* ---- GFX window: forward to the library's BAR dispatch (identity offset, see header) -------------------- */
static uint64_t applegfx_gfx_read(void *o, uint64_t off, unsigned size) {
    struct applegfx_ctx *C = o;
    uint64_t v = 0;
    /* The library's registers are 4-byte; assemble wider (8-byte) accesses from native 4-byte reads. */
    for (unsigned done = 0; done < (size ? size : 4); done += 4)
        v |= (uint64_t)lagfx_mmio_read(C->dev, off + done) << (done * 8);
    return v;
}
static void applegfx_gfx_write(void *o, uint64_t off, uint64_t val, unsigned size) {
    struct applegfx_ctx *C = o;
    for (unsigned done = 0; done < (size ? size : 4); done += 4)
        lagfx_mmio_write(C->dev, off + done, (uint32_t)(val >> (done * 8)));
}

/* ---- IOSFC window: a host model of PGIOSurfaceHostDevice (see the big comment above / BRIDGE.md) -------- *
 *
 * The protocol, reconstructed from reims-vgpu's iosurface module (the ground truth) and QEMU's
 * apple-gfx-mmio.m: a 64-bit register file (0x1000..0x1030) driving a guest command ring; each ring
 * command maps/unmaps/tests one IOSurface named by a 1..8192 "mapping id"; a map reads the guest's
 * descriptor for that id, walks its page table to the guest pages that back the surface, aliases
 * them host-side and records the surface; each completed command raises the IOSFC IRQ. */

/* Little-endian guest word accessors (the guest and the host are both arm64 LE; assembled by byte
   so the code is endian-correct regardless). */
static uint32_t iosfc_ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t iosfc_ld64(const uint8_t *p) {
    return (uint64_t)iosfc_ld32(p) | ((uint64_t)iosfc_ld32(p + 4) << 32);
}
static void iosfc_st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void iosfc_st64(uint8_t *p, uint64_t v) { iosfc_st32(p, (uint32_t)v); iosfc_st32(p + 4, (uint32_t)(v >> 32)); }

/* Host pointer for `len` guest bytes at guest-physical `gpa`, or NULL if the range leaves RAM.
   This is the whole of "mapMemory" for this device: the engine's RAM is one contiguous host
   mapping, so a guest-physical address is ram->host + (gpa - ram->pa). */
static uint8_t *iosfc_gpa_to_host(struct applegfx_ctx *C, uint64_t gpa, uint64_t len) {
    if (len == 0 || gpa < C->ram->pa) return NULL;
    uint64_t off = gpa - C->ram->pa;
    if (off + len < off || off + len > C->ram->size) return NULL; /* overflow / out of range */
    return C->ram->host + off;
}

/* A page-table entry's target guest-physical address: pageNumber (bits 2..31) << page shift. */
static uint64_t iosfc_pte_address(uint32_t entry) {
    return (uint64_t)(entry >> 2) << IOSFC_PAGE_SHIFT;
}

/* A decoded surface descriptor, both forms upgraded to the fields this device needs
   (reims descriptor.rs: the second form, and upgrade_descriptor for the first). */
struct iosfc_desc {
    uint32_t word0;         /* first word: a page-table entry rooting the surface's page table */
    uint32_t pixel_format;
    uint64_t offset;
    uint64_t alloc_size;
    uint64_t width, height, bytes_per_row;
    uint8_t  plane_count;
    int read_only;
    int host_only;
};

/* Decode the descriptor at `d` in the current form. */
static void iosfc_decode_descriptor(const uint8_t *d, int second_form, struct iosfc_desc *out) {
    memset(out, 0, sizeof *out);
    out->word0 = iosfc_ld32(d + 0);
    out->pixel_format = iosfc_ld32(d + 4);
    if (second_form) {
        out->offset = iosfc_ld64(d + 0x08);
        out->alloc_size = iosfc_ld64(d + 0x18);
        out->width = iosfc_ld64(d + 0x20);
        out->height = iosfc_ld64(d + 0x28);
        out->bytes_per_row = iosfc_ld64(d + 0x30);
        out->plane_count = d[0x3e];
        out->read_only = (d[0x41] & 1u) != 0;  /* bits41: readOnly:1, hostOnly:1 */
        out->host_only = (d[0x41] & 2u) != 0;
    } else {
        /* First form (0x200): narrower fields, widened as reims upgrade_descriptor does. width and
           height live in the high 24 bits of word14/word18 (the low byte is the element size). */
        out->offset = iosfc_ld32(d + 0x08);
        out->alloc_size = iosfc_ld32(d + 0x10);
        out->width = iosfc_ld32(d + 0x14) >> 8;
        out->height = iosfc_ld32(d + 0x18) >> 8;
        out->bytes_per_row = iosfc_ld32(d + 0x1c);
        out->plane_count = d[0x24];
        out->read_only = (d[0x27] & 1u) != 0;  /* bits27: readOnly:1 */
        out->host_only = 0;                     /* an upgraded first-form descriptor is never host-only */
    }
}

/* Fill `s->pages` with the host address of each of `page_count` guest pages of the surface
   `desc` roots, through its one- or two-level page table (reims walk.rs). Returns an IOReturn. */
static int iosfc_walk(struct applegfx_ctx *C, uint32_t mapping_id, const struct iosfc_desc *desc,
                      uint64_t page_count, struct iosfc_surface *s) {
    uint64_t root_gpa = iosfc_pte_address(desc->word0);
    uint8_t *root = iosfc_gpa_to_host(C, root_gpa, IOSFC_PAGE_SIZE);
    if (!root) {
        fprintf(stderr, "hengevm: applegfx: iosfc map: surface %u page-table root at 0x%llx not in guest RAM\n",
                mapping_id, (unsigned long long)root_gpa);
        return IOSFC_IO_NO_MEMORY;
    }
    uint64_t entries_per_page = IOSFC_PAGE_SIZE / IOSFC_TABLE_ENTRY;
    if (page_count > entries_per_page * entries_per_page)
        return IOSFC_IO_NO_MEMORY; /* more than two levels of page table hold */

    s->pages = page_count ? calloc(page_count, sizeof(uint64_t)) : NULL;
    if (page_count && !s->pages) return IOSFC_IO_NO_MEMORY;
    s->page_count = page_count;

    uint64_t table_count = (page_count + entries_per_page - 1) / entries_per_page;
    if (table_count <= 1) {
        /* One level: each root entry names a page. */
        for (uint64_t i = 0; i < page_count; i++) {
            uint32_t e = iosfc_ld32(root + i * IOSFC_TABLE_ENTRY);
            if (!(e & IOSFC_PTE_VALID) || (e & IOSFC_PTE_TABLE)) return IOSFC_IO_BAD_ARGUMENT;
            uint8_t *hp = iosfc_gpa_to_host(C, iosfc_pte_address(e), IOSFC_PAGE_SIZE);
            if (!hp) return IOSFC_IO_NO_MEMORY;
            s->pages[i] = (uint64_t)(uintptr_t)hp;
        }
        return IOSFC_IO_SUCCESS;
    }
    /* Two levels: each root entry names a table of up to one page of page entries. */
    uint64_t mapped = 0, remaining = page_count;
    for (uint64_t t = 0; t < table_count; t++) {
        uint32_t te = iosfc_ld32(root + t * IOSFC_TABLE_ENTRY);
        if (!(te & IOSFC_PTE_VALID) || !(te & IOSFC_PTE_TABLE)) return IOSFC_IO_BAD_ARGUMENT;
        uint8_t *tbl = iosfc_gpa_to_host(C, iosfc_pte_address(te), IOSFC_PAGE_SIZE);
        if (!tbl) return IOSFC_IO_NO_MEMORY;
        uint64_t count = remaining < entries_per_page ? remaining : entries_per_page;
        for (uint64_t j = 0; j < count; j++) {
            uint32_t e = iosfc_ld32(tbl + j * IOSFC_TABLE_ENTRY);
            if (!(e & IOSFC_PTE_VALID) || (e & IOSFC_PTE_TABLE)) return IOSFC_IO_NO_MEMORY;
            uint8_t *hp = iosfc_gpa_to_host(C, iosfc_pte_address(e), IOSFC_PAGE_SIZE);
            if (!hp) return IOSFC_IO_NO_MEMORY;
            s->pages[mapped++] = (uint64_t)(uintptr_t)hp;
        }
        remaining -= count;
    }
    return IOSFC_IO_SUCCESS;
}

/* Release one surface slot. */
static void iosfc_free_slot(struct iosfc_surface *s) {
    free(s->pages);
    memset(s, 0, sizeof *s);
}

/* The host pointer of `mapping_id`'s descriptor, within the mapped descriptor table/page; writes
   *form_second. NULL (with an IOReturn in *status) when the table entry or page is unreachable. */
static uint8_t *iosfc_descriptor_ptr(struct applegfx_ctx *C, uint32_t mapping_id, int *second_form,
                                     int *status) {
    struct applegfx_iosfc *D = &C->iosfc;
    *second_form = (D->desc_enable & 1u) != 0;
    uint64_t form_size = *second_form ? IOSFC_DESC_SECOND : IOSFC_DESC_FIRST;
    uint64_t id = mapping_id;
    uint64_t page_index = id / D->descriptors_per_page;
    uint64_t within = id % D->descriptors_per_page;
    if (page_index >= IOSFC_TABLE_LEN) { *status = IOSFC_IO_BAD_ARGUMENT; return NULL; }
    uint32_t te = iosfc_ld32(D->table_host + page_index * IOSFC_TABLE_ENTRY);
    if (!(te & IOSFC_PTE_VALID)) { *status = IOSFC_IO_NO_MEMORY; return NULL; }
    uint8_t *page = iosfc_gpa_to_host(C, iosfc_pte_address(te), IOSFC_PAGE_SIZE);
    if (!page) { *status = IOSFC_IO_NO_MEMORY; return NULL; }
    return page + within * form_size;
}

/* `map` command: resolve the surface named by `mapping_id`, make its pages host-visible and record
   it; set the guest descriptor's "mapped" bit on success (reims table.rs map + walk). */
static int iosfc_map(struct applegfx_ctx *C, uint32_t mapping_id) {
    struct applegfx_iosfc *D = &C->iosfc;
    if (mapping_id == 0 || mapping_id >= IOSFC_MAPPING_IDS) return IOSFC_IO_BAD_ARGUMENT;
    struct iosfc_surface *slot = &D->slots[mapping_id];
    if (slot->present) return IOSFC_IO_SUCCESS; /* already mapped */

    int second_form = 0, status = IOSFC_IO_SUCCESS;
    uint8_t *dptr = iosfc_descriptor_ptr(C, mapping_id, &second_form, &status);
    if (!dptr) return status;

    struct iosfc_desc desc;
    iosfc_decode_descriptor(dptr, second_form, &desc);
    if (!(desc.word0 & IOSFC_PTE_VALID)) return IOSFC_IO_BAD_ARGUMENT;

    struct iosfc_surface made;
    memset(&made, 0, sizeof made);
    made.read_only = desc.read_only;
    made.host_only = desc.host_only;
    made.pixel_format = desc.pixel_format;
    made.width = desc.width;
    made.height = desc.height;
    made.bytes_per_row = desc.bytes_per_row;
    made.alloc_size = desc.alloc_size;
    made.base_offset = desc.offset;
    /* Whole pages spanning the allocation. */
    uint64_t page_count = (desc.alloc_size + IOSFC_PAGE_SIZE - 1) >> IOSFC_PAGE_SHIFT;

    if (desc.host_only) {
        made.page_count = page_count;      /* no guest pages; a real host would allocate backing here */
    } else {
        status = iosfc_walk(C, mapping_id, &desc, page_count, &made);
        if (status != IOSFC_IO_SUCCESS) { iosfc_free_slot(&made); return status; }
    }
    made.present = 1;
    *slot = made;

    /* Set the guest descriptor's mapped bit so the guest's mapper sees the surface as live. */
    uint32_t w = iosfc_ld32(dptr);
    iosfc_st32(dptr, w | IOSFC_PTE_MAPPED);

    /* Hand the mapped surface to the renderer. lagfx_iosurface_register gathers the guest pages
       into a sampleable VkImage keyed on mapping_id (the entry point this bridge added upstream;
       see BRIDGE.md and the android/applegfx patch). Host-only surfaces have no guest backing to
       upload, so they are left for the library's own allocation path and not registered here. A
       register failure is non-fatal: the IOSFC map still succeeds (the guest's mapper is satisfied
       by the mapped bit); only the on-screen content is missing, which we log. */
    int registered = 0;
    if (!made.host_only && made.pages && made.page_count && C->dev) {
        const void **pv = calloc((size_t)made.page_count, sizeof(*pv));
        if (pv) {
            for (uint64_t i = 0; i < made.page_count; i++)
                pv[i] = (const void *)(uintptr_t)made.pages[i];
            lagfx_iosurface_mapping_t lm = {
                .mapping_id    = mapping_id,
                .pixel_format  = made.pixel_format,
                .width         = (uint32_t)made.width,
                .height        = (uint32_t)made.height,
                .bytes_per_row = (uint32_t)made.bytes_per_row,
                .alloc_size    = made.alloc_size,
                .page_host_addrs = pv,
                .page_count    = (uint32_t)made.page_count,
                .page_size     = (uint32_t)IOSFC_PAGE_SIZE,
            };
            lagfx_status_t rst = lagfx_iosurface_register(C->dev, &lm);
            registered = (rst == LAGFX_OK);
            if (!registered)
                fprintf(stderr, "hengevm: applegfx: iosfc id=%u register failed (%d); surface mapped "
                        "but not displayable\n", mapping_id, (int)rst);
            free(pv);
        }
    }

    fprintf(stderr, "hengevm: applegfx: iosfc map id=%u %llux%llu fmt=0x%x pages=%llu%s%s\n", mapping_id,
            (unsigned long long)made.width, (unsigned long long)made.height, made.pixel_format,
            (unsigned long long)made.page_count, made.host_only ? " host-only" : "",
            registered ? " -> renderer" : (made.host_only ? "" : " (not registered)"));
    return IOSFC_IO_SUCCESS;
}

/* `unmap` command: release the surface and clear the guest descriptor's mapped bit
   (reims table.rs unmap). This model tracks no external holders, so a mapped surface is never
   Busy. */
static int iosfc_unmap(struct applegfx_ctx *C, uint32_t mapping_id) {
    struct applegfx_iosfc *D = &C->iosfc;
    if (mapping_id == 0 || mapping_id >= IOSFC_MAPPING_IDS) return IOSFC_IO_BAD_ARGUMENT;
    struct iosfc_surface *slot = &D->slots[mapping_id];
    if (!slot->present) return IOSFC_IO_SUCCESS;

    /* Drop the renderer's backing for this id before clearing our slot. Mirrors the register in
       iosfc_map; a no-op (NOT_FOUND) for surfaces that were never registered (host-only, or a
       register that failed). */
    if (!slot->host_only && C->dev)
        lagfx_iosurface_unregister(C->dev, mapping_id);

    int second_form = 0, status = IOSFC_IO_SUCCESS;
    uint8_t *dptr = iosfc_descriptor_ptr(C, mapping_id, &second_form, &status);
    if (dptr) {
        uint32_t w = iosfc_ld32(dptr);
        iosfc_st32(dptr, w & ~IOSFC_PTE_MAPPED);
    }
    iosfc_free_slot(slot);
    return IOSFC_IO_SUCCESS;
}

/* `test` command: write each 8-byte word of the surface's allocation with its own byte offset, as
   a device self-test the guest reads back (reims table.rs test_pattern). Guest-backed surfaces are
   written page by page (a 16 KiB page is a multiple of 8, so no word straddles a page); host-only
   surfaces have no backing in this model and are a no-op success. */
static int iosfc_test(struct applegfx_ctx *C, uint32_t mapping_id) {
    struct applegfx_iosfc *D = &C->iosfc;
    if (mapping_id == 0 || mapping_id >= IOSFC_MAPPING_IDS) return IOSFC_IO_BAD_ARGUMENT;
    struct iosfc_surface *s = &D->slots[mapping_id];
    if (!s->present) return IOSFC_IO_BAD_ARGUMENT;
    if (s->host_only || !s->pages) return IOSFC_IO_SUCCESS;

    uint64_t total = s->page_count << IOSFC_PAGE_SHIFT;
    for (uint64_t off = 0; off + 8 <= total && off < s->alloc_size + 8; off += 8) {
        uint64_t pg = off >> IOSFC_PAGE_SHIFT, within = off & (IOSFC_PAGE_SIZE - 1);
        if (pg >= s->page_count) break;
        iosfc_st64((uint8_t *)(uintptr_t)s->pages[pg] + within, off);
    }
    return IOSFC_IO_SUCCESS;
}

/* Raise the IOSFC completion interrupt on its own GIC line (apple-gfx-mmio.m routes the
   PGIOSurfaceHostDevice's raiseInterrupt to irq_iosfc, distinct from the GFX device's irq_gfx). */
static void iosfc_raise_interrupt(struct applegfx_ctx *C) {
    hv_gicv3_pend(C->gic, C->iosfc_irq);
    if (C->ncpus == 1) hv_machine_wake(C->m);
}

/* Drain the ring from the read counter to the write counter: run each command, write its status
   back into the ring slot, bump the read counter and raise the IRQ (reims ring.rs run_ring). */
static void iosfc_run_ring(struct applegfx_ctx *C) {
    struct applegfx_iosfc *D = &C->iosfc;
    if (D->ring_base == 0 || D->ring_size == 0 || !D->ring_host || !D->table_host) return;
    while (D->ring_rd < D->ring_wr) {
        uint64_t slot_index = D->ring_rd % D->ring_size;
        uint8_t *cmd = D->ring_host + slot_index * IOSFC_RING_CMD_SZ;
        uint32_t command = iosfc_ld32(cmd + 0);
        uint32_t mapping_id = iosfc_ld32(cmd + 4);
        int status;
        switch (command) {
            case IOSFC_CMD_MAP:   status = iosfc_map(C, mapping_id);   break;
            case IOSFC_CMD_UNMAP: status = iosfc_unmap(C, mapping_id); break;
            case IOSFC_CMD_TEST:  status = iosfc_test(C, mapping_id);  break;
            default:
                fprintf(stderr, "hengevm: applegfx: iosfc unsupported command %u for surface %u\n",
                        command, mapping_id);
                status = IOSFC_IO_UNSUPPORTED;
                break;
        }
        iosfc_st32(cmd + 8, (uint32_t)status); /* status word at command + 8 */
        D->ring_rd++;
        iosfc_raise_interrupt(C);
    }
}

/* Whether the second descriptor form is offered: on 16 KiB pages a one-page table reaches every
   mapping id, so the form is offered (reims regs.rs second_form_holds_every_id). */
static uint64_t iosfc_features_offered(void) {
    uint64_t entries = IOSFC_PAGE_SIZE / IOSFC_TABLE_ENTRY;
    uint64_t per_page = IOSFC_PAGE_SIZE / IOSFC_DESC_SECOND;
    return (entries * per_page >= IOSFC_MAPPING_IDS) ? 1 : 0;
}

/* Tear every surface down (a descriptor-base or enable write does this; reims teardown_surfaces). */
static void iosfc_teardown_surfaces(struct applegfx_iosfc *D) {
    for (uint32_t i = 1; i < IOSFC_MAPPING_IDS; i++)
        if (D->slots[i].present) iosfc_free_slot(&D->slots[i]);
}

/* Read a 64-bit register (features is computed; an unknown offset is all ones). */
static uint64_t iosfc_reg_get(struct applegfx_ctx *C, uint64_t rbase) {
    struct applegfx_iosfc *D = &C->iosfc;
    switch (rbase) {
        case IOSFC_REG_RING_BASE:       return D->ring_base;
        case IOSFC_REG_RING_SIZE:       return D->ring_size;
        case IOSFC_REG_DESCRIPTOR_BASE: return D->descriptor_base;
        case IOSFC_REG_RING_WR_COUNTER: return D->ring_wr;
        case IOSFC_REG_RING_RD_COUNTER: return D->ring_rd;
        case IOSFC_REG_FEATURES:        return iosfc_features_offered();
        case IOSFC_REG_DESC_ENABLE:     return D->desc_enable;
        default:                        return (uint64_t)-1;
    }
}

/* Apply a full 64-bit register write with its side effects (reims regs.rs mmio_write + setters). */
static void iosfc_reg_put(struct applegfx_ctx *C, uint64_t rbase, uint64_t value) {
    struct applegfx_iosfc *D = &C->iosfc;
    switch (rbase) {
        case IOSFC_REG_RING_BASE:
            /* A new base unmaps the ring and keeps the size: the ring stays unmapped until the
               size is written again. */
            D->ring_host = NULL;
            D->ring_base = value;
            break;
        case IOSFC_REG_RING_SIZE: {
            /* A size needs a base and is stored before the ring is mapped; a refused mapping leaves
               the register at the new size and the ring as it was. */
            if (value == 0 || D->ring_base == 0) return;
            D->ring_size = value;
            uint64_t length = value * IOSFC_RING_CMD_SZ;
            if (length / IOSFC_RING_CMD_SZ != value) return; /* overflow: register set, ring unchanged */
            uint8_t *mapped = iosfc_gpa_to_host(C, D->ring_base, length);
            if (mapped) D->ring_host = mapped; /* else keep the old ring */
            break;
        }
        case IOSFC_REG_DESCRIPTOR_BASE:
            /* A new base tears every surface down, drops the table and maps one page of table. */
            iosfc_teardown_surfaces(D);
            D->table_host = NULL;
            D->descriptor_base = value;
            if (value != 0) D->table_host = iosfc_gpa_to_host(C, value, IOSFC_PAGE_SIZE);
            break;
        case IOSFC_REG_RING_WR_COUNTER:
            D->ring_wr = value;
            iosfc_run_ring(C); /* the doorbell */
            break;
        case IOSFC_REG_RING_RD_COUNTER:
            if (!D->ring_host) D->ring_rd = value; /* writable only while no ring is mapped */
            break;
        case IOSFC_REG_FEATURES:
            break; /* read-only */
        case IOSFC_REG_DESC_ENABLE:
            /* Keep bit 0, set the form's descriptors-per-page and drop the table (teardown). */
            D->desc_enable = value & 1u;
            D->descriptors_per_page = IOSFC_PAGE_SIZE / (D->desc_enable ? IOSFC_DESC_SECOND : IOSFC_DESC_FIRST);
            iosfc_teardown_surfaces(D);
            D->table_host = NULL;
            D->descriptor_base = 0;
            break;
        default:
            break;
    }
}

/* The IOSFC registers are 64-bit. The guest's mapper drives them with 64-bit accesses; a narrower
   access is served against the containing 8-byte register (reims regs.rs notes QEMU's 32-bit shell
   truncates reads / zero-extends writes). */
static uint64_t applegfx_iosfc_read(void *o, uint64_t off, unsigned size) {
    struct applegfx_ctx *C = o;
    struct applegfx_iosfc *D = &C->iosfc;
    if (!D->ready) return (uint64_t)-1;
    uint64_t rbase = off & ~7ull, sub = off & 7ull;
    pthread_mutex_lock(&D->lock);
    uint64_t v = iosfc_reg_get(C, rbase);
    pthread_mutex_unlock(&D->lock);
    if (size == 0 || size >= 8) return v;
    uint64_t mask = (size >= 8) ? ~0ull : ((1ull << (size * 8)) - 1);
    return (v >> (sub * 8)) & mask;
}
static void applegfx_iosfc_write(void *o, uint64_t off, uint64_t val, unsigned size) {
    struct applegfx_ctx *C = o;
    struct applegfx_iosfc *D = &C->iosfc;
    if (!D->ready) return;
    uint64_t rbase = off & ~7ull, sub = off & 7ull;
    pthread_mutex_lock(&D->lock);
    uint64_t full = val;
    if (size && size < 8) {
        /* Splice the narrow write into the current register value, then apply the whole word. */
        uint64_t cur = (rbase == IOSFC_REG_FEATURES) ? 0 : iosfc_reg_get(C, rbase);
        uint64_t mask = ((1ull << (size * 8)) - 1) << (sub * 8);
        full = (cur & ~mask) | ((val << (sub * 8)) & mask);
    }
    iosfc_reg_put(C, rbase, full);
    pthread_mutex_unlock(&D->lock);
}

/* Re-upload each live guest-backed IOSurface from its still-mapped pages so a surface the guest keeps
   redrawing (the compositor / wallpaper framebuffer) tracks those writes on screen. lagfx_iosurface_register
   snapshots the pages once at map time; this is the per-vblank refresh. The guest-RAM mapping is fixed, so the
   page host pointers captured at map time stay valid — only their contents change — and refresh re-uploads into
   the existing VkImage (no recreate). Held under D->lock so a concurrent unmap can't free s->pages mid-upload.
   Set HV_GPU_APPLEGFX_NO_REFRESH=1 to disable (debug / measure). */
static void iosfc_refresh_surfaces(struct applegfx_ctx *C) {
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("HV_GPU_APPLEGFX_NO_REFRESH") ? 1 : 0;
    if (disabled || !C->dev) return;
    struct applegfx_iosfc *D = &C->iosfc;
    if (!D->ready) return;
    pthread_mutex_lock(&D->lock);
    for (uint32_t id = 1; id < IOSFC_MAPPING_IDS; id++) {
        struct iosfc_surface *s = &D->slots[id];
        if (!s->present || s->host_only || !s->pages || !s->page_count) continue;
        const void **pv = calloc((size_t)s->page_count, sizeof(*pv));
        if (!pv) continue;
        for (uint64_t i = 0; i < s->page_count; i++)
            pv[i] = (const void *)(uintptr_t)s->pages[i];
        lagfx_iosurface_mapping_t lm = {
            .mapping_id    = id,
            .pixel_format  = s->pixel_format,
            .width         = (uint32_t)s->width,
            .height        = (uint32_t)s->height,
            .bytes_per_row = (uint32_t)s->bytes_per_row,
            .alloc_size    = s->alloc_size,
            .page_host_addrs = pv,
            .page_count    = (uint32_t)s->page_count,
            .page_size     = (uint32_t)IOSFC_PAGE_SIZE,
        };
        lagfx_iosurface_refresh(C->dev, &lm);
        free(pv);
    }
    pthread_mutex_unlock(&D->lock);
}

/* ---- ~60 Hz pump: vblank/online IRQ + frame read-out -------------------------------------------------- */
static void *applegfx_pump(void *arg) {
    struct applegfx_ctx *C = arg;
    const struct timespec interval = {0, 16666667}; /* ~60 Hz */
    for (;;) {
        /* Display online / vblank: fires the online IRQ (vector 0 -> GFX line) the moment the guest enables a
           display, which the deadlock fix in the library depends on. */
        lagfx_timer_tick_vblank(C->dev, C, applegfx_write_memory, applegfx_read_memory);

        /* Refresh live IOSurfaces from guest memory before reading the frame, so the compositor's latest
           drawing is in the sampled backing. */
        iosfc_refresh_surfaces(C);

        /* Pull the latest rendered frame, if any, into the back buffer and publish it. */
        size_t stride = 0;
        bool newf = false;
        lagfx_status_t st = lagfx_display_read_frame(C->disp, C->back, C->bufsize, &stride, &newf);
        if (st == LAGFX_OK && newf) {
            pthread_mutex_lock(&C->flock);
            uint8_t *tmp = C->front;
            C->front = C->back;
            C->back = tmp;
            C->have_frame = 1;
            pthread_mutex_unlock(&C->flock);
        }

        nanosleep(&interval, NULL);
    }
    return NULL;
}

/* The newest applegfx frame (BGRA, width*4 bytes per row), or -1 if none yet. Mirrors hv_pvg_lite_frame(). */
int hv_pvg_applegfx_frame(const uint8_t **pixels, uint32_t *width, uint32_t *height);
static struct applegfx_ctx *g_ctx; /* published once the bridge is live */

int hv_pvg_applegfx_frame(const uint8_t **pixels, uint32_t *width, uint32_t *height) {
    struct applegfx_ctx *C = g_ctx;
    if (!C) return -1;
    int rc = -1;
    pthread_mutex_lock(&C->flock);
    if (C->have_frame) {
        *pixels = C->front;
        *width = C->width;
        *height = C->height;
        rc = 0;
    }
    pthread_mutex_unlock(&C->flock);
    return rc;
}
#endif /* HAVE_APPLEGFX */

int hv_pvg_applegfx_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    const char *path = getenv("HV_GPU_APPLEGFX");
    if (!path || !*path)
        return 0; /* a different backend was chosen; leave the device to it */

#if defined(HAVE_APPLEGFX)
    static struct applegfx_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.m = m, ctx.gic = gic, ctx.gfx_irq = gfx_irq, ctx.iosfc_irq = iosfc_irq;
    ctx.ncpus = m->ncpus;
    ctx.ram = hv_ram_lookup(m, VMAPPLE_MEM);
    if (!ctx.ram || !ctx.ram->host) {
        fprintf(stderr, "hengevm: applegfx: guest RAM is not host-mapped; using the built-in GPU instead\n");
        return 0;
    }

    fprintf(stderr, "hengevm: applegfx: libapplegfx-vulkan %u.%u.%u (%s)\n", lagfx_version_major(),
            lagfx_version_minor(), lagfx_version_patch(), lagfx_build_info());

    lagfx_device_descriptor_t desc;
    memset(&desc, 0, sizeof desc);
    desc.shell.opaque = &ctx;
    desc.shell.create_task = applegfx_create_task;
    desc.shell.destroy_task = applegfx_destroy_task;
    desc.shell.map_memory = applegfx_map_memory;
    desc.shell.unmap_memory = applegfx_unmap_memory;
    desc.shell.read_memory = applegfx_read_memory;
    desc.shell.write_memory = applegfx_write_memory;
    desc.shell.raise_interrupt = applegfx_raise_interrupt;

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&desc, &err);
    if (!dev) {
        fprintf(stderr, "hengevm: applegfx: device init failed: %s; using the built-in GPU instead\n",
                err ? err : "(no detail)");
        free(err);
        return 0;
    }
    ctx.dev = dev;

    lagfx_display_mode_t mode = {.width_px = 1920, .height_px = 1080, .refresh_rate_hz = 60};
    lagfx_display_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    dd.name = "Henge display";
    dd.size_mm_width = 400, dd.size_mm_height = 250;
    dd.modes = &mode, dd.mode_count = 1;
    dd.callbacks.opaque = &ctx;
    dd.callbacks.frame_ready = applegfx_frame_ready;
    dd.callbacks.mode_changed = applegfx_mode_changed;
    lagfx_display_t *disp = lagfx_display_new(dev, &dd, 0, 1, &err);
    if (!disp) {
        fprintf(stderr, "hengevm: applegfx: display init failed: %s; using the built-in GPU instead\n",
                err ? err : "(no detail)");
        free(err);
        lagfx_device_free(dev);
        return 0;
    }
    ctx.disp = disp;
    free(err);

    /* The MMIO<->PCI bridge is opt-in while the IOSFC (IOSurface host device) window is unimplemented: without
       it the guest has no surface backing, so engaging the bridge by default would black the screen. By default
       applegfx proves the library loads/initialises on the tablet and then hands the screen to pvg_lite (a
       picture). Set HV_GPU_APPLEGFX_BRIDGE=1 to engage the experimental bridge and test it (see
       android/applegfx/BRIDGE.md). */
    if (!getenv("HV_GPU_APPLEGFX_BRIDGE")) {
        fprintf(stderr, "hengevm: applegfx: device and display created on this tablet (guest memory and interrupts "
                        "wired). The MMIO<->PCI bridge is implemented but off by default (the IOSFC window is not "
                        "done yet, so it would not render); set HV_GPU_APPLEGFX_BRIDGE=1 to try it. Using the "
                        "built-in GPU for the screen.\n");
        lagfx_display_free(disp), lagfx_device_free(dev);
        return 0;
    }

    /* Frame buffers for the advertised mode (BGRA, tightly packed). Mode changes beyond this are a TODO:
       the single advertised mode means the guest should select it. */
    ctx.width = mode.width_px, ctx.height = mode.height_px;
    atomic_store(&ctx.cur_w, ctx.width);
    atomic_store(&ctx.cur_h, ctx.height);
    ctx.bufsize = (size_t)ctx.width * ctx.height * 4u;
    ctx.front = malloc(ctx.bufsize);
    ctx.back = malloc(ctx.bufsize);
    if (!ctx.front || !ctx.back) {
        fprintf(stderr, "hengevm: applegfx: out of memory for the frame buffers; using the built-in GPU instead\n");
        free(ctx.front), free(ctx.back);
        lagfx_display_free(disp), lagfx_device_free(dev);
        return 0;
    }
    memset(ctx.front, 0, ctx.bufsize);
    memset(ctx.back, 0, ctx.bufsize);
    pthread_mutex_init(&ctx.flock, NULL);

    /* Bring up the IOSFC device model (PGIOSurfaceHostDevice). Its slots are keyed by mapping id;
       index 0 is unused (ids are 1..8192). First form until the enable register selects the second. */
    ctx.iosfc.slots = calloc(IOSFC_MAPPING_IDS, sizeof(struct iosfc_surface));
    if (!ctx.iosfc.slots) {
        fprintf(stderr, "hengevm: applegfx: out of memory for the IOSFC surface table; using the built-in GPU instead\n");
        free(ctx.front), free(ctx.back);
        lagfx_display_free(disp), lagfx_device_free(dev);
        return 0;
    }
    pthread_mutex_init(&ctx.iosfc.lock, NULL);
    ctx.iosfc.descriptors_per_page = IOSFC_PAGE_SIZE / IOSFC_DESC_FIRST;
    ctx.iosfc.ready = 1;

    /* Claim both MMIO windows. The GFX window forwards to the library; the IOSFC window drives the
       PGIOSurfaceHostDevice model above. Self-locked like reims: each access may block in the library
       or the ring drain, which must not stall the engine's device lock for the other vCPU. */
    hv_machine_add_mmio(m, gfx_pa, 0x10000, &ctx, applegfx_gfx_read, applegfx_gfx_write, "gfx-applegfx");
    m->mmio[m->nmmio - 1].self_locked = 1;
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, &ctx, applegfx_iosfc_read, applegfx_iosfc_write, "iosurface-applegfx");
    m->mmio[m->nmmio - 1].self_locked = 1;

    g_ctx = &ctx;
    if (pthread_create(&ctx.pump, NULL, applegfx_pump, &ctx) == 0) {
        pthread_detach(ctx.pump);
        ctx.pump_running = 1;
    } else {
        fprintf(stderr, "hengevm: applegfx: could not start the vblank/frame pump\n");
    }

    fprintf(stderr, "hengevm: applegfx: EXPERIMENTAL MMIO<->PCI bridge engaged on this tablet. The GFX window and "
                    "interrupts are wired to libapplegfx-vulkan, and the IOSFC (PGIOSurfaceHostDevice) window now "
                    "models the surface-mapping protocol (registers, ring, map/unmap/test, page walk, IRQ); mapped "
                    "surfaces are recorded but not yet handed to the renderer, which needs a new lagfx hook (see "
                    "android/applegfx/BRIDGE.md). Pick Graphics: reims for the known-working GPU, or set "
                    "HV_GPU_APPLEGFX_PROBE_ONLY=1 for the safe fallback.\n");
    return 1; /* the bridge owns the GPU windows now */
#else
    (void)m, (void)gic, (void)gfx_pa, (void)iosfc_pa, (void)gfx_irq, (void)iosfc_irq;
    fprintf(stderr, "hengevm: applegfx: this GPU host (libapplegfx-vulkan) is not built into this engine; "
                    "using the built-in GPU instead. Pick Graphics: reims for the app's reims-vgpu host.\n");
    return 0;
#endif
}
