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
  the system's driver in the same run, logged. The prebuilt needs `libhardware.so`, `libsync.so` and
  `libnativewindow.so`, platform libraries an app's linker namespace is refused, so `turnip_load.cpp`
  opens it as Android opens a vendor driver: in a child of the exported `sphal` namespace (the hidden
  linker calls from linkernsbypass, BSD-2, vendored under `linkernsbypass/`), and hands metal2vulkan
  the driver's `vk_icdGetInstanceProcAddr` by address (`M2V_VULKAN_GIPA`).

**0.3.11 booted macOS to the Setup Assistant on the tablet** (2026-11-10), on the system driver.

Input after that: the engine places the mouse relatively, correcting each VNC pointer event by the
guest's actual cursor position, which it read from pvg_lite's display page; with the device in the
app that read nothing, and touches drifted. The app now publishes reims-vgpu's cursor position
through the shared file (`cursor_x/y/seq`, header version 2) and the engine reads it
(`hv_pvg_external_pointer`, a weak hook in host/vm.c); the engine's own display size follows the
reims mode, so the VNC framebuffer, the boot framebuffer and the Mac's screen are one coordinate
space; and a mouse is a mouse in the app (hover, buttons, wheel through as they are), not a finger.

0.3.15, input on the open screen. The pointer shook around the finger and a letter typed itself
several times: the app sent each input message from a thread of its own, so they arrived out of
order (a key's release before its press leaves it held on the Mac, repeating; pointer places
arrive shuffled), and the engine sent the whole remaining distance at every pointer event, several
times over before the guest's cursor had moved once. The app now sends input from one thread, in
order; the engine walks the pointer with one thread that sends a step and waits for the guest's
cursor to answer before measuring again (android/vnc.c), and a button waits for the pointer to
arrive. The soft keyboard's hidden field types only what changed and is never empty, so Backspace
keeps working after everything is deleted.

Turnip was never in an APK before 0.3.15: Mesa's Android build exports one symbol, `HMI` (the
driver as Android's hardware module), not `vk_icdGetInstanceProcAddr`, so the CI's check refused
it and the step was allowed to fail. `turnip_load.cpp` now opens the module's `vk0` device as
Android's loader does and answers a `vkGetInstanceProcAddr` over it; the CI fails if the APK
would go out without the driver.

