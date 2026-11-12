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

## What is NOT resolved — the IOSFC window (the real blocker)

The IOSFC window is a **second Apple device**, `PGIOSurfaceHostDevice`, that exists only in the MMIO/ARM
transport. `libapplegfx-vulkan` does not implement it and exposes no entry point for it: there is no
`lagfx_iosfc_mmio_read/write`, and its `mmioReadAtOffset`/`mmioWriteAtOffset` register layout is not
reverse-engineered anywhere in the library.

Why this matters: `apple-gfx-mmio.m` sets `desc.usingIOSurfaceMapper = true`, which tells the guest/framework to
route IOSurface sharing through this device's `mapMemory(phys, len, ro, &va)` callback — **not** through the FIFO
`0x26`–`0x2a` opcodes the library implements for the PCI transport (where `usingIOSurfaceMapper` is false). On
Apple silicon, WindowServer composites almost entirely out of IOSurfaces, so without a working IOSurface host
device the renderer has no surface backing even when the GFX command ring runs perfectly.

The bridge therefore answers the IOSFC window with a logging stub (`applegfx_iosfc_read/write`: reads return 0,
writes are swallowed, first 16 accesses logged). The gfx device will initialise and the ring will drain, but
frames are unlikely until the IOSurface host device is built.

### To close it (future work)

1. **RE the PGIOSurfaceHostDevice MMIO protocol.** Capture `offset`/`value` from the stub's logs during a macOS
   boot, or disassemble the guest `AppleParavirtGPU` kext's IOSFC path. Map registers to: ring/doorbell geometry,
   a `mapMemory(phys,len,ro)` request/response, and the completion IRQ on `irq_iosfc`.
2. **Add a library API** (`lagfx_iosfc_mmio_read/write`, or a second `lagfx_iosurface_host_t`) that owns the
   mapper, or implement the device entirely in this glue backed by the engine's RAM (`ctx.ram->host`) — the
   mapping itself is trivial here (guest-physical → `ram->host + (gpa - ram->pa)`), mirroring
   `apple_gfx_mmio_map_surface_memory` in QEMU. The hard part is the register/command protocol, not the mapping.
3. **Route completions** to `iosfc_irq` (already threaded through `applegfx_raise_interrupt`'s sibling path /
   `ctx.iosfc_irq`).
4. **Mode changes**: the frame buffers are sized to the single advertised mode (1920×1080). Honour
   `mode_changed` with a realloc under `flock` once a second mode is offered.

## Safety / how to drive it

- `HV_GPU_APPLEGFX=<path>` selects this backend (set by the app/`--gpu applegfx`).
- The bridge **engages by default** (claims both windows, returns 1). It logs its experimental status.
- `HV_GPU_APPLEGFX_PROBE_ONLY=1` keeps the old safe behaviour: create the device+display to prove the library
  initialises on-device, then fall back to `pvg_lite` without touching the MMIO windows.
- For a known-working GPU, use `Graphics: reims`.
