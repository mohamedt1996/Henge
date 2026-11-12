# applegfx: the apple-gfx MMIO ↔ PCI transport bridge

This note records exactly how Henge's `vmapple` paravirtual-GPU transport is bridged onto
[`libapplegfx-vulkan`](https://github.com/MattJackson/libapplegfx-vulkan), what the mapping is, the evidence
for it, and — honestly — what is still missing before `Graphics: applegfx` can render. The code is
`android/pvg_applegfx.c`; the presentation hook is in `host/vm.c` (`hv_vm_boot_display`).

## The two transports

Apple's `ParavirtualizedGraphics.framework` host API has two QEMU device shells, confirmed by reading the
upstream sources directly:

| | PCI variant (`hw/display/apple-gfx-pci.m`) | MMIO variant (`hw/display/apple-gfx-mmio.m`) |
|---|---|---|
| Register window | one PCI BAR = `s->common.iomem_gfx` | sysbus MMIO #0 = **same** `s->common.iomem_gfx` (GFX) |
| Second device | none | sysbus MMIO #1 = `s->iomem_iosfc` (0x10000), a separate **PGIOSurfaceHostDevice** |
| Interrupts | `msi_notify(dev, vector)` — PGDevice is the only source | GIC line `irq_gfx` (PGDevice) **and** `irq_iosfc` (PGIOSurfaceHostDevice) |
| IOSurface sharing | via the FIFO command stream (PGDevice internal) | via the PGIOSurfaceHostDevice, with `desc.usingIOSurfaceMapper = true` |

`libapplegfx-vulkan` is a clean-room implementation of the **PCI** variant: one `lagfx_device_t` behind one BAR
(`lagfx_mmio_read/write`), interrupts via a single `raise_interrupt(opaque, vector)` shell callback using MSI
vector numbers, and IOSurface handled as FIFO opcodes `0x26`–`0x2a` on channel 0
(`src/handlers/iosurface/iosurface.c`).

Henge's `vmapple` exposes the **MMIO** variant: two windows `VMAPPLE_GFX` (0x30200000) and `VMAPPLE_IOSFC`
(0x30210000), each 0x10000, plus two GIC SPIs (`32+0x11` gfx, `32+0x10` iosfc). This is what Apple-silicon macOS
actually drives, and what the working `reims` backend matches.

## The mapping (and its evidence)

### GFX window → `lagfx_mmio_read/write`, identity offset

- `apple-gfx-pci.m:apple_gfx_pci_realize` registers exactly one BAR and points it at `s->common.iomem_gfx`.
- `apple-gfx-mmio.m:apple_gfx_mmio_init` does `sysbus_init_mmio(..., &s->common.iomem_gfx)` for window #0 — the
  **same** `MemoryRegion`, built once in `apple-gfx.m:apple_gfx_common_init` with `apple_gfx_ops`.
- `apple_gfx_ops.read/write` call `[pgdev mmioReadAtOffset:offset]` / `mmioWriteAtOffset:value:` with the
  window-relative offset, unchanged, in **both** variants.

Therefore the GFX-window offset is transport-independent and maps **1:1** onto the library's BAR offset:

```
lagfx_mmio_read (dev, gfx_offset)            // no translation
lagfx_mmio_write(dev, gfx_offset, value)
```

The library keeps its registers at BAR `0x1000+` and reserves `0x0..0xfff` for the PCI MSI-X table
(`src/protocol/protocol.h`, `src/device.c:lagfx_mmio_read` returns 0 below `LAGFX_MSIX_RANGE_END`). On MMIO that
low 4 KiB is simply never accessed, because interrupts arrive over the GIC rather than an MSI-X table — so the
identity map is exact, not merely close. Known registers (from `protocol.h` / `state.h`):

```
0x1000 STATUS_CONTROL     0x1008 WRITE_PTR (primary ring doorbell)   0x1014 display IRQ bitmask
0x1018 stamp IRQ bitmask  0x101c ROOT_PAGE_NUMBER   0x1020 stamp doorbell   0x1034 BINARY_VERSION
```

Access width: `apple_gfx_ops.impl.max_access_size = 4`, so the guest uses 4-byte register ops; the bridge still
splits any 8-byte access into two native 4-byte `lagfx_mmio_*` calls (`applegfx_gfx_read/write`).

### Interrupts → the GFX GIC line

- In `apple-gfx-mmio.m`, the **PGDevice** descriptor's `raiseInterrupt` block pulses `irq_gfx`; the separate
  **PGIOSurfaceHostDevice** descriptor's `raiseInterrupt` block pulses `irq_iosfc`.
- The library *is* the PGDevice, so every interrupt it raises through the shell `raise_interrupt` callback (only
  MSI vector 0, for stamps/displays — see the library's `CLAUDE.md`: "MSI vector 0 only for stamp / pending
  bitmask interrupts") belongs to the **GFX** line.

So `applegfx_raise_interrupt(vector)` edges `gfx_irq` via `hv_gicv3_pend` (an edge, matching
`qemu_irq_pulse`), regardless of `vector`, and wakes a 1-CPU guest. The `iosfc_irq` line is owned by the
IOSurface host device and is left to the (unimplemented) IOSFC path. *(This corrects the pre-bridge placeholder,
which routed `vector != 0` to `iosfc_irq`.)*

### vblank / frame-out

- `lagfx_timer_tick_vblank(dev, &ctx, write_memory, read_memory)` is called at ~60 Hz from a pump thread; the
  library fires the display-online IRQ (vector 0 → GFX line) the instant the guest enables a display
  (`ss[+0x104]==0xC`), which its ABBA-deadlock fix relies on.
- `lagfx_display_read_frame(disp, buf, size, &stride, &new)` returns BGRA8; the pump double-buffers it and
  publishes it through `hv_pvg_applegfx_frame()`, which `hv_vm_boot_display()` presents exactly like
  `hv_pvg_lite_frame()`.

## The IOSFC window — `PGIOSurfaceHostDevice` (now modelled)

The IOSFC window is a **second Apple device**, `PGIOSurfaceHostDevice`, that exists only in the MMIO/ARM
transport. `apple-gfx-mmio.m` sets `desc.usingIOSurfaceMapper = true`, which tells the guest/framework to route
IOSurface sharing through this device — **not** through the FIFO `0x26`–`0x2a` opcodes the library implements for
the PCI transport (where `usingIOSurfaceMapper` is false). On Apple silicon, WindowServer composites almost
entirely out of IOSurfaces, so without this device the renderer has no surface backing even when the GFX command
ring runs perfectly.

`libapplegfx-vulkan` does not implement it, and its MMIO register layout is not reverse-engineered anywhere in
the library. It **is** implemented and documented, byte for byte, in
[`steelbrain/reims-vgpu`](https://github.com/steelbrain/reims-vgpu) — the SAME vmapple apple-gfx-MMIO variant,
including the IOSurface device, and it works. The protocol below is reconstructed from that module (the ground
truth) and cross-checked against QEMU's `hw/display/apple-gfx-mmio.m`. `android/pvg_applegfx.c` now models the
device end to end from this; what remains open is only the hand-off of a mapped surface to the renderer (below).

Sources (reims commit pinned in `android/gpu/reims/build.sh`,
`crates/reims-vgpu-core/src/iosurface/`): `regs.rs` (registers), `ring.rs` (command ring), `table.rs`
(map/unmap/test), `walk.rs` (page-table walk), `descriptor.rs` (descriptor formats),
`device.rs` (device lifecycle + the surface lookup), and the C host shell `crates/reims-vgpu-c/src/iosurface.rs`.

### The register file (`regs.rs`)

Six writable 64-bit registers plus a read-only features register, at MMIO offsets `0x1000`+. (reims notes QEMU's
shell declares them 32-bit, so a narrow access truncates on read / zero-extends on write; the guest's mapper uses
64-bit accesses. The Henge model serves a narrow access against the containing 8-byte register.) An offset naming
no register reads **all ones** and ignores writes.

```
0x1000 RING_BASE        ring guest-physical base; a write unmaps the ring and keeps the size
0x1008 RING_SIZE        ring length in COMMANDS; needs a base; maps the ring (size * 16 bytes)
0x1010 DESCRIPTOR_BASE  descriptor-table guest-physical base; a write tears down every surface,
                        drops the table and maps one guest page of table there
0x1018 RING_WR_COUNTER  the ring write counter; a write is the DOORBELL (drains the ring)
0x1020 RING_RD_COUNTER  the ring read counter; writable only while no ring is mapped
0x1028 FEATURES         read-only: bit 0 = the second descriptor form is offered
0x1030 DESC_ENABLE      descriptor form select: bit 0 = second form (else first)
```

`FEATURES` bit 0 is offered when one page of descriptor table reaches every mapping id: on 16 KiB pages
(Apple silicon) a table page holds 4096 entries × 16 second-form descriptors/page = 65536 ≥ 8192, so it is
offered; on 4 KiB pages it is not (the guest then uses the first form). `DESC_ENABLE` also sets the
descriptors-per-page (`PAGE / 0x200` first form, `PAGE / 0x400` second) and, like a `DESCRIPTOR_BASE` write,
tears the surfaces down and drops the table.

### The command ring (`ring.rs`)

The ring is an array of 16-byte commands at `RING_BASE`, `RING_SIZE` of them. One command:

```
+0x00 u32 command      1 = map, 2 = unmap, 3 = test; anything else = Unsupported
+0x04 u32 mapping_id   the surface id, 1..8192
+0x08 i32 status       the host writes the IOReturn back here
+0x0c u32 (unused)
```

The doorbell (a write to `RING_WR_COUNTER`) drains the ring: for each command from `RING_RD_COUNTER` up to
`RING_WR_COUNTER`, read the slot at `RING_BASE + (rd % size) * 16`, run it, write the IOReturn into the slot's
status word, bump `RING_RD_COUNTER`, and **raise the IOSFC interrupt** (one per command). A doorbell with no ring
or no descriptor table mapped runs nothing. IOReturn codes written back:

```
Success 0   NoMemory 0xe00002bd   BadArgument 0xe00002c2   Unsupported 0xe00002c7   Busy 0xe00002d5
```

### `map` — descriptor read + page walk (`table.rs`, `descriptor.rs`, `walk.rs`)

`map(id)` makes the IOSurface that mapping id's descriptor describes, keyed by id:

1. **Find the descriptor.** The descriptor table is one page of 4-byte entries at `DESCRIPTOR_BASE`.
   `page_index = id / descriptors_per_page` selects a table entry → a **descriptor page** (guest-physical);
   `within = id % descriptors_per_page` selects the descriptor in it. The descriptor is `0x200` bytes (first
   form) or `0x400` bytes (second form).
2. **Decode it.** The descriptor's first word is a page-table entry `{ valid:1, (isTable|mapped):1,
   pageNumber:30 }`; its `pageNumber << page_shift` roots the surface's own page table. The base descriptor also
   carries `pixel_format`, `offset`, `alloc_size`, `width`/`height`, `bytes_per_row`, `plane_count`, and flag
   bits `readOnly` and (second form) `hostOnly`. A first-form descriptor is widened to the second form's fields
   (reims `upgrade_descriptor`); it is never host-only.
3. **Walk the pages.** `page_count = ceil(alloc_size / PAGE)`. The root page table is one or two levels: with at
   most one page of entries (`PAGE/4`), the root entries name the surface's pages directly; beyond that, each
   root entry names a second-level table of up to one page of page entries. Each entry must be `valid` and, at
   the leaf, not a table; its `pageNumber << page_shift` is the guest-physical page, aliased to a host pointer.
   A bad root/leaf entry is BadArgument; an unreachable page is NoMemory. A `hostOnly` surface lists no guest
   pages (a real host allocates its backing).
4. **Record + mark mapped.** The resolved host page list and geometry are stored in the device's slot for `id`,
   and the guest descriptor's `mapped` bit (bit 1 of the first word) is set so the guest's mapper sees it live.

`unmap(id)` clears the descriptor's `mapped` bit and releases the slot (reims refuses it **Busy** while any
other reference holds the IOSurface; this model tracks no external holders, so it always releases). `test(id)`
writes each 8-byte word of the surface's allocation with its own byte offset — a self-test the guest reads back.

### Interrupts

`apple-gfx-mmio.m` gives the `PGIOSurfaceHostDevice` descriptor its **own** `raiseInterrupt` block, which pulses
`irq_iosfc` — distinct from the GFX device's `irq_gfx`. reims raises interrupt 0 through that block once per ring
command. The Henge model pulses `ctx.iosfc_irq` (via `hv_gicv3_pend`, edge, waking a 1-CPU guest) per command.

### How a mapped surface reaches the renderer (`device.rs`) — and the library gap

In reims the two devices are **not** wired by an explicit host call. The IOSurface host device registers itself
process-globally as the "latest device"; the graphics device's mapper service then calls
`retain_surface_with_mapping_id(id)` on it to resolve an IOSurface by mapping id (reims
`IoSurfaceHostDevice::latest_core` + `HostDeviceCore::retain_surface_with_mapping_id`). `usingIOSurfaceMapper =
true` is exactly what tells the graphics side to resolve IOSurfaces this way instead of over the FIFO.

**`libapplegfx-vulkan` has no equivalent.** Its IOSurface support is entirely the PCI-transport FIFO opcodes
`0x26`–`0x2a` (`src/handlers/iosurface/iosurface.c`: `CmdCreate/Delete/Lookup/Import/UnmapIOSurfaceBacking2`),
keyed by `(surface_id, task_id)` in `src/protocol/resource_registry.c`. There is:

- no `PGIOSurfaceHostDevice` and no second MMIO device;
- no `lagfx_iosfc_mmio_read/write` or `lagfx_iosurface_host_t` in `include/libapplegfx-vulkan.h`;
- no process-global "latest IOSurface device" registry;
- no `retain_surface_with_mapping_id` / mapping-id → surface entry point the renderer could call.

So this bridge resolves every `map` to a concrete host surface (host page pointers + geometry) and records it.
Handing that surface to the renderer used to be the blocker — the stock library offered no public hook to inject
a host-mapped surface and have the renderer find it. **That hook now exists**, added as a Henge-local patch on
the pinned upstream rev.

### The lagfx API addition (implemented)

`android/applegfx/patches/0001-iosurface-register-api.patch` adds two exported functions and one struct to
`include/libapplegfx-vulkan.h`, implemented in `src/protocol/iosurface_api.c`. `android/applegfx/build.sh`
applies the patch after checking out `APPLEGFX_REV`, before the cross build, so the shipped static lib and its
installed headers carry the API. The real shape (as merged):

```c
typedef struct {
    uint32_t          mapping_id;      /* surface_id / registry key      */
    uint32_t          pixel_format;    /* Metal MTLPixelFormat code      */
    uint32_t          width;           /* pixels                         */
    uint32_t          height;          /* pixels                         */
    uint32_t          bytes_per_row;   /* guest stride (0 => width*4)    */
    uint64_t          alloc_size;      /* total backing bytes            */
    const void *const *page_host_addrs;/* host VAs, page_count entries   */
    uint32_t          page_count;      /* number of mapped pages         */
    uint32_t          page_size;       /* bytes per page (e.g. 16384)    */
} lagfx_iosurface_mapping_t;

LAGFX_EXPORT lagfx_status_t lagfx_iosurface_register(
    lagfx_device_t *device, const lagfx_iosurface_mapping_t *mapping);
/* Re-upload current guest-page contents into the existing VkImage (no recreate),
   for a surface the guest keeps redrawing. Falls back to register if unseen. */
LAGFX_EXPORT lagfx_status_t lagfx_iosurface_refresh(
    lagfx_device_t *device, const lagfx_iosurface_mapping_t *mapping);
LAGFX_EXPORT lagfx_status_t lagfx_iosurface_unregister(
    lagfx_device_t *device, uint32_t mapping_id);
```

Internally `register` reuses the library's own IOSurface machinery: it gathers the mapped pages into one
contiguous upload, builds a sampleable `VkImage` via `lagfx_vk_iosurface_create` + `lagfx_vk_iosurface_upload_pixels`
(a staging copy — no external-memory import needed), and binds it in the resource registry as a `TEXTURE` keyed
on `mapping_id`, so the existing `lagfx_resource_lookup_texture(mapping_id)` path — the one `CmdLookupIOSurface`
and the SetFragmentTextures auto-create walk already use — resolves it with no further change. With
`LAGFX_HAVE_VULKAN` off the registry entry is still recorded (no image), keeping `register`/`unregister`
symmetric. Both functions compile with and without Vulkan.

`iosfc_map`/`iosfc_unmap` in `android/pvg_applegfx.c` call these at the point of a successful `map`/`unmap`:
`map` builds the `const void *` page array from `made.pages` and calls `lagfx_iosurface_register`; `unmap` calls
`lagfx_iosurface_unregister(mapping_id)` before freeing the slot. Host-only surfaces (no guest backing) are not
registered. A register failure is non-fatal — the IOSFC `map` still reports success to the guest; only the
on-screen content is missing, and that is logged.

**Freshness.** `register` snapshots the guest pages once. A surface the guest keeps redrawing (the compositor /
wallpaper framebuffer) is kept current by `iosfc_refresh_surfaces()` in the ~60 Hz pump thread: each vblank it
walks the live guest-backed slots under the IOSFC lock and calls `lagfx_iosurface_refresh(mapping_id)`, which
re-uploads the current page contents into the *existing* `VkImage` (a staging-buffer copy, no recreate). The
guest-RAM mapping is fixed, so the page host pointers captured at map time stay valid — only their contents
change. Set `HV_GPU_APPLEGFX_NO_REFRESH=1` to disable (debug / measure cost). Refresh is held under the IOSFC
device lock so a concurrent `unmap` cannot free a surface's pages mid-upload.

### Building Vulkan-on

The default `android/applegfx/build.sh` builds the lib **Vulkan-off**, because the default engine is a fully
static binary (`STATIC = -static` in `android/Makefile`) and a static binary can't dynamically load the tablet's
Vulkan driver. In that configuration `register`/`refresh` record the mapping but build no image.

To build the real rendering path, set `APPLEGFX_VULKAN=1` when invoking `build.sh`:

- A static archive doesn't link a loader, so the cross build needs only the Vulkan **headers** to compile the
  `LAGFX_HAVE_VULKAN` code. `build.sh` fetches pinned `KhronosGroup/Vulkan-Headers`, points
  `APPLEGFX_VULKAN_INCLUDE` at them, and configures with `-Dforce_vulkan=true` (added by
  `patches/0002-forcible-vulkan-build.patch`: a meson option that forces the gate on from those headers without
  relying on meson's loader auto-detection, which can't see a cross loader). It then writes a `VULKAN_ON` marker
  into the output dir.
- The engine resolves the archive's Vulkan symbols against the tablet's loader: when the `VULKAN_ON` marker is
  present, `android/Makefile` adds `-lvulkan` to the engine link and drops `-static` (a Vulkan engine is
  necessarily dynamic). The driver on the Xiaomi Pad 7 is Turnip (`libvulkan_freedreno.so`), already bundled in
  the APK for `Graphics: reims`.

So the remaining work to light up pixels is an integration/packaging choice — ship the engine built with
`APPLEGFX_VULKAN=1` (hence dynamic) alongside the bundled loader — not a missing capability. On-device
confirmation (that Turnip resolves and the compositor surface renders) is the test that still needs the tablet.

### Refresh optimizations (done)

- **Dirty detection.** `lagfx_iosurface_refresh` hashes the current page contents (fast 64-bit FNV-1a) and
  compares against the hash of the last upload, stored on the surface's resource-registry entry
  (`refresh_hash`). When they match — the common case, a static wallpaper — it skips the whole GPU re-upload
  (no staging buffer, no layout transition, no queue submit). The MMIO transport has no `0x3b` BackingUpdate
  signal, so content hashing stands in for a guest dirty hint. Per vblank this costs one read of the surface
  bytes to hash; the GPU work happens only on an actual change.
- **Contiguous zero-gather.** When a surface's mapped pages are contiguous in host memory — the usual case,
  since every guest page is an alias into the engine's single `mmap`'d RAM — refresh/register read them in
  place and hand the base pointer straight to the uploader, with no intermediate gather copy. Scattered
  surfaces still gather into one buffer. (Both behaviours unit-tested.)

### Still open: true zero-copy GPU aliasing

The staging upload still copies the pixels into a device-local `VkImage`. A fully zero-copy path would import
the contiguous host range as `VkDeviceMemory` via `VK_EXT_external_memory_host` so the `VkImage` aliases the
guest pages directly on Turnip — dropping even the staging copy. The contiguous fast path above already produces
exactly the single host pointer that import needs, so the groundwork is in place; wiring it needs on-device
confirmation that Turnip exposes `VK_EXT_external_memory_host` and honours the page alignment, which is tablet
work. It is a further optimization on top of a correct, working path, not a prerequisite for pixels.
- **Mode changes.** The frame buffers are sized to the single advertised mode (1920×1080). Honour `mode_changed`
  with a realloc under `flock` once a second mode is offered.
- **Busy semantics.** reims returns `Busy` on `unmap` while another reference holds the IOSurface; once surfaces
  are shared with the renderer, this model should track holders and do the same.

## Safety / how to drive it

- `HV_GPU_APPLEGFX=<path>` selects this backend (set by the app/`--gpu applegfx`).
- The bridge **engages by default** (claims both windows, returns 1). It logs its experimental status.
- `HV_GPU_APPLEGFX_PROBE_ONLY=1` keeps the old safe behaviour: create the device+display to prove the library
  initialises on-device, then fall back to `pvg_lite` without touching the MMIO windows.
- For a known-working GPU, use `Graphics: reims`.
