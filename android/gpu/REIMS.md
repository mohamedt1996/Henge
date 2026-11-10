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
wrong the picture is wrong, not the process.

Sixth run: no `fatal`; a plain SIGSEGV in native code two minutes in, after the fetch pipelines went
through (797 interrupts, 2 presents). The stage says only "running", so this build adds a backtrace
to the crash handler (`_Unwind_Backtrace` + `dladdr`: library and offset per frame, the fault
address) and, at the next start, Android's own record of the last death with its tombstone
(`ApplicationExitInfo`, Android 12+).

Seventh run: no segfault this time; 114 presents, 2669 interrupts, then WindowServer's next
pipeline was refused: `Unsupported { feature: RasterOrderGroups, needs: FragmentShaderPixelInterlock }`.
`[[raster_order_group]]` runs a fragment function's body in the fragment interlock, which Qualcomm's
driver lacks; on Android the body now runs without the critical section (the same tiler bet as the
fetch; `m2v_unordered_fetch.py`).

Runs 6 and 8 (0.3.8 and 0.3.9, with the backtrace): the same SIGSEGV inside `vulkan.adreno.so`
(pc 0x2255f4, 0x132cec) from metal2vulkan's `CommandBuffer::commit`, right after the 2-attachment
RGBA16Float pass that fetches: Qualcomm's driver dies committing a pass that reads an attachment as
an input while writing it as a colour (legal with the subpass's self-dependency; a driver bug).
`m2v_unordered_fetch.py` now takes metal2vulkan's STORAGE form instead (the profile claims the pixel
interlock; the attachment is left out of the subpass and read and written as a storage image, with
a barrier between draws, the path RADV/ANV/NVIDIA take) and never emits the interlock
instructions, which answer by the enabled features.

With it, three things against the next gaps rather than after them:
- **Refusals no longer end the app** (`reims_lenient.py`): a pipeline the device cannot build
  answers Metal's own NSError (code 3, "the compiler refused the function"), and a creator with a
  nil answer (textures, buffers, heaps) answers nil, each logged with the runtime's full text. The
  guest handles both as it would on a Mac whose compiler refused a shader; a WindowServer that
  cannot live without the pipeline aborts and launchd restarts it, and the Mac and the app go on.
- **Mac screen** setting (Graphics: reims): 1024x768, 1280x800 (default), 1600x1000, 1920x1080;
  `HV_REIMS_MODE=WxH`. Without the host-memory import every surface is a copy compared on the CPU
  at each commit, so pixels are the first performance lever.
- `disable_argument_buffers` on the device: the plain path has run on this device, the other never.

- **Vulkan driver** setting (Graphics: reims): *Turnip* (default where the build bundles it) or
  *System*. The APK carries Turnip, Mesa's open Adreno driver, as `libvulkan_freedreno.so` (a prebuilt
  for Android's KGSL from whitebelyash's CI of upstream Mesa, MIT), and metal2vulkan opens it directly
  as the ICD through `vk_icdGetInstanceProcAddr` (`m2v_icd_library.py`, `M2V_VULKAN_LIBRARY`): no
  loader, no window, none needed. On Turnip the fetch is exact (`VK_EXT_rasterization_order_attachment_access`,
  the inputs form, flags and all); the interlock and the host-memory import are still absent, so the
  unordered raster order groups and the copy path stay. A Turnip that gives no device falls back to
  the system's driver in the same run, logged.

**0.3.11 booted macOS to the Setup Assistant on the tablet** (2026-11-10), on the system driver.

Input after that: the engine places the mouse relatively, correcting each VNC pointer event by the
guest's actual cursor position, which it read from pvg_lite's display page; with the device in the
app that read nothing, and touches drifted. The app now publishes reims-vgpu's cursor position
through the shared file (`cursor_x/y/seq`, header version 2) and the engine reads it
(`hv_pvg_external_pointer`, a weak hook in host/vm.c); the engine's own display size follows the
reims mode, so the VNC framebuffer, the boot framebuffer and the Mac's screen are one coordinate
space; and a mouse is a mouse in the app (hover, buttons, wheel through as they are), not a finger.

What to expect next, in order:

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

## What the native-macOS-on-Snapdragon projects give this port

Surveyed 2026-11-10: [UniXNU](https://github.com/UniXNU/UniXNU), [macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng),
[macOS-uke](https://github.com/iOSNJBrepo/macOS-uke) and [Snapintosh](https://github.com/Snapintosh/Snapintosh) (with
SnapMetal, SnapAdreno, air2ir3). All four run Apple's kernel natively on a Snapdragon (an EL2 monitor or
a patched kernel, their own Adreno kernel driver, a Metal plug-in inside macOS that compiles AIR for the
Adreno). Henge emulates a Mac and plays its GPU work on Android's Vulkan through reims-vgpu, so none
of their code runs here: no kernel, no monitor, no IOGPU kext, no in-guest Metal plug-in. What does
carry over is what they learned about macOS's GPU clients, and the Turnip route:

- **WindowServer aborts, never degrades, on a nil texture over an IOSurface or a nil sampler**
  (SnapMetal `AdrenoMTLDevice.m`, macOS-uke's plan): a refused creator kills the guest's WindowServer,
  which launchd restarts. `reims_lenient.py` answers nil rather than ending the app, so a gap now costs
  a WindowServer restart loop with the gap named in henge.log, not the run.
- **CoreAnimation's generic pipeline blends through framebuffer fetch.** On a device that answers
  "no fetch" it is built with blending off (black rounded corners, blur and backdrop; no abort); on a
  device that answers "fetch" and then refuses the pipeline, CoreAnimation aborts. The port answers
  "fetch" (the profile claims the interlock) and builds the pipeline through the storage form, so that
  pipeline must keep compiling: a refusal of it is the first thing to look for in a WindowServer loop.
  `CA_DISABLE_GENERIC_SHADERS` and `CA_DISABLE_APL_FILTER` are CoreAnimation's own switches for both
  (set in the guest, not here).
- **Snapintosh's order of WindowServer failures** (`docs/GUI-BOOT-ROADMAP.md`, sections 9-12): the
  10-bit framebuffer format CoreAnimation picks, render-target usages, depth/stencil and
  multi-attachment passes, `no_perspective` interpolation, buffer textures, constant samplers,
  half-float surfaces, mip levels, 16-bit derivatives, then framebuffer fetch; a command buffer that
  never retires latches submission; and the first desktop ran at one frame per 10 s until the host's
  own overhead was cut. metal2vulkan already models all of the shader and format items (it was built
  against macOS guests on other GPUs); the list is the checklist for what a refusal in henge.log means.
- **A persistent shader cache is what makes a second boot usable** (macOS-uke: 107 of WindowServer's
  209 pipelines found in the cache on the second boot). `files/reims-home` holds it here.
- **Turnip, not Qualcomm's driver, is where metal2vulkan on an Adreno has run** (macOS-uke, Snapintosh):
  it has `VK_EXT_rasterization_order_attachment_access` (the fetch becomes exact), and its bugs are
  debuggable. It lacks `VK_EXT_external_memory_host` and the interlock as Qualcomm's does, so the copy
  path and the unordered raster order groups stay. Bundling an Android Turnip (the emulator
  community's prebuilts, loaded directly as the ICD) is the next structural step once the stock driver
  has shown what else it refuses.
