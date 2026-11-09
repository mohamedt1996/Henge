# Graphics: reims (experimental)

macOS's paravirtual GPU device runs in the Android app, on
[reims-vgpu](https://github.com/steelbrain/reims-vgpu): the host side of Apple's
ParavirtualizedGraphics protocol (the device vmapple's macOS drives), drawing the guest's Metal work
on Vulkan through [metal2vulkan](https://github.com/steelbrain/metal2vulkan). On the tablet that is
the Adreno GPU, through Android's own Vulkan driver, with Android still in charge of it.

This is the same idea as macOS-uke's GPU stack (AdrenoMetal's Metal on metal2vulkan, on the Adreno),
without replacing Android: macOS keeps Apple's own GPU driver, unmodified, and the app is its GPU.
It replaces the home-grown renderer (`replay/`, Graphics: Vulkan), which guessed at the serialized
pipelines; reims-vgpu implements the whole protocol, as QEMU's `apple-gfx` uses it.

```
engine (hengevm, --gpu reims)                     app (libhengegpu.so)
  pvg_reims.c: vmapple's gfx and IOSurface         reims/host.c: rvg_device (gfx), rvg_iosurface_device,
  MMIO windows ── gpu.reims slot ──────────▶         one rvg_display; guest RAM as one memory-map range
  (each vCPU its own slot, futex-woken)              over the RAM file (MAP_SHARED, as reims wants on Linux)
  irq thread ◀── irq_bits ─────────────────          raise_interrupt: the gfx and IOSurface lines, pulsed
                                                   frames: rvg_display_encode_frame into an RGBA8 frame
  guest RAM: disk.img.ram (HV_RAM_KEEP) ◀─ mmap ─    target, read back, pointer drawn over, shown by the
                                                   replay's presenter on the screen's Vulkan surface
```

- `reims_link.h`: the shared file's layout.
- `../pvg_reims.c`: the engine's side; `devices/vmapple.c` calls it through `hv_pvg_external_create`
  before falling back to `pvg_lite`.
- `reims/host.c`: the app's side, shaped like QEMU's `hw/display/apple-gfx-mmio.c` on the same C API.
- `reims/build.sh`: builds `libreims_vgpu.a` for `aarch64-linux-android` from both projects at pinned
  commits; `reims/android_port.py` ports them (Rust names Android `target_os = "android"`, where both
  build for `"linux"` only), with no window (`reims/window_android.rs`) and no video backends.
  CI runs it; when it fails, the APK is built without reims and the app logs that it has none.

## Status

First run on the tablet (2026-11-09): the build for Android works, the engine's file is found, guest
RAM is mapped, and `rvg_device_create` died at once with `reims-vgpu: fatal: ... the Vulkan runtime
refused it: no shader cache directory: no home directory`. metal2vulkan keeps its translated shaders
and pipeline records in `$HOME/.cache/reims-vgpu-vulkan` and refuses to make a device without the
directory; an Android app's process has no `HOME`. `host.c` now sets `HOME` to a folder the app
passes (`files/reims-home`, which outlives the process: the second boot finds the first's pipelines,
which is what macOS-uke found makes WindowServer usable), and `android_port.py` caps the cache at
4 GiB instead of the library's 25.

Second run (same day): device, IOSurface device and display came up, and the app died at
`reims-vgpu: fatal: memory.map.mapping_length_unaligned` while running: macOS's IOSurface mapper
wrote a ring size whose length (size x 16 bytes) is not whole 16 KiB pages, which the library
treats as host death (its docs 6.23). `android_port.py` now rounds the ring's mapping up to whole
pages (the ring's index math keeps the guest's size), `host.c` logs the mapper's setup registers,
and the engine's reims MMIO is self-locked so a GPU access no longer stalls the other devices.

Third run: the mapper's register log showed the death follows the descriptor table write, not a
ring size. The IOSurface device sizes its descriptor table, page mappings and features by the host's
page size (sysconf), equal to the guest's on every host the library supports (Apple silicon 16 KiB
both, x86-64 Linux 4 KiB both); Android arm64 has 4 KiB host pages under a 16 KiB-page guest, so the
4 KiB table mapping died in the map. `android_port.py` makes the device's pages the guest's
(`APV_PAGE_SIZE`).

Fourth run: the mapper set up (ring 0x400 entries, a whole page), the GPU driver's handshake went
through, 14 interrupts reached macOS and a frame was shown; then the first texture over an IOSurface
died: `newTextureWithDescriptor:iosurface:plane:: ... Unsupported { feature: HostMemory, needs:
VK_EXT_external_memory_host }`. Qualcomm's Vulkan driver has no `VK_EXT_external_memory_host`, which
metal2vulkan uses to wrap guest pages as GPU memory, and without it its Importer refused to exist.
`m2v_no_host_import.py` (run by `android_port.py`) gives such a device an importer that imports
nothing, so every no-copy buffer and IOSurface texture takes the runtime's existing copy path
(`copied-host-pages`: a copy of the pages, compared on the CPU at each commit). Slower than imports,
and the first thing to revisit (Turnip has the extension; a bundled Turnip is the long-term route).

Fifth run: the copy path works. 1034 presents, 6466 interrupts, macOS's 1920x1080 mode set, two
minutes of GPU work; then WindowServer's `inplace_copy_lph` pipeline needed framebuffer fetch
(programmable blending), which metal2vulkan builds through `VK_EXT_rasterization_order_attachment_access`
or `VK_EXT_fragment_shader_interlock`, and Qualcomm's driver has neither. `m2v_unordered_fetch.py`
emits the input-attachment form without the order flags on Android (a tiler shades one pixel's
fragments in order in practice; the extension promises what the hardware does). Where the bet is
wrong the picture is wrong, not the process. What to expect next, in order:

- metal2vulkan on the Adreno driver: it needs a Vulkan 1.2 device; what the driver lacks shows as
  `reims` lines in `adb logcat -s henge-gpu`, and a `reims-vgpu: fatal` line ends the app's process;
- speed: every GPU register access crosses processes (a futex round trip), and frames are read back
  to the CPU before they are shown.

The app's process must stay alive while the Mac runs: VmService starts the device before the engine
and stops it after. Without the app the engine waits a minute for it at the first GPU access.

## Testing without the tablet

`reims/selftest.c` plays the engine against the app's half (`host.c`) on reims-vgpu, on any Vulkan
driver: the device, IOSurface device and display come up, gfx and IOSurface register reads and writes
cross through the slots, and a frame of the display is encoded into a frame target and read back.
Built on Linux against a reims-vgpu built for the host (the window crate's Android stub standing in for
SDL3), it passed on lavapipe (2026-11-09):

    cc -DHENGE_REIMS -I<reims>/crates/reims-vgpu-c/include android/gpu/reims/host.c \
       android/gpu/reims/selftest.c libreims_vgpu_c.a -lpthread -ldl -lm -o reims-selftest
    VK_ICD_FILENAMES=<lavapipe>/lvp_icd.json ./reims-selftest /tmp/x

On a host without `/dev/udmabuf` (Android), reims-vgpu views guest memory a 4 KB mapping at a time,
counted against `vm.max_map_count`: at 65530 that covers about 190 MB of guest RAM. The app raises it
through su when Graphics: reims starts, and logs whether it could.