0.3.16. A mouse is captured over the reims screen (Android's pointer capture): its raw movement goes
to the Mac as it is (the app's own VNC message 250, `relative_event` in android/vnc.c), so nothing
steers it to a place on the screen; fingers still do that. Sound: the engine is a static program with
none of Android's audio libraries, so `android/audio_android.c` serves the guest's virtio-snd PCM on
127.0.0.1:5901, pulled at the stream's rate by the clock, and VmService plays it through an
AudioTrack (Settings: Sound). The shader cache was already kept between boots (`files/reims-home`);
the log now says at each start how many files and megabytes it found there.

0.3.16 on the tablet (2026-11-10): Turnip opened ("in a child of the sphal namespace, as Android's
hardware module"), the cache held 1173 files, and the app died at `MTLCreateSystemDefaultDevice():
the Vulkan runtime refused it: Vulkan failure -7` (VK_ERROR_EXTENSION_NOT_PRESENT). reims-vgpu's
window probe opens the *system's* loader, which lists `VK_KHR_surface` and the rest, and adds them
to the instance; Turnip's hardware module, opened without that loader, has none. Android has no
window here, so `android_port.py` leaves the probe out (0.3.17). And since such a refusal is the
library's death, not a status `host.c` can fall back from: a run that dies on Turnip marks Turnip
as crashed for that build of the app, and the next start uses the system's driver, still on reims.

0.3.18, the pointer again. On 0.3.16 the Mac's pointer still jumped between places a few hundred
pixels apart (photographs show it in three or four places within one exposure). The engine's walk
sent a step and measured again at the guest cursor's *first* move; a guest that answers one step in
pieces (X then Y, or over a few frames) was then sent the rest a second time, and a simulation of
such a guest throws the pointer from edge to edge exactly so. The walk now waits until the cursor
has stood still for 40 ms before it measures, and logs a `hengevm: pointer:` line every two seconds
while walking (target, cursor, step, answered and unanswered counts, the gain it has learned).
Settings has Touch: Direct or Trackpad; Trackpad moves the pointer from where it is (the app's
message 250, as a captured mouse does) and reads nothing back.

0.3.20, speed, measured before it is guessed at. The desktop came up on the system's driver (JIT,
4 CPUs) and is slow. Nothing kept the Mac's CPU threads off the tablet's slow cores: each is now
bound to every core but the slowest kind (`hv_host_cpu_thread`, android/runner.c), and with root
the app moves the engine into Android's top-app scheduling groups (`favourEngine`). The heartbeat
says how long the Mac's CPUs stood waiting on the app's GPU in each 30 s, and the app's `reims:`
line every 10 s gives frames per second and each frame's time to draw, to read back and to show.
What those say decides the next step: the per-commit comparison of every copied guest page inside
metal2vulkan (`copied.rs`; it wants a log of dirty pages, which reims-vgpu takes from KVM alone)
is the suspected cost, the frame's own round trip a small one.

0.3.20 on the tablet (2026-11-10): the screen stopped at 391 presents, and the log says how. The
guest waits for FIFO 1's stamp to reach 0x10e700; it holds 0x10e000, then 0x10e100, then 0x10e200,
one step every twenty seconds or so, each step one pending `CMD=0x43` (ExecIndirect3: a Metal
command buffer). So the GPU is not hung: each command buffer takes tens of seconds, the guest's
driver gives up at 2 s, reports, and tries to resume a FIFO that was never faulted. The Mac's CPUs
waited 2.6 s of 120 on GPU registers, so the time is not in the engine's hand-off either; it is
inside the library's execution of a command buffer. 0.3.21 samples it: on a stamp wait report
(every 30 s at most, ten a run) each busy thread and each of the library's threads writes its
backtrace to the log (`sample: thread ... is at:`), and the CI keeps the unstripped
libhengegpu.so as the `symbols` artifact to name the offsets.

0.3.22, three costs that need no measurement to know. (1) The Mac's RAM was a 4 GB regular file on
the tablet's flash, mapped shared (what iOS needs to stay under its memory ledger; Linux has none):
the kernel wrote it back for as long as the guest wrote its RAM, every page faulted again after
each writeback, the boot touched all 4 GB, and the guest's CPUs were held whenever dirty pages
outran the flash. It is now a memfd (`HV_RAM_MEMFD`, devices/vmapple.c) that the app opens as
`/proc/<engine>/fd/<n>`; where Android refuses that open, the app leaves a note and the next start
uses the file again; Settings, Mac's memory: In RAM or On storage. (2) The VNC server sent the
whole boot framebuffer ten times a second, converted a pixel at a time, though nothing in it
changes once macOS draws through the GPU, and the app converted it again and never showed it: an
unchanged picture is no longer sent, the usual format goes out as rows, and the app skips the
pixels under the GPU view. (3) Each of macOS's disk flushes was an fsync of the disk image; it is
fdatasync now (`HV_FLUSH=2`).

The samples of 0.3.21 on the tablet (the system's driver; symbolized by the CI's Symbolize job from
the build's `symbols` artifact): the queue waiters idle in `completion::wait_for_queue` (nothing
submitted is pending: the GPU itself is not busy), most FIFO threads idle on their rings, one
waiting on another's stamp, and wherever a thread was at work it was in
`CopiedBuffers::copy_in` (`sync_blocks`, copying) under `CommandBuffer::commit_now`, or waiting for
the copies' lock in `copy_in` or `merge_back`. A commit compares every copied host buffer with its
pages, whether the command buffer uses it or not; with the guest's RAM a file on flash those pages
had to come back from storage each time. 0.3.23: `m2v_copy_in_used.py` gives a commit the copies
its commands use (the keys of the uses they noted, as a completion already takes the keys they
wrote), every copy where a command reaches memory no use names; a line every 10 s of commits says
how many MiB are live, compared and passed over, and in how long. Settings, GPU memory checks:
Used or Everything (`M2V_COPY_IN_ALL`), for a window that shows stale pixels.

0.3.23 on the tablet (2026-11-10, the system's driver, 1280x800). The comparisons are no longer the
cost: 1740 commits in 10 s compared 8 GiB and passed over 158 GiB in 2.9 s. The desktop still ran
at 17 frames a second at best, each frame 12 ms of the GPU's own work, with the Mac's CPUs waiting
20 s of every 30 on GPU registers (1.5 ms an access), and the guest's driver still gave up on a
stamp after 2 s. Two things in that log:

- The one FIFO thread at work in the stamp report's samples was in
  `Device::new_buffer_with_descriptor`, in `std::fs::File::read`. The only file the runtime reads
  there is `/proc/self/maps`: before it makes a no-copy buffer or an IOSurface texture it asks
  whether the process maps the pages writable, with `PROCMAP_QUERY` on Linux 6.11 and later, and on
  an older kernel (the tablet's) by reading the maps' lines from the first to the pages, up to
  twice an object (the import that fails, then its copy). Without `/dev/udmabuf` every run of a
  view of the guest's pages is a mapping of its own, so the process holds hundreds of thousands,
  and the kernel formats them under the process's map lock, which the views' own `mmap` and
  `munmap` (the register accesses) wait for. Measured on a Linux host: 65 ms a read at 200,000
  mappings below the pages. `m2v_map_asks.py`: `MADV_POPULATE_WRITE` (or `_READ`) over the pages
  alone answers "every page is mapped with the permission" (0.6 to 3 ms for an 8 MiB surface of
  2048 mappings on the same host, and the same answers as the lines for writable, read-only,
  inaccessible and unmapped pages); every other answer still reads the lines. A line every 10 s
  says how many questions were asked, how many read the maps, and the time; `M2V_MAPS_LINES`
  reads the lines every time, to compare.
- `guest RAM (4096 MB) is backed by a file`: the Mac's RAM was on flash again. 0.3.22's memfd is
  named to the app as `/proc/<engine>/fd/<n>`, and opening that is an `open` of a tmpfs file, which
  Android's SELinux policy gives no app (apps use such files through descriptors they are passed),
  so the first start left its note and every later one used the file. The engine now hands the
  descriptor itself to the app over an abstract Unix socket (`gpu/ram_fd.h`, `@henge-ram-<pid>` in
  the header where the file's name was); the note and the file remain the way back. With the RAM
  in memory the comparisons' outliers (200 MiB in 1.3 s where 8 GiB take 2.9 s) should go too:
  those are pages coming back from storage.

Version 0.3.24.

0.3.24 on the tablet (2026-11-10): the RAM arrived as a descriptor and stayed in memory, and no
question read the maps (812 questions in 343 ms at most). The frame rate did not move: 22 frames a
second at best, the Mac's CPUs still waiting 18 to 22 s of 30 on GPU registers, a millisecond an
access. So the maps were not that wait, and what it is has not been measured: an access is a post to
the app's thread, the library's own work, and the answer back, and only their sum was logged.
0.3.26 logs the parts every 10 s (`reims: registers:`): the accesses, the time for a request to
reach the app's thread (the engine stamps the post, `sent_ns`), the time inside the library, and the
four registers that took the most. What is left of the engine's `CPUs waited` is the way back.
And it takes the scheduler out of the short ones: the engine's CPU thread waits awake for 150 us by
the clock before it sleeps (it gave up after 2000 loads, a few microseconds), the app's thread stays
awake as long after an access for the next, and runs at nice -10. `HV_REIMS_SPIN_US` changes the
engine's wait (0: as before).

Version 0.3.26.

0.3.26 on the tablet (2026-11-10): nothing was drawn. Every GPU task of the Mac's failed as it was
made (`command 56 faulted: task 1 not created`), and each of its commands after that (`no task 2`,
2000 of them in 90 s). reims-vgpu makes no isolated task unless the memory map's host address is
whole 16 KiB pages (`whole_pages`, tasks/remote.rs), which it checks without logging, and the app
mapped the RAM wherever the kernel put it, on the tablet's 4 KiB pages: it had landed on a 16 KiB
boundary in every run before, by its neighbours, not by anything the code did. That it did not
this time is inferred (no "refused" line, which every other way to fail the handshake logs); the
address was not logged. 0.3.27 maps the RAM on a 2 MiB boundary (`map_ram_aligned`) and logs the
address and whether it is on the guest's pages; if tasks still fail with the address on them, the
cause is elsewhere and 0.3.26's waiting awake is the next suspect.

The same log has the first split of a register access: 40 us to reach the app and 40 us in the
library, where an access cost a millisecond before the CPU threads waited awake. So the
millisecond was the scheduler, twice an access, and it is gone; what that gives in frames a second
is not known until a run draws.

Version 0.3.27.

0.3.27 on the tablet (2026-11-10): the RAM is on the guest's pages (`mapped at 0x73a5600000`), every
task was made, and the desktop drew: 27 frames a second at best, the Mac's CPUs waiting 2 to 4 s of
30 on GPU registers where they waited 18 to 22. Then Safari opened apple.com, and from about 400 s
everything slowed together: a frame from 10 ms to 100-480 ms to draw, the copies' comparisons from
under 0.5 ms a MiB to about 10, the Mac's CPUs to a quarter of their instructions, a register's way
to the app from 40 us to 446. Things that share only the tablet slowing at once is the tablet short
of something, memory (4 GB of RAM now in memory, the copies at 190-224 MiB, Safari) or heat, and the
log could not say which. 0.3.28 puts the tablet's state in the heartbeat: memory available, swap
used, the kernel's memory pressure, the hottest sensor, and whether the fastest core is held below
its top frequency.

Version 0.3.28.

Upstream pins reviewed (2026-11-12), to clear `memory.map.mapping_length_unaligned` by a bump:
reims-vgpu and metal2vulkan are already pinned at each repo's current `master` HEAD (reims-vgpu
`1fe0acb6`, metal2vulkan `219f0136`, the rev reims-vgpu's own `Cargo.toml` names), both dated
2026-10-29, so there is nothing newer on the default branch to bump to. Each repo's recent work
(2026-11-11) lives on a `development` branch that splits `reims-vgpu-core` into `reims-vgpu-graphics`
and `reims-vgpu-vulkan/src/core/`; every `android_port.py` anchor and `build.sh`'s metal2vulkan
crate-path patch name the old `reims-vgpu-core/src/iosurface/*` layout, so adopting it is a port
rewrite, not a pin bump, and is left for a later, build-tested change. That branch still ends the
process on an unaligned mapping length (`fatal_unless!(total_length % PAGE_SIZE == 0,
DeathKind::MappingLengthUnaligned)` in `core/memory/map.rs`), so it does not handle the ring's
unaligned length: the ring's whole-page rounding in `android_port.py` is kept. (It does move the
map granule to a compile-time `PAGE_SIZE` — 16 KiB on arm64, 4 KiB on x86-64 — which matches what
the device/descriptor page-size edits already force.) No pin moved; the crash is not cleared by any
available bump and the on-device test still stands.

Unaligned mapping length made survivable at the funnel (2026-11-12). The pinned source was read to
find every site that enforces a page-aligned mapping length. There is one fatal site,
`MemoryMap::new_range_mapping` in `reims-vgpu-core/src/memory/map.rs`
(`fatal_unless!(total_length % APV_PAGE_SIZE == 0, MappingLengthUnaligned)`); the other
`MappingLengthUnaligned` references are its `DeathKind` and tests. That one function is the funnel
**every** mapping goes through: the device root's pages and FIFO, the FIFO rings, the IOSurface ring
(`iosurface/regs.rs`) and descriptor table, the GPU task roots (`gpu_task/handshake.rs`), and
`new_contiguous_mapping` all call it. On the pinned commit `APV_PAGE_SIZE` is a **compile-time**
constant (`1 << APV_PAGE_SHIFT`, `APV_PAGE_SHIFT = 14` on arm64), i.e. 16 KiB — *not* the host's
`sysconf` page — so this is not a 4 KiB/16 KiB mix-up at the check itself. The real cause is that the
check is **stricter than the host needs**: the Vulkan platform's `HostVm` (`os/linux/vm.rs`) already
rounds every length to its own 4 KiB host pages in `allocate_anywhere`, `protect_max`, the remaps and
`deallocate`, so a length that is whole 4 KiB host pages but not whole 16 KiB guest pages (macOS's
IOSurface mapper writes such a ring, `ring_size x 16` bytes) would map fine, yet core aborts the
process first. The earlier fix only pre-rounded the ring in `regs.rs`, leaving the FIFO, task-root
and descriptor callers able to reach the fatal check with such a length.

`android_port.py` now rounds the length up to a whole guest page **at the funnel**
(`new_range_mapping`), not just at the ring: an unaligned `total_length` is rounded up to
`APV_PAGE_SIZE` (logged once as a warning), the allocation, the protect and the `Mapping`'s teardown
all span the rounded length so nothing leaks, the rounded tail is backed by the allocation's own
zeros, and the range-coverage check compares the mapped ranges against the *requested* length so the
caller's index math keeps the guest's logical size. A mapping past the end of guest RAM is still
refused (`None`), not fatal, as before. The ring's existing `regs.rs` rounding is kept as a harmless
belt-and-suspenders (map.rs then sees an already-aligned length). The edit is anchored on the exact
pinned text and exits the build if that text changed; it was verified by running the whole
`android_port.py` against fresh clones of both pinned commits (green, and idempotent on a re-run) and
by confirming both map.rs anchors match exactly once. Not yet exercised on the tablet: whether this
clears the crash for good still needs the on-device boot, but no mapping call site can now reach the
fatal length check.

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

0.3.37, a kernel with udmabuf (`android/kernel/README.md`; the tablet booted it 2026-11-11): Settings >
Zero-copy makes `/dev/udmabuf` the app's to open (root: mode 666, the GPU device's SELinux type) and raises
its limits; the engine's RAM memfd is sealed against shrinking, which udmabuf requires; and the runtime's
importer on a device without VK_EXT_external_memory_host keeps the dma-buf route where the driver has
VK_EXT_external_memory_dma_buf (`m2v_no_host_import.py`), so on Turnip a no-copy buffer or IOSurface
texture is the Mac's pages, not a copy. Untested on the tablet: the log says `henge app: zero-copy: ...`,
`metal2vulkan: no VK_EXT_external_memory_host; dma-bufs of host pages: ...`, and the 10 s
`copied host buffers` line shows what is still copied.

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

## GPU backends (not one basket)

`--gpu` chooses the paravirtual-GPU host, so the GPU is not bet on a single implementation:

| Mode | Host | State |
| --- | --- | --- |
| `on` | `pvg_lite` (built in) | minimal device; macOS's driver runs, no Android renderer |
| `vulkan` | the home-grown replay (`pvg_android.c`, air2spv) | experimental; guesses some pipelines |
| `reims` | reims-vgpu in the app, on metal2vulkan | experimental; the furthest-along path |
| `applegfx` | libapplegfx-vulkan (`pvg_applegfx.c`) | a second, independent PVG host, built into the engine by CI (`android/applegfx/build.sh`). On select it creates the real lagfx device + display with the engine's guest RAM and GIC wired, and logs its version; the screen stays on `pvg_lite` until the apple-gfx MMIO↔PCI transport bridge is written |

`vmapple.c` tries the external hosts in order (`hv_pvg_applegfx_create`, then reims's `hv_pvg_external_create`), each gated by its own `HV_GPU_*` variable, then `pvg_lite`. Adding a backend is: a `pvg_*.c` that defines its `hv_pvg_*_create` (0 when not chosen or not built), a `--gpu` name in `runner.c`, the object on the engine link in `android/Makefile`, and a `GPU_MODES` entry in the app. libapplegfx-vulkan is Apple's **PCI** paravirtual-GPU variant (one BAR + MSI-X), while `vmapple` exposes the **MMIO** variant (two windows + GIC) that reims matches. `pvg_applegfx.c` already wires the library's memory and interrupt callbacks to the engine; the remaining work to make it drive the screen is a bridge between the two transports, plus feeding its frames to the presenter.
