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
  guest RAM: disk.img.ram (HV_RAM_KEEP) ◀─ mmap ─    target, read back into the screen's own memory, the
                                                   pointer drawn over it there, shown by the replay's
                                                   presenter on the screen's Vulkan surface
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

0.3.50 on the tablet (2026-11-12, build 55; Zero-copy on, the udmabuf kernel): the first run on Turnip with
`/dev/udmabuf` open. `dma-bufs of host pages: imported, images over them: true`, two frames, then the first
IOSurface texture ended the app: `newTextureWithDescriptor:iosurface:plane:: the Vulkan runtime refused it:
invalid use of new_buffer_with_descriptor: DmaBuf`. The next start marked Turnip as crashed and ran on
Qualcomm's driver, on copies: up to 13 GiB compared in 4.1 s of a 10 s window at 25 frames a second, the
1280x800 RGBA16Float framebuffer a shadowed texture, its pass split in two. The tablet in that run: 590 to
1160 MB of memory available, 4 to 5.6 GB of 6.1 GB of swap used, the hottest sensor 93 to 101 C, the fastest
core held to 2.2 of 2.8 GHz, the JIT cache filled and dropped four times in 330 s.

0.3.52, from that log. Read from the pinned sources, not guessed:

- **Why Turnip died** (`m2v_dma_buf_checks.py`). `Problem::DmaBuf` has one site in metal2vulkan,
  `Device::import_from_source`: the dma-bufs reims-vgpu's source made failed `answers`, the runtime's check
  of its source's contract, which it treats as misuse and reims-vgpu as the app's death. `answers` is the
  chunks' lengths against the request (`chunks_cover`, which one chunk of the request's own length passes
  by construction: what reims-vgpu makes of a 0x7d0000-byte surface within udmabuf's raised limits) and,
  for each descriptor, "is a dma-buf of at least its chunk's length": `fstatfs` must succeed and name the
  dma-buf file system. On Android that call is a `getattr` on a superblock with no SELinux label of its own
  (there is no genfscon for `dmabuf`), which an app's domain is not given: EACCES, so a descriptor udmabuf
  just made is "no dma-buf". That is the likeliest cause; the log could not show it. Now a descriptor is a
  dma-buf where `fstatfs` says so or where the kernel names its file `/dmabuf:` (`/proc/self/fd`) or its
  fdinfo has a dma-buf's `exp_name:`; and an answer that still fails the check is logged in full (the
  request, the chunks, each descriptor's size, file system or `fstatfs` error, and name) and becomes a copy
  of the pages, as on a driver that imports no dma-bufs. Nothing in that path ends the app any more.
- **Why the import must not simply be let through** (`android/gpu/turnip/`). metal2vulkan never flushes or
  invalidates around a dma-buf import: "the CPU's writes through it and the GPU's through the imports meet
  in the same pages with no flush or invalidate (the types are host-coherent)". That holds on the desktop
  GPUs it was measured on. Turnip's own cached allocations on KGSL hold it too: they are asked for with
  `KGSL_MEMFLAGS_IOCOHERENT`. Its *import* of a dma-buf does not: `kgsl_bo_init_dmabuf` passes `.flags = 0`,
  which KGSL takes as write-combined and never I/O-coherent, and Turnip's own comment on imports says it
  has "no way of handling" a producer's cached memory. The Mac's RAM is exactly that: written by the JIT
  through cached mappings. In place on the stock driver, the GPU would read main memory while what the Mac
  just drew sits in a CPU cache: stale blocks in every CPU-drawn window. Cleaning the caches by hand at each
  commit would need each import's uses known there and time per byte at every commit (not measured). So
  Henge builds its own Turnip: the stock
  release's source (whitebelyash/mesa-unified, `turnip/gen8`, pinned) with one change,
  `kgsl_coherent_import.py`: with `TU_KGSL_IMPORT_COHERENT` set an import asks KGSL for
  `IOCOHERENT` and the write-back cache mode, reads back the flags KGSL kept (`IOCTL_KGSL_GPUOBJ_INFO`), and
  under `=require` refuses an import that was not kept coherent with `VK_ERROR_INVALID_EXTERNAL_HANDLE`,
  which metal2vulkan answers with a copy. The CI builds it (three minutes, cached) into the APK as
  `libvulkan_freedreno_henge.so` beside the stock driver.
- **Who decides** (`host.c`'s `in_place_policy`, `m2v_no_host_import.py`). metal2vulkan's dma-buf route is
  on only with `M2V_DMABUF_INPLACE=1`, which the app sets only for a driver file that carries the string
  `TU_KGSL_IMPORT_COHERENT` (Henge's build), together with `TU_KGSL_IMPORT_COHERENT=require`. On the stock
  Turnip and on Qualcomm's driver the route is off and everything is a copy, as before 0.3.37. Settings,
  *Mac's memory in place*: Auto, Off (`in-place-off`), Force (`in-place-force`: in place on a driver not
  known to be coherent, to see what it does). The log's lines, in order: `reims: the Mac's memory in
  place: ...`, `metal2vulkan: no VK_EXT_external_memory_host; dma-bufs of host pages: ...`, and at the
  first import `turnip: dma-buf imports: KGSL keeps them I/O-coherent ... (flags ...)`.
- **Which driver runs** (`VmService`). Henge's Turnip first; if it takes a build of the app down, the stock
  Turnip (on copies); if that does, Qualcomm's. Settings, *Turnip build*: Henge's or Stock. And a death
  Android records as its own doing (low memory, the user, an update; a SIGKILL where the low-memory killer
  is reported as a signal) no longer counts against the GPU: before, any kill of the app during a reims run
  marked Turnip as crashed for the build, or sent the next run to Graphics: GPU.
- **The JIT cache** (`host/jit_region.c`). A full flush set `used = 0` and kept every page: after the first
  fill the engine held all 768 MB for good, whatever the heartbeat's "MB" said, and Android compressed dead
  code into swap that the next fill had to bring back to overwrite. `hv_jit_reset` now gives the pages
  back (`MADV_REMOVE` through the writable alias of the memfd, which unmaps both mappings); every other CPU
  is parked at the dispatcher then, so nothing holds an address in it. Measured on Linux: 64 MB resident
  to 0 at each reset, refilled and run again (`tests/test_jit_region.c`). The heartbeat says `N MB in use
  (M MB given back at flushes)`. `HV_JIT_KEEP=1` keeps the pages. Settings, *JIT cache*: 768 (as before),
  512, 384, 256 MB (`--jit-mb`).
- **Huge pages** (`devices/vmapple.c`). 84 MB of the Mac's 4096 were in huge pages in that run: the kernel
  gives one only where 2 MB lie free together at the first touch. Settings, *Huge pages: Eager*
  (`HV_RAM_THP=2`): a thread touches the RAM in order as the Mac starts (`MADV_POPULATE_WRITE`), the same
  memory the boot takes anyway, while whole runs are still to be had. The app's mapping of the RAM asks for
  huge pages too.

None of the GPU part has run on the tablet. What the first run's log decides, in order: whether
`metal2vulkan: ...: the dma-bufs made of ... do not answer the request` still appears (then its text says
which half of the check, and the pages are copies); whether Turnip's line says KGSL keeps imports
I/O-coherent (if not, copies again, by design); and with both good, whether the `copied host buffers` line's
"MiB live" and "compared" fall to what is left outside the Mac's own pages.

What was looked at and left: a log of the JIT's writes as metal2vulkan's dirty source (its source is KVM's
ioctls, read by the device itself: not a callback a JIT can answer); the scheduler floor reims-vgpu's FIFO
threads ask for (`sched_setattr` is refused an app; the app's process is already in `top-app`); the
comparison's own speed. The other half of a copy's upkeep, the merge back of the GPU's writes at each
completion, had no line at all: `m2v_copy_in_used.py` now prints one every 10 s (`N completions compared X
MiB and wrote Y MiB of the GPU's changes back to the Mac's pages, in Z ms`), so the next log on copies says
whether it is worth working on (its changed blocks are merged eight bytes at a time).

0.3.52 on the tablet (2026-11-12, build 57; three runs, Henge's Turnip, Zero-copy on). It held: `turnip:
dma-buf imports: KGSL keeps them I/O-coherent ... (flags 0x8c140080)`, no `copied host buffers` lines, a
frame drawn in 3 to 6 ms (10 to 13 before), up to 55 frames a second, the JIT cache's memory given back at
each flush. The first run on the new driver had three waits of two seconds and more for pipelines and one
dropped draw; the third, with the driver's pipeline cache filled (5973 files, 70 MB), had none, and held 42
to 48 frames a second for a minute with Safari, Terminal, Finder and Activity Monitor open, then 24 to 35.
The second was ended by Android for memory right after boot, which the app took for what it was. What did
not move: the fastest core held to 1.7 of 2.8 GHz at 70 to 96 C in every run (heat, not something the app
sets); the JIT cache at 384 MB dropped one to three times every 30 s, re-translating 1.4 to 4.3 million
blocks each 30 s; huge pages 14 to 18 MB of 4096 on Eager and none shown on On; the framebuffer still a
shadowed texture, its pass split in two; 1.1 to 2.5 ms to read a frame back and 1.2 to 2.3 ms to show it.
The CPUs' "waited 7.2 s on the GPU" in 30 s is all four CPUs' time summed: about 6 % of it.

0.3.53, from those logs.

- **A frame was copied four times on the CPU after the GPU drew it**: read back into a buffer of the host's,
  copied into a second with the pointer over it, copied into the replay's, copied into the screen's staging
  buffer; a pointer move alone was three of the four. 4 MB each at 1280x800. Now the frame target is read
  back straight into the screen's own memory (`screen_frame`, `replay_frame_begin`), the pointer is drawn
  over it there with the few pixels it covers kept aside and put back before it is drawn again
  (`pointer_drop`, `pointer_lift`), and the presenter shows that memory. One copy, the read-back itself. A
  screen that goes away (the app in the background) has its frame kept by the replay and handed to the next
  one, so a still desktop is shown again without the Mac drawing. 12 MB less held by the app.
- **The screen's swapchain was the window's size** (the Mi Pad 7's panel is 3200x2136): at every frame the system's
  driver cleared all of it and filled it with a scaling blit of the 1280x800 frame, some 50 MB written on
  the GPU for a 4 MB picture, into three or four window-sized buffers (27 MB each). Now, where the window is
  at least the frame's size, the swapchain is made at the frame's own scale and the window's shape (1280x854
  there: the frame's pixels one to one, 27 rows of black above and below) and Android's compositor scales it
  to the window, which the display's own scaler does where the hardware takes the layer. A present is a
  clear of the small image and one copy of the frame. The letterbox is where it was to a pixel, so touches
  land as before. A window smaller than the frame, a rotated surface or a swapchain that is not RGBA8 keeps
  the blit; Settings > Screen scaling: Blit (`HENGE_SCREEN_DIRECT=0`) forces it; and a surface that calls three such swapchains in a row
  suboptimal or out of date without a clean present between gets the window-sized one from then on (Android's
  loader is believed to report suboptimal for a changed rotation only; its source was not re-read for this,
  and the guard is for a loader that does otherwise). The `reims:` line every 10 s now ends with which form
  the screen took, with the window's size, and its own time per present, none of which the app's log had.
- **Huge pages** (Settings; root). Asking was not enough: the kernel gives a 2 MB page only where 2 MB lie
  free in one piece at the first touch. While the Mac runs, each put back after: free memory is compacted once
  before the engine starts (`/proc/sys/vm/compact_memory`; the log has the free memory in pieces of 2 MB and
  more before and after, from `/proc/buddyinfo`); `defrag` never -> madvise or defer -> defer+madvise, so a
  first touch of memory that asked makes room; `enabled` never -> madvise, which is what Linux 6.1 starts
  khugepaged by (as its `hugepage_flags_enabled` reads; not checked on the tablet's kernel), and khugepaged at
  16384 pages every 2 s, gathering a 2 MB range only where at most 64 of
  its 512 pages are untouched (so it adds at most an eighth to what the Mac uses). The engine maps the RAM
  on a 2 MB boundary itself. Eager's walk stops when the tablet has under 768 MB available: the walk of
  build 57 took all 4096 MB in 2.7 s with about 1 GB available, which most likely pushed pages straight
  out to swap, and what comes back from swap comes back small. The heartbeat shows the figure while huge
  pages are asked for, 0 included, and how much of it is mapped whole (`ShmemPmdMapped`), which is the part
  that spares the TLB. How much a tablet this full can keep huge is not known: the next log says.
- **The framebuffer as a shadowed texture** is metal2vulkan's rule, not a failure: a texture over host pages
  is taken in place only where its usage reads it; one the GPU renders into is an image of the runtime's
  own, the pages' texels copied in before a use, kept aside before a write and merged back after
  (`in_place.rs`, `shadow_merge.rs`). All on the GPU: a few copies of 8 MB a frame, none of it the CPUs'.
  The reason is Vulkan's letter (an image over imported memory starts UNDEFINED, and its transitions and a
  pass's load and store may touch any byte bound to it), not a driver seen doing so. Settings > Framebuffer
  in place (off; an experiment; `M2V_RENDER_IN_PLACE=1`, `m2v_render_in_place.py`) asks for such a texture
  in place too where it is over dma-bufs, its rows hold texels alone (10240 = 1280 x 8) and the device's own
  check of the first move passed; every refusal falls back to the shadow with its notice. It could not be
  run before it shipped (no `/dev/udmabuf` off the tablet): a crash with it on turns it off again and leaves
  the driver where it was, and `metal2vulkan: (Henge)` lines say what each written texture's usage was and
  what was asked. Whether it is faster is open: the fetch on Turnip reads the attachment as an input, which
  an image made for the texture's declared usage may lack, and the runtime then renders through a stand-in.
- **The pass split in two** ("its draws keep different attachments out of their subpass", and one for a tile
  dispatch) was read and left: it is decided deep in the runtime's render-pass shapes, costs GPU time alone,
  and the GPU is not what holds the Mac back here (the CPUs wait on it 6 % of the time).
- The JIT cache's note in Settings said to try 384 MB when memory is short; the log says 384 re-translates
  far more. It says 512 first now.

Tested off the tablet: the replay's self-test on lavapipe has the frame's-scale swapchain (1280x800 in
3200x2136 -> 1280x854, pixels one to one, bars black; 1920x1080 next -> 1920x1282), the blit for a window
smaller than the frame, and reims frames written in place (new, kept, new again at another size; kept by
the replay with no screen), clean under the address sanitizer; `reims/selftest.c` against a host build of
the patched library draws a frame into the presenter's memory; the pointer's lift and drop match 0.3.52's
compose at twelve positions, clipped ones and one off the screen included. Not tested: anything on Adreno,
Android's compositor scaling the swapchain, the kernel's huge-page settings, the experiment.

0.3.53 on the tablet (2026-11-13, build 58; two runs of seven minutes, the second with Framebuffer in place
on; JIT cache 512 MB, Huge pages On, the same four Mac apps).

- The screen: `a 1280x827 swapchain at the frame's scale in a 3200x2067 window, scaled by the compositor`, and
  2053x800 in a 3200x1247 window while Android's keyboard was up. No fallback to the blit. A present took 1.5
  to 1.7 ms (no figure from before to set against it).
- A frame, weighted by frames, 0.3.52's third run / the first / the second: drawing 4.75 / 4.02 / 3.43 ms; the
  read back 1.83 / 1.66 / 1.54 ms; the pointer and the hand-over 1.75 / 0.21 / 0.26 ms (it was two copies of
  the frame). Peaks of 50.6 / 55.3 / 52.4 frames a second. The runs drew different things.
- A window of ten seconds with 5 frames and 598 presents: the pointer moving, each a few pixels' work.
- Framebuffer in place: taken (`that texture is an image in place`), no refusal, no crash, the better run by
  the user's eye, and the two-attachment pass no longer split (`m2v_render_in_place.py` has the detail). On
  by default from 0.3.54.
- Huge pages did not come: 18 MB in the first run, 0 in the second. The kernel's own settings had khugepaged
  off outright (`pages_to_scan 1`, `scan_sleep_millisecs 4294967295`, `enabled never`, `defrag never`);
  compacting left 320 MB and then 64 MB free in pieces of 2 MB or more, of a tablet with 0.5 to 1 GB
  available and 3.7 to 4.9 GB in swap. There is no room for them beside a 4 GB Mac on this tablet.
- The first run after the update had two waits of 2 s and one dropped draw again (the driver's own pipeline
  cache starts empty when the driver's build changes); the second none.
- Heat: 93 to 101 C in the first run's first minutes with the fastest core at 2.5 to 2.7 GHz; in the second
  it was held to 1.7 to 1.9 GHz from the third minute while the hottest sensor read 74 to 82 C, so what
  holds it back is not that sensor (the case's temperature, most likely).
- The JIT cache at 512 MB still dropped about once every 30 s: 1.5 to 3.6 million blocks translated each 30 s.

0.3.54, from that log.

- **The JIT cache's refill, looked at and measured off the tablet, not changed.** `tests/bench_translate.c`
  translates glibc's code block by block: a block is 4.4 instructions and becomes 206 bytes of host code
  (the tablet's log: 265 bytes), eleven times the guest's, 61% of the inline part the loads and stores'
  lookup in the software TLB; translating one takes some 3 to 4 thousand instructions, about a microsecond
  on the tablet's cores. At 70 thousand blocks a second that is a few percent of the four CPUs' time, with
  the trips through the dispatcher and the chains patched again after a drop perhaps as much again: an
  estimate, under a tenth of the CPUs' time in all. So the refill is not where the Mac's speed goes, and the
  cures that would end it (a cache that evicts by parts, smaller translations) are deep changes to code
  that cannot be run here. One thing is open: the memory given back at each drop (0.3.52). In the log the
  tablet's available memory jumps by some 400 MB at a drop (998 MB with 50 MB of cache in use, 915 with 120)
  and is back near 570 to 650 by the time the cache is half full, where it stays while the cache grows on:
  so about half of what is given back is taken by something else within seconds, and the cache's second
  half is made room for by reclaim, some 250 MB every 30 s, swap among it. Keeping the pages would end that
  and leave the tablet 250 MB less on average. Which costs the Mac less is not known: Settings > JIT cache
  memory: Keep (`HV_JIT_KEEP=1`) against Give back, with the heartbeat's new swap figures, says.
- **What the next log says instead** (the heartbeat, every 30 s, always on): the time spent translating; the
  share of the blocks that were in the cache before its last drop, by address and by address and memory
  (the same code again against code that is new, and code the Mac paged out and in against code that
  stayed); a block's size in instructions and bytes; code pages written over, blocks two CPUs made at
  once, and how much of what a drop threw away was dead already; what the tablet moved through swap and how
  many page faults waited for it; and how much of the time the Mac's CPU threads ran and how much they stood
  ready without a core. Settings > Engine statistics adds the engine's own report once a minute (`--stats
  60`: TLB misses, slow memory accesses by cause, exceptions, interrupts, the Mac's idle share).
- Framebuffer in place is on unless Settings says Off; a crash with it on turns it off before it blames the
  driver.

0.3.54 on the tablet (2026-11-13, build 59; one run of eleven minutes, Engine statistics on, JIT cache 512 MB,
the same four Mac apps). What the new figures said:

- Translating takes 2.5 to 4.5 s of every 30 s of the four CPUs together: 2 to 4% of their time (the engine's
  own line: 6 to 12% of one CPU). A block is 5 instructions and 265 bytes. So the estimate held.
- 60 to 76% of the blocks translated were in the cache before its last drop, nearly all at the same place in
  the Mac's memory: the same code again, not code that moved. The cache is smaller than what the Mac runs.
  0 to 10% of what a drop threw away was dead; blocks made twice are a thousandth.
- The Mac is never idle with the four apps open (`wfi 0/s (0% idle)` from the second minute to the ninth).
- The Mac's CPU threads ran 76 to 95% of the time and stood ready without a core 3 to 10% of it.
- **The find: the translation lock.** `translation lock 4653/s 41.1% (of one CPU)`. Across the ten reports the
  waits for it follow the code pages the Mac writes over (`smc`), not the translations: 2.3 to 2.5% of one
  CPU's time at 1 to 7 pages a second, 5 to 12% at 37 to 72, 27% at 142, 33% at 165, 41% at 206.
  `hv_invalidate_code` held the lock while it walked every exit chained to another page (`xchained`), to
  find those into the one page written; some 20 thousand such exits are chained each second and the list
  empties only when the cache drops, so it held 300 thousand of them on average.
- No swap figures: Android refuses an app `/proc/vmstat`.

0.3.55, from that log.

- **A code page written over costs what it should.** The exits chained to another page are threaded by the
  page they go to (`HvMachine.xhead`, `xlink`: 65536 buckets), and `hv_invalidate_code` walks that page's
  alone. With 300 thousand exits chained, a write took 1.4 ms before and 3.5 us after (`qemu-user`, where an
  instruction is about a nanosecond; a third of that on the tablet), all of it under the lock every CPU needs
  to publish a translation. `tests/test_xchain.c` checks the threading against the rule it replaced over 139
  thousand random chains, 58 thousand writes and a thousand flushes, pages that share a bucket among them,
  and fails where the walk is broken on purpose.
- A CPU that finds the translation lock held looks again a few times before it sleeps for it: the holder
  publishes a block in a fraction of a microsecond, and the 2 to 4 thousand sleeps a second that remained
  where nothing held the lock long took 6 to 12 us each.
- The heartbeat has the engine's own page faults and how many of them waited for swap or storage
  (`/proc/self/stat`, which an app may read).
- What this should give: where the Mac writes code pages fast (Safari was open; a JavaScript engine writes
  its own code), up to a third or so of one CPU's time back from waiting, and a tenth from the walk itself.
  Where it does not, nothing. Not measured on a tablet.

What the statistics leave to do, by size, none of it started: the cache is too small for the code the Mac
runs (768 MB, the default, drops less; memory is what 512 was chosen for); `tlbi acks` 4 to 10% of one CPU
(a CPU waits for the others to take a TLB invalidation); the indirect-branch caches refilled after each of
3 to 5 thousand process switches a second (200 to 460 thousand fills a second); `page end` 50 to 140 thousand
slow memory accesses a second.

0.3.54 again on the tablet (2026-11-13, a run of forty minutes; JIT cache at its 768 MB, Huge pages off,
Engine statistics on). The larger cache holds what the Mac runs far better: 2 to 7 thousand blocks translated
a second from the tenth minute on, against 30 to 90 thousand at 512 MB, and most half-minutes with no drop at
all. The translation lock's waits still follow the code pages written (20 and 23% of one CPU's time at 106 and
135 pages a second), which 0.3.55 answers. The Mac's CPU threads ran 84 to 95% of the time and stood ready
without a core 2.6 to 6.8% of it; the Mac switched address space 16 to 34 thousand times a second; `tlbi
acks` 2 to 6% of one CPU. A frame: 3.2 ms to draw, 1.2 to read back, 0.2 for the pointer, 1.4 to present; no
stall, no dropped draw. From the sixth minute the fastest core was held to 2.3 and 1.9 GHz and from the
fifteenth to 1.7, with the hottest sensor at 67 to 78 C; the frame rate sat near 28 for the second half of the
run.

0.3.56: the cores, the engine's side and the kernel's.

- **A fast core of its own for each of the Mac's CPUs** (android/runner.c; `HV_CPU_PIN=0` and Settings >
  Cores for the Mac: Off put it back). The four CPU threads floated over the five fast cores. Now CPU 0 has
  the fastest core, the others the next ones down, and the lowest-numbered fast core is left over: on the Mi
  Pad 7, cores 7, 6, 5 and 4, with core 3 left to the app's GPU threads and Android. The Mac spreads its work
  evenly over its CPUs in the logs, so "the busiest" is CPU 0 for want of a busier one; it also polls the
  devices. The engine's other threads (disks, network, sound, VNC) are kept to the slow cores; not while
  installing.
- **Android's background groups off the fast cores** (root; VmService.coreTuning): `background`,
  `system-background` and `restricted` are narrowed to the slow cores while the Mac runs. `foreground` (the
  compositor, touch) and `top-app` (the engine, the app's GPU threads) keep what they have.
- **Swap**: `vm.page-cluster` 0 where it is more (no read-ahead from swap), the kernel's newer reclaim
  (`lru_gen`) on where the kernel has it and it is off. The log gets zram's compressor, what it stores and
  how well it packs, and any swap that is not zram. A compressor cannot be changed while its swap is in use,
  so that one is the user's to set at boot.
- **Fast cores awake** (root; its own setting): no idle state that takes more than 100 us to leave on the
  fast cores (`pm_qos_resume_latency_us`); the log lists core 7's idle states and their latencies.
- Every one of these is put back when the Mac stops, by the app, and by a root shell that waits for the
  engine's pid where Android has ended the app; a quick stop and start does not undo the new run's settings.
- Not measured on a tablet. What the next log shows of it: the engine's "has core N of its own" lines, the
  app's `cores:` and `swap:` lines, and the heartbeat's "waited ... for a core".

0.3.57, from macOS-uke's `gpu/` folder (surveyed 2026-11-14; its kernel driver, Darwin Turnip and in-guest Metal
plug-in have no counterpart here, see the section below):

- **Mac screen: Half and Screen scaling: Sharp.** macOS-uke draws the desktop at half the panel and doubles each
  pixel (`gpu/plugin/scanout_x2.ll`). Half is the panel's own size halved (`VmService.halfScreen`: 1600x1068 on the
  Mi Pad 7); Sharp is the blit's form with a nearest filter where the fit is a whole number of times
  (`replay/screen_vk.c`), linear with the same geometry otherwise, so touches map as before. A blit that covers the
  whole window no longer clears it first. Compositor still shows Half through the display's own scaler (an even 2x,
  but blended). Tested on lavapipe (`replay/selftest.c`: every one of the 6.8 million pixels the frame's at (x/2, y/2));
  not run on a tablet. 1600x1068's rows are not whole 16 KiB pages (6,835,200 bytes a frame), which nothing here
  is known to need; if the Mac's screen fails at Half only, that is the first suspect.
- **METAL2VULKAN_SHADER_CACHE_OPTIONAL** (`reims/m2v_cache_optional.py`, macOS-uke's switch, which upstream does not
  have): a cache folder that cannot be made tries the app's cache folder, and one that takes no write (storage full)
  is kept for what it holds, instead of refusing the device and ending the app. Type-checked against the pinned
  metal2vulkan; not run on a tablet.

0.3.58, the engine's CPU side, from the statistics of 0.3.54 and 0.3.55 (the list above: `page end`, `tlbi acks`):

- **Page-end loads and stores without the full helper** (`core/mmu.c`, `pageend_access`; `host/enter.S`). The
  inline TLB check refuses any access in a page's last 15 bytes (it allows 15 bytes of every access), so an 8-byte
  load of a page's last word went to the light refill, which sent it on to the full helper: every register and the
  SIMD file saved, the instruction decoded, the access done in C. Plain integer loads and stores with no writeback,
  single or pair (LDR/STR in every addressing form the fast path has, sign-extending, LDP/STP, LDNP/STNP, LDPSW),
  are now done by the light refill itself on the page's host copy when the TLB maps the page (or the refill's own
  walk does); the thunk saves x19..x29 for it. Accesses that really cross into the next page, SIMD, atomics and the
  rest go to the helper as before. `tests/mem_programs.S` `pageend_regs`: 21 such accesses into and out of x19..x30
  and xzr, matched against the CPU running them natively; `pageend` went from 9 helper calls to 4 (the 4 that cross).
  HV_NO_PAGEEND=1 is the old way. Stats: "page-end accesses done light".
- **A TLBI by ASID no longer stops the other CPUs** (`hv_tlbi_drop_remote_asid`, `core/sysreg.c`). It was posted to
  each CPU and waited for (the `tlbi acks`). A CPU not running that address space has none of its entries in its
  live TLB (a switch puts them aside), so the TLBI's own CPU drops that ASID's stash and branch-cache id for it,
  under the same walk-sequence check the TLBIs by page use; one running it, or that raced a switch, is posted to as
  before. And a CPU applying a TLBI by ASID for a process it is not running keeps the running one's TLB, which it
  emptied before. `tests/test_mmu.c` checks both answers, the 8-bit ASID compare and the guards. HV_NO_REMOTE_ASID=1
  is the old way. Stats: "TLBIs by ASID/s".
- **A translation cache kept across boots: looked at, not built.** The tablet's own figures (0.3.54) put translating
  at 2 to 4% of the four CPUs' time, about a microsecond a block. The Mac's kernel and shared libraries are slid to a
  new address every boot, so a kept translation needs every guest address in it moved, and reading a block back from
  storage takes longer than translating it again. What costs is the cache dropping whole while 60 to 76% of what it
  translates again was in it before the drop, which a cache that evicts by parts would answer.
- Checked off the tablet: the engine's tests under qemu-aarch64 (test_mmu 75 checks, test_mem with the new program,
  test_jit, test_exc, test_xchain; test_devices' timer checks fail the same way with and without these changes under
  qemu's timing, as dczva does). Not run on a tablet; the next log's statistics lines say what each changed.

0.3.59, from the 0.3.58 log (2026-11-14, a Mac at Half with YouTube and apple.com playing) and the list after it:

- **Settings > Mouse: Android's pointer** (the default). A mouse moves Android's own pointer, which the display
  draws at its own rate, dressed in the Mac's glyph (`GpuView.nativeReimsCursor`, scaled to the screen), and the
  Mac's pointer is put exactly under it: the USB device beside the keyboard is now a composite one with an absolute
  pointer (`devices/xhci.c`, QEMU's usb-tablet report; HV_USB_TABLET=1, set by the app), driven by the app's
  message 251 (`android/vnc.c`). Clicks carry their position, so they land where the pointer is drawn; the wheel
  stays the relative mouse's. The Mac's pointer is left out of the frames then (`reims_host_native_pointer`), so
  its moves cost no frame. Captured is the old way. `tests/test_xhci.c` drives the controller as a guest driver
  does: both configurations, the report descriptor by interface, endpoint 2's reports, merging and clamping. Not
  run on macOS: if the keyboard or pointer misbehaves, Mouse: Captured restores the old USB device.
- **Posted GPU register writes** (`android/pvg_reims.c`): a write goes on as soon as the app's slot has it; the
  thread's next access waits for it, so order holds and reads see every write. The log had the Mac's CPUs waiting
  1 to 4.7 s of every 30 here (single waits to 11 ms), mostly on gfx +0x1020. The app marks a write done with
  IDLE. `tests/test_reims_link.c` runs the engine's half against a stand-in slot thread. HV_REIMS_POSTED=0: the
  old way.
- **The Mac's clock read in place** (`android/compat/mach/mach_time.h`, `translate_counter`): the engine's time is
  the CPU's counter where its rate converts exactly (24 MHz, or 19.2 MHz times 5/4 = c + c/4, the Mi Pad 7's), so
  translated code reads CNTVCT with `isb; mrs; add` instead of 150 to 180 thousand helper calls a second.
  HV_CLOCK_COUNTER=1: the system clock and the helper, as before.
- **Raced TLBI drops wait for the walk** (`core/sysreg.c`): a remote drop that met the target mid-walk is tried
  again when the walk is over (at most HV_RACE_SPIN_US, 15), not after 64 yields; posts cost about 20 us each.
  Stats: "raced drops still posted after waiting".
- **The shader primer** (`reims/reims_primer.py`): metal2vulkan's pipeline records (descriptors, not
  translations) appended to `$HOME/.cache/henge-pipelines.bin` (32 MiB at most) and warmed on the first device
  of the next run (256 MiB of driver cache data); the log says how many after 60 s. HENGE_PRIMER=0: off.
  Type-checked against the pinned reims-vgpu and metal2vulkan.
- **Mac screen: Half from the window**, and the Mac's screen immersive: the log's window was 3200x2067 (the
  navigation bar), so 1600x1068 never fitted it twice and Sharp fell back to a smooth 1.94x. The screen now hides
  the system bars, and Half halves the window the screen was last shown in.
- **Settings > JIT cache: 1 GB.** The log: the 768 MB cache filled and was dropped whole every ~80 s, and 52 to
  73% of what was translated after was code it had held (translating took 7 to 10% of the time). Dropping by
  parts would answer it properly; not done yet. Nor is presenting reims's frame without reading it back (it needs
  an export from reims-vgpu's frame target and an image shared between two Vulkan devices).

0.3.60, from the 0.3.59 log (2026-11-14, Safari with three tabs, windows moved while videos played):

- **The USB tablet is its own device on port 2** (`devices/xhci.c`): QEMU's usb-tablet whole (vendor 0x0627,
  product 0x0001, its descriptors and report), as macOS guests under QEMU drive it. In 0.3.59 it was interface 1
  of the Apple keyboard, which macOS left alone: the log has no sign it was read, the Mac's pointer stayed where
  it was (Android's pointer "far" from it) and clicks went nowhere. The controller now has a slot per device and
  reports two ports with the tablet (one without, as before). The log says how far macOS takes it ("hengevm: usb
  tablet: addressed", "configuration 1 set", "report endpoint configured", "macOS is reading reports", report
  counts). Until macOS reads it, Android's pointer steers the relative mouse as before, clicks included
  (`hv_xhci_pointer_live`, "hengevm: pointer: ..." says which). `tests/test_xhci.c`: 30 checks.
- **The primer warms where reims-vgpu adopts its device** (`gpu/device.rs` `adopt`): 0.3.59 hooked open_device's
  making of the default device, which never ran (a device adopted earlier answered the open), so the log had no
  primer line. Type-checked against the pinned reims-vgpu and metal2vulkan.
- **A drop of the JIT cache gives back its bookkeeping** (`core/dispatch.c` `release_tables`): the block and slot
  tables (136 bytes per block, hundreds of MB written by a full cache) stayed resident after every drop, and the
  tablet, 0.5-0.8 GB free with 4-5.5 GB in swap, swapped them out and in again. Their pages go back with the
  code's (MADV_DONTNEED; HV_JIT_KEEP=1 keeps both). The heartbeat's "MB of tables".
- **JIT cache: 1 GB goes back to 768 MB once** (it can be set again): it still dropped five times in twelve
  minutes while the tablet swapped hard. The Mac's memory stays 4 GB.

0.3.61, less heat for the same speed:

- **Idle CPUs sleep until their timer** (`core/dispatch.c` `wait_for_interrupt`): the Mac's CPUs 1 to 3 look after
  only their own timer, and anything else for them is an interrupt, which wakes them; they slept at most 10 ms, so
  each idle one woke 100 times a second for nothing. Now until their timer's deadline, at most 250 ms
  (HV_WFI_CAP_MS). CPU 0 keeps 10 ms: its tick polls the devices. The heartbeat's wfi/s shows it.
- **GPU register waits in WFE, and as long as answers take** (`android/pvg_reims.c`): the wait before sleeping
  was a yield loop of up to 150 us at full clock. The core now waits in WFE on the slot (the app's store wakes it
  at once; a clock-gated core otherwise), and for four times the recent answers' average plus 10 us (30 to 150
  us; HV_REIMS_SPIN_US fixes it). The heartbeat's "awake up to N us". `tests/test_reims_link.c`: 6 checks.
- **Settings > Big core: Leave out** (the default, `android/runner.c`, `VmService.cores`): the Mac's CPUs on the
  four 2.6 GHz cores, the 2.8 GHz X4 left to the GPU side and Android; Fast cores awake keeps only the Mac's cores
  out of deep idle. Use: as before.

0.3.62, from the 0.3.61 log (2026-11-15, 13 minutes of Safari and video):

- **The USB tablet plugged in after boot** (`devices/xhci.c`): the 0.3.61 log had no "usb tablet" line at all: macOS
  never addressed the device that was on port 2 from the reset. Port 2 is now empty at the reset and connected 2 s
  after macOS configures the keyboard (15 s after its driver starts at the latest), as a device plugged into a
  running Mac. Every step on port 2 is logged ("hengevm: usb tablet: plugged in on port 2", "port 2: write ...:
  reset", "address device ...", each request, "configure endpoints ...", at most 80 lines). `tests/test_xhci.c`: 32.
- **Stale blocks taken off the hash** (`core/dispatch.c` lookup): a new block goes in with the stale ones of its
  bucket unlinked. Safari's JavaScript compiler rewrote one page 207 times and every version stayed: 426 thousand
  stale blocks, chains of 58. The stats line's "stale blocks (N/s taken off)".
- **The JIT cache in four parts** (`evict_part`, HV_JIT_PARTS, 1: whole): when it fills, only the oldest part is
  dropped: exits chained into it go back to their stubs, the hash is made again from the other three, its code and
  tables go back to the kernel. Full drops came every 80-90 s and 45-88% of what followed was translated again.
  `tests/test_jitparts.c` lays the chains out by hand (14 checks); `tests/test_jit.c` runs every program with a part
  dropped after every few translations. The heartbeat: "N drops (N of one part of 4)".
- **Smaller bookkeeping**: a block is 64 bytes (80), a chainable exit 44 (56): about 40 bytes less per block.
- **JIT cache: 512 MB by default**: in parts it stays nearly full, so 512 holds what 768 did on average.

0.3.63, from the 0.3.62 log and run (2026-11-15: no clicks, the Mac slower, Safari reloading a tab):

- **Clicks through the mouse's buttons** (`android/vnc.c` `absolute_event`): the tablet gives the place, the virtio
  mouse the buttons (HV_TABLET_BUTTONS=1: the tablet's bits). The tablet was live in the log (after 39+ port resets)
  but its button bits did nothing. A finger's taps go through the tablet too where it is live, not by steering.
  **Settings > The Mac's own pointer: Shown too** keeps the Mac's pointer in the frames, to see whether it follows.
  The tablet's log: milestones always ("address device", "configuration set", report counts with the number of
  resets), resets counted, the rest capped.
- **JIT cache 768 MB, dropped whole, again by default**; Settings > JIT drops: In parts is 0.3.62's way. 512 MB in
  parts kept code 25-33 s where 768 whole kept it 42 on average (the Mac makes ~15 MB of translations a second), so
  1.7 million blocks were translated every 30 s where 1.0 were, and a drop came 2-5 times per 30 s. Parts smooth
  the drops; at one size they translate as much. "In the cache before its last drop" counts rightly with parts.
- **GPU register waits stay awake at least 60 us** (30: waits of up to 6.1 s per 30 in the log, 3.7 before).
- **Settings > Longer blocks** (HV_SUPERBLOCKS, off): a first measure toward smaller translations (54 bytes per Mac
  instruction now: a block of 5 instructions is 26 words inline, 33 of exits and slow paths, 9 of literals). The
  real work, shared slow paths and smaller exits, is not done.

0.3.64, from the 0.3.63 runs (2026-11-15: the Mac's pointer under Android's only at the middle of the screen):

- **Places on the Mac's screen reckoned by the Mac's screen** (`ScreenActivity.layoutScreen`, `GpuView.nativeReimsMode`,
  `android/vnc.c` `mac_size`): with Graphics: reims the VNC framebuffer is the boot framebuffer (640 wide in the
  logs: every steered place had x <= 639), and the app laid touches and the pointer out by it: right at the middle,
  off towards the left and right. Both sides now use the size the Mac set (1600x1068).
- **The tablet's port reset takes 20 ms** (`devices/xhci.c`, HV_USB_RESET_MS; after 20 resets at once again):
  answered within the register write, macOS reset the port 9546 times in each run before it addressed the device.
- **Fast cores awake: off by default.** With it the tablet held its cores to 1.9 GHz by 3.5 minutes, without by 7.5.
- **Settings > Steady speed** (root, off): the Mac's cores capped at 85% of their top clock.

0.3.65, from the 0.3.64 log (2026-11-15: the mouse works; swap full at 6.1 of 6.1 GB, 0.4 GB free):

- **Where the memory is, once a minute**: "henge: memory:" (the engine: the Mac's RAM and JIT cache, its own, what
  of it is in swap; the tablet's programs, shared memory, file cache, kernel, GPU, and what the swap takes) and
  "henge-gpu: memory:" (the app's process). Nothing said whose the 12 GB of demand on a 7.5 GB tablet were.
- **`build/android/jitsize code.bin`** (`tools/jitsize.c`): the translator's output measured on a file of arm64 code
  without a guest. On libc's .text: 12.5 words per instruction (the tablet: 13-15): a block of 4.5 instructions is
  21 words inline, 13 of exit stubs (1.6 exits of 8), 15 of slow paths (1.8 of 8.2) and 7 of their 16-byte infos.
  The changes within reach (a shared call sequence per 1 MB chunk, a smaller info) come to 7-10%, not the third
  hoped for, for new assembly on every slow path; not made. Sharing literals: 0.1%.

0.3.66, smaller translated code and bookkeeping:

- **Shared tails** (`core/translate.c` `put_tramp`, `chunk_take`; `host/enter.S` `hv_chain_thunk_x7`): a slow
  path's call of the helper, a chainable exit's stub and an unchained exit's lookup each end in `adr` at their
  data, which follows in the code, and a B to one of three tails at the start of every 1 MB chunk. Before, each
  site saved x30, loaded the thunk's address and did a blr, with its data in the pool or after an alignment nop,
  and reloaded x0 after (the thunks do). `jitsize` on libc: 12.49 -> 10.87 words per instruction (13% less: literals
  1.51 -> 0.30, out of line 6.25 -> 5.85). Settings > Compact code: Off (HV_NO_TRAMPS) is the old form.
- **A block's record is 48 bytes** (64): the RAM range as an index, narrow counts, no flush count.

0.3.67, upstream Henge (iOS) merged in, 223 commits from 0.1 to 0.3 (Leviidev/Henge main, 2026-11-15):

- **JIT and TLB**: TCR_EL1, PAR_EL1 and PAN read inline at EL1 (a Sonoma login's helper exits for MRS 42-79K/s ->
  8K/s on the iPhone); a code change drops only the cached branch targets into its page (HV_IJC_PAGES); big range
  TLBIs drop targets only in chunks that hold some; remote drops let walks of other pages through (HV_WALK_LOG);
  chaining and placing a block wait for the instruction caches once. Kept from this branch where both did the same
  thing their own way: the chained-exit list by target page (0.3.55, which the JIT parts of 0.3.62 build on), the
  light refill for page-end accesses (so HV_RECHECK stays off here, on upstream), the shared tails of 0.3.66.
  Upstream's HV_SWITCH_FLUSH ended off by default (a checked skip found a stale entry); so it is here.
  `hv_jit_finish2` and `hv_jit_finish_branch` clean the data cache first on Linux (`__clear_cache`): Apple's
  cores don't need it, Arm's do (QEMU ran stale code without it: test_jit and test_mem crashed).
- **GPU replay**: macOS 13's and 14's GPU drivers (command 0x37 replayed as 0x43; Sonoma's object lists at task
  offset 0x4000); compute pipelines in the GPU trace; the display refresh event (HV_PVG_VBLANK, off).
- **Input**: macOS's virtio mouse driver drops BTN_RIGHT, so the right button is a Control-click (host/vm.c: the
  pointer's serial queue, `android/compat/dispatch/dispatch.h` here). The USB bus has upstream's **Magic Trackpad 2**
  on port 2 and the tablet of 0.3.60 moved to **port 3** (HV_USB_TABLET=1), with its late plug-in and slow port
  reset as before. Settings > Touch: **Magic Trackpad** sends every finger to it (VNC messages 252, 253;
  ScreenActivity `magicTouch`, after ios/Henge/Trackpad.swift): macOS's own gestures, tap and two-finger-tap clicks,
  hold to drag.
- **Instant resume** (Settings, off): Save & Stop sends SIGUSR1; the runner pauses the Mac and writes it with
  host/snapshot.c (LZ4 from third_party/lz4 behind `android/compat/compression.h`; dispatch's _f forms instead
  of blocks); the next Start restores it and flaps the network link. The renderer's own state is not in the file.
- **Mac apps**: upstream's "Mac apps without macOS" runs apps on iOS's own frameworks (Foundation, UIKit), which
  Android has not, so it is not here. What carries over: the **shared folder** (host/share.c, a WebDAV server on a
  Unix socket at 10.0.2.4:80 in macOS; `android/compat/copyfile.h`, `removefile.h`), and Settings > Mac apps
  brings a .dmg, .pkg or .zip from the tablet into its Mac Apps folder, for macOS to open.
- Also: the memory balloon (virtio-balloon: holes punched in the RAM file), disk images whose real size is
  reported and transfers over 128 MB, the guest's virtual counter starting at 0 (CNTVOFF), shutdown and restart
  without leaving the app's process, macOS 12 Monterey booting to Setup. DNS through Apple's resolver (dns_proxy.c)
  is stubbed: libslirp's own, as before.

0.3.68, from the 0.3.67 log (2026-11-22): translating took 1-3.8 s of the four CPUs' 120 s per 30 s, while the
engine waited on swap 200-620 thousand times per 30 s with 530-1070 MB of the tablet's 7.5 GB available and swap
nearly full. Memory, not the JIT, is what the tablet is short of.

- **Settings > Give memory back** (off; HV_BALLOON=0): upstream's memory balloon (devices/virtio_balloon.c), and the
  screen's 🎈 asks macOS for 512, 1024 or 1536 MB back while it runs (VNC message 254, `hv_vm_balloon`). The
  Mac's RAM in memory (a memfd) is now the balloon's file (`b->ram_fd`), so a page handed back is a hole punched
  in it: freed in the engine's and the renderer's mappings at once, with nothing left in swap (madvise on a shared
  mapping would only have unmapped it). With Zero-copy: a udmabuf pins its pages until it goes, and it goes with
  the Metal object over them (reims-vgpu `dma_buf.rs`: "an import goes with its buffer"), which macOS cannot give
  back while it holds it; a dma-buf made later takes the file's pages as they are then. The heartbeat: "N MB given
  back by the Mac (balloon)"; the balloon's own lines say what macOS lists and gives. macOS's AppleVirtIOBalloon
  has not been seen on a tablet (nor on upstream's phone): the log says whether it binds.

0.3.69 (test build, branch cpu-gpu-speed, 2026-11-22):

- **JIT**: CONTEXTIDR_EL1 (read and write), PSTATE.UAO (MSR immediate and register) and SCTLR_EL1 writes that change
  none of M, WXN, E0E or EE are translated inline at EL1, no longer a helper exit each (about a third of the
  0.3.68 log's ~22 thousand MSR exits a second). A SCTLR write that changes one of those four still takes the helper.
- **reims-vgpu dae3e6b / metal2vulkan eac9da3** (2026-11-07), from 1fe0acb / 219f013 (2026-10-29): the last
  reims-vgpu with Apple's ParavirtualizedGraphics glue (upstream deleted it in the next commits, 3db95eb and
  110b536, for its own device). What rides with it for the tablet:
  - the guest's RAM gets udmabuf's dma-bufs of its pages (2b8828c): with Turnip, which imports no host pointer,
    RAM chunks were copies; they can now be imported in place;
  - metal2vulkan itself compares only the copied host buffers a commit's commands use (what
    m2v_copy_in_used.py did; only its merge-back cost line stays) and takes textures that render or that a
    shader writes in place, each checked on the device (what m2v_render_in_place.py did, without the
    tight-rows condition; the script now does nothing there);
  - reims-vgpu keeps and warms pipeline records itself (records.rs): reims_primer.py runs only on older
    revisions, and upstream's warm budget is cut from 4 GiB to 256 MiB;
  - fixes: GPU writes into the Mac's read-only memory dropped as Metal does, pictures landed in pages imported
    as dma-bufs, nil for over-long textures, among 109 commits past fe127c1.
- **Backported** from reims-vgpu's 2026-11-15 dma-buf work (reims_dma_buf_kept.py): udmabuf's limits read
  once a second, not at every dma-buf (56c7608); each dma-buf source's latest four answers kept and given
  again as duplicate descriptors (c52a553: upstream found 9 in 10 asks repeated); runs that fit one dma-buf
  made into it directly (e8ee29f, its second half). Log: "reims-vgpu: (Henge) dma-buf answers: N made, M
  kept". The rest of that work (region files, grouped dma-bufs, view folding) is for RADV's and NVIDIA's
  per-submission import lists and needs the later tree.
- m2v_no_host_import.py follows `DeviceWord::restored`; reims_lenient.py follows `created_or_decided`.

0.3.70 (2026-11-22): the 0.3.69 log's engine died of SIGSEGV twice while Instant resume paused the Mac to save it, with
no word of where. A fault in the engine's own code (not in translated code) now writes "hengevm: signal N in the engine
(thread T)" and its frames as offsets from the engine's start ("hengevm+0x..."); the CI's symbols artifact keeps the
unstripped engine (hengevm-android) they resolve against (addr2line -fe hengevm-android 0x...).

0.3.71 (2026-11-22): Gunyah, phase 2b, after DroidVM (github.com/Droid-VM), whose crosvm boots VMs on Gunyah on a
Redmi Note 14 Pro+ (SM7635) with the tablet's own kernel, 6.1.138-android14-11. The resource manager takes one
memory region at a VM's creation (what the probe found in 0.3.41); DroidVM's "pseudo-unprotected" VMs get the rest
in after the start: the host shares a memfd through /dev/gunyah_share (DroidVM's gunyah-host-share module), and the
guest accepts it itself with the resource manager's MEM_ACCEPT over its rm-rpc channel.
- gunyah-probe --share-window MB [--window-at ADDR]: the probe's guest accepts such a window, stores a mark in it
  and reads back the host's; the report says whether both sides see the same pages. The app's Test Gunyah has four
  new steps (4 MB; 64 MB at the Mac's base 0x70000000; 2 MB at the boot ROM's place, below the base; 512 MB).
- .github/workflows/gunyah-modules.yml builds DroidVM's gunyah-host-share and gunyah-kvcalloc for android14-6.1
  and packs them as a Magisk module (release gunyah-modules), loaded at boot.
The engine (host/gunyah.c) moves to this way once the steps pass.

0.3.72 (2026-11-22): the tablet's answers to 0.3.71's steps. DroidVM's gunyah-host-share module loads, and sharing
4 MB into a running VM works (memparcel handle 5); the guest did not find its resource-manager channel, and sharing
64 MB reset the tablet.
- The guest looked for a node whose first compatible string is "qcom,gunyah-message-queue". It now takes any string
  beginning "gunyah-resource-manager" or "qcom,resource-manager" (as DroidVM's guest module finds the node), reg as
  one or two cells per capability, and reg before or after compatible.
- A window is now made as DroidVM's crosvm makes them (mthp.rs): caches dropped and memory compacted, shmem huge
  pages on advice, populated, collapsed 2 MB at a time, pinned, at a 2 MB-aligned address. A parcel takes an entry
  per folio, and the 64 MB window of 4 KB pages was 16384 of them. The probe says how much of the window is in 2 MB
  pages, and shares nothing over 4 MB that is under 90% in them (--share-small-pages overrides).

0.3.73 (2026-11-22): 0.3.72 on the tablet: the guest drives the resource manager (VM id 128), and windows of 4, 64
and 512 MB in 2 MB pages are shared, accepted, and seen alike by guest and host, at 0x81200000 and 0x72000000. A
window at the boot ROM's place, 0x100000, below the base 0x70000000, is refused (MEM_ACCEPT error 2): the VM's
layout starts at its base. Two steps ask whether a VM with base 0 starts now (0.3.41 said no, before the tree's
memory node was named as the resource manager wants), and whether it takes 64 MB at 0x70000000.

0.3.74 (2026-11-22): 0.3.73 on the tablet: a VM based at 0 is refused when it starts (VM_INIT, error 2), as is
memory below the base, so the boot ROM (0x100000) and the config window (0x400000) can never be Gunyah memory.
The plan is a hand-over: Henge's own CPUs run the ROM and iBoot, and Gunyah's take over once the Mac needs neither,
most likely at iBoot's jump into the kernel (MMU off, x0 = boot_args, CPU 0 only, nothing set up yet). The engine
now logs each time a CPU turns its MMU off, says when x0 is the kernel's boot_args, and after the first logs every
use of memory below the Mac's RAM: one normal boot's log shows the hand-over point and whether anything after it
still reads the ROM or the config window. The Gunyah test asks where the VM's own first region can go (memory
below a high base; a base just under the Mac's RAM) and whether 2 and 3 GB can be had in 2 MB pages.

0.3.75 (2026-11-22): the Gunyah hand-over, first try. 0.3.74's boot log: CPU 0 turns its MMU off three times in
RAM (AVPBooter's loader to iBootStage1, iBootStage1 to iBootStage2, iBootStage2 to the kernel), and the third is the
jump into the kernel with only CPU 0 on. Engine Gunyah now runs the boot ROM and iBoot on Henge's CPUs and stops
CPU 0 there (host/vm.c); host/gunyah.c then patches the kernel collection for a Qualcomm CPU (Apple's own registers
read 0, MIDR reads Apple's value through stubs in the code's padding, the PAC hypercalls succeed, timebase-frequency
set to the counter's 19.2 MHz), starts a VM whose own 2 MB region sits at 0x6fe00000, shares the Mac's RAM into it
after the start in 256 MB pieces, and host/gunyah_shim.S accepts them, loads CPU 0's state from a hand-over page
and returns to the Mac. If anything fails before the VM starts, Henge's CPUs go on. The Mac's RAM must be all in 2 MB
pages: henge-reserve (android/tools/henge_reserve.c) takes it at boot (Settings > Gunyah memory, the henge-gunyah
Magisk module v2) and the kernel is pushed to compact harder. The stage log names RAM pages below 0x70000000 per
stage. The Gunyah test now says whether PAC instructions run on Gunyah's vCPUs and whether each vCPU signs alike,
and has an optional 8 MB share of 4 KB pages.

0.3.76 (2026-11-22): 0.3.75's Gunyah test: PAC instructions run on Gunyah's vCPUs and sign alike on all four (the
same keys, APIAKey 0), MPIDR 0x80000000 as Henge's CPU 0, and 8 MB of 4 KB pages shared and accepted without a reset
(64 MB of them reset the tablet in 0.3.71). So the hand-over no longer needs all of the Mac's RAM in 2 MB pages: each
2 MB piece is classed by its page frame (pagemap, kpageflags' THP flag), runs of 2 MB pages go in pieces of up to
256 MB and runs of 4 KB pages in pieces of 8 MB, up to 640 pieces (the hand-over table is 16 KB now). The 2 and 3 GB
test steps, made beside a 3 GB reserve, had Android kill the app; they are replaced by the reserve's own count
(--reserve-status), which takes no memory. The Gunyah memory card says what is reserved in words, with Copy.

0.3.77 (2026-11-22): 0.3.76's first hand-over on the tablet: the JIT stopped at iBoot's jump into the kernel, the
kernel was found and patched (17 reads, 5 writes of Apple's registers, 5 MIDR reads, 19 PAC hypercalls, 4
timebase-frequency properties), the VM started, the Mac's 3 GB were shared as 12 pieces of 2 MB pages, and the tablet
reset once the vCPUs ran, before any of the shim's lines reached storage. Untried until then: 99 doorbells in a started
VM, and 3 GB in 12 pieces accepted. The Gunyah test now tries those without the Mac (--bells with 4 CPUs; the
reserved memory as the window, in 256 MB pieces, every piece accepted: 1 GB, 3 GB, then everything). The engine
forces the shim's progress to storage (per piece), counts 2 MB pages by page frame (pinning first; smaps had said 844
of a reserve that was all 2 MB pages), and HV_GUNYAH_BELLS=0 leaves the doorbells out. The app no longer turns the
JIT's in-memory RAM off after a Gunyah run (reims could not take the root engine's memory: "Protocol error").

0.3.78 (2026-11-22): 0.3.77's Gunyah test passed everything the Mac's VM has (4 CPUs, 99 doorbells, 3 GB in 12
pieces accepted), and its hand-over still reset the tablet: no vCPU exit at all after GH_VCPU_RUN (not even the
shim's first report, now forced to storage), then the hypervisor's watchdog bit ("cpu alive mask from last pet 0f",
20 s since the last pet). Two changes: the hand-over page moves to 0x20020000, beside the UART where the probe's
reports always exited (0x6f000000, just under the VM's base, may be memory the resource manager keeps); and the engine
sends each vCPU thread a signal every 2 ms (HV_GUNYAH_KICK_MS) so GH_VCPU_RUN comes back and the thread yields, as a
guest that never stops otherwise keeps its CPU from Android. The test gets a guest that spins 30 s with no exit,
plain and kicked (--spin, --kick), which says whether that is what happened and whether the kicks help.

0.3.79 (2026-11-22): 0.3.78's hand-over ran the shim on Gunyah for the first time, twice, with no reset: the hand-over
page beside the UART exits, the kicks bring every vCPU back, the shim found its channel, read the vCPU (MIDR 0xf0480,
MPIDR 0x80000000 as Henge's CPU 0, PAC key readable) and accepted the first 256 MB piece; the second was refused,
NORESOURCE (2), both times. The probe's 12 pieces were all accepted with its tree region in the VM region's second
megabyte; the engine's took 1.9 MB of it from 64 KB on. The engine now lays the region out as the probe does, and the
shim tries a refused piece four times before it stops.

0.3.80 (2026-11-22): the Mac ran on Gunyah for the first time (0.3.79): the layout fix let the shim accept all 12
pieces, load CPU 0's state and jump into the kernel. The tablet reset soon after, with nothing more in the log (the
engine wrote only on unusual events). Now every device page the Mac touches first, and every second for a minute (then
every ten) each vCPU's exits and last access, are forced to storage, so the log's end says how far the kernel got.

0.3.81 (2026-11-22): 0.3.80's log showed why the Mac went nowhere after the jump: the shim reports the jump and then
loads x0-x30 from the hand-over page, and the engine had stopped answering that page at the report, so those 31
loads (0x20020200-0x200202f0) went to Henge's devices, which answered 0. The kernel started with every register zero
(x0 should be boot_args), made no more exits for 30 s, and did not reset the tablet this time. The page is now the
hand-over's for the whole run.

0.3.82 (2026-11-23): 0.3.81: macOS's kernel ran on Gunyah with its real registers, wrote to the serial port (6404
stores) and panicked at once: "Unexpected debugger trap while SP1 selected at pc 0xfffffe002e5e1828", ESR
0x02000000 (an instruction the CPU or Gunyah does not take; the JIT did). The tablet stayed up for 4+ minutes. The
engine now reads the Mac's serial output for panic lines and logs the instruction words around each pc named
(decoded where they are system instructions), so the next build can rewrite that instruction.

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

0.3.83 (2026-11-23): 0.3.82 named the trapping word: 0xe7ffdeff, XNU's own TRAP_DEBUGGER, not a CPU gap. The kernel
called panic() this early (x0 3 = DBOP_PANIC), still on SP1, and the trap handler loses the panic's text. The engine
now reads x0..x3 off the first register dump and rebuilds the panic's message from the Mac's RAM ("gunyah: THE PANIC:").

0.3.84 (2026-11-23): 0.3.83 rebuilt the real panic: "Synchronous exception taken while SP1 selected at pc
0xfffffe0019e6404c", a fault in the kernel's earliest C code (x8 0xfffffe001e504000; the trap's stale FAR,
0xfffffe001e504080 = x8 + 0x80, past the kernel's data). Its text was cut at x27 (format longer than 512 bytes), losing
ESR and FAR. The format buffer is now 3 KB, and the instructions at the first fault's pc and (PAC-stripped) lr are logged.

0.3.85 (2026-11-23): 0.3.84's full panic: ESR 0x72000002 = EC 0x1c, a pointer-authentication failure (DA key) at
AUTDA x2, x3 (pc's word 0xdac11862) on a NULL pointer. The tablet's CPU has FEAT_FPAC (a failed AUT faults at once),
and the kernel's keys are what Apple's hypervisor would set through the PAC hypercalls, answered "success" with nothing
done. Every MSR SCTLR_EL1 in the kernel now goes through a stub that clears EnIA/EnIB/EnDA/EnDB, and the handed-over
SCTLR has them clear: all PAC/AUT instructions become no-ops, signing and checking alike, so the kernel's XPAC-and-
compare checks pass. The Mac runs without pointer authentication (HV_GUNYAH_KEEP_PAC=1 keeps it).

0.3.86 (2026-11-23): 0.3.85 got past the PAC panic: 6 SCTLR writes patched, the kernel made 143 exits (the serial
port's set-up, last a store to UARTCR at 0x20010030) and then ran on silently with no text and no more exits. Gunyah
gives no view of a vCPU's registers, so the engine now reads the kernel's own log: when vCPU 0's exits stop for 3 s,
XNU's msgbuf (magic 0x063061) is found in the kernel's data and its last 3 KB logged ("gunyah: the kernel's log").
(One run's shim stopped on piece 5 with the resource manager's error 6, ARGUMENT_INVALID; the next run accepted all.)

0.3.87 (2026-11-23): 0.3.86's kernel log: ... "kprintf initialized", "initialize_screen", "pe_arm_init_interrupts",
"found interrupt-controller", "failed to find the timer" (ignored by XNU), then "MACH Reboot" 0.25 ms later: the
panic path's restart, the panic's own text going to the panic buffer and console, not msgbuf. On a stall the engine
now also logs every "panic(cpu" text in the Mac's RAM. And the shim's piece accept, refused ARGUMENT_INVALID (6) on
piece 5 or 6 in half the runs, now tries 64 times 50 ms apart (3 s) before giving up, in case it is a moment's state.

0.3.88 (2026-11-23): 0.3.87: the 3 s accept retry worked (one NORESOURCE on piece 9, then all 12 accepted). The kernel
log is the same ("failed to find the timer", then "MACH Reboot"), and no "panic(cpu" text is anywhere in the Mac's RAM:
this early a panic's text has no buffer and goes only to the video console, drawn into the framebuffer. On a stall the
engine now writes that framebuffer (from msgbuf's "initialize_screen" line) to Henge/gunyah-screen.bmp, and looks for
"Debugger called: <...>" in RAM too.

0.3.89 (2026-11-23): where the kernel dies: right after "failed to find the timer" XNU's arm_init runs pe_init_fiq,
which sets up the GIC redistributor for the timer's FIQ. 0.3.84's register dump already pointed there: x9 0x81ffffff
(the value XNU writes to GICR_IGROUPR0) and the stale FAR x8 + 0x80 (GICR_IGROUPR0 in the SGI frame), so a data abort
on Gunyah's redistributor came first, and the PAC panic was its handler. 0.3.88's screen was black and no panic text
was anywhere: this early a panic has no buffer and no console. Now: (1) the kernel's boot-args get "serial=3 debug=0x8
keepsyms=1 -v", so its console and any panic (with a symbolicated backtrace) come out on the serial port into this
log; (2) before the kernel, the shim makes each GIC access pe_init_fiq will make (GICD, GICR RD and SGI frames, the
ICC group-0 registers) under a guard and logs its value or its fault's ESR.

0.3.90 (2026-11-23): 0.3.89 never reached the kernel: the 0.3.88 engine (root) outlived its app when the update killed
the app, kept its Gunyah VM with the reserved RAM shared into it and the VNC and sound ports, so the new start's first
share failed ("Invalid argument") and the ports were taken. A Gunyah start now ends any engine still running first.

0.3.91 (2026-11-23): 0.3.90's serial console gave the panic: "Unhandled Unknown implementation specific error ...
esr=0x96000010 far=...504080": a synchronous external abort (DFSC 0x10), what Gunyah injects for a GIC access it will
not emulate, 0x80 past the kernel's GICR SGI frame. Yet the shim's GIC probes all worked: GICD_TYPER 0x161001f,
redistributors at 0x10010000 128 KB apart (vCPU 1 at 0x10030000, vCPU 2 at 0x10050000), GICR_IGROUPR0 at 0x10020080
written and read back, every ICC group-0 register fine. So the kernel addresses it differently: its /arm-io/gic reg is
now logged and set to Gunyah's layout if it differs, and the panic's saved state (pc, lr, x0-x28, far, esr) is read
from RAM with the faulting instructions decoded (load/store forms, writeback and pairs, which a hypervisor cannot
emulate, named).

0.3.92 (2026-11-23): 0.3.91 named the faulting instruction: `str w9, [x8, #0x80]!` (b8080d09) in pe_init_fiq, the
GICR_IGROUPR0 store, pre-indexed. A store that also updates its base gives a hypervisor no syndrome (ESR.ISV 0), and
Gunyah answers such a device access with a synchronous external abort (0x96000010); Apple's hypervisor and Henge's
JIT decode the instruction instead. The kernel's GIC layout was right (+0x10000, 4 MB). Fix: kpatch finds the kernel's
exception tables (each MSR VBAR_EL1's ADRP/ADD) and puts host/gunyah_fixup.S in front of their EL1 synchronous
entries: an external abort without a syndrome from an integer LDR/STR pre/post-indexed is carried out as a plain
access (which Gunyah emulates) plus the base update, and execution goes on after it; anything else reaches XNU's
handler as before (its first instruction, a B, re-aimed). Any device, not only the GIC.

0.3.93 (2026-11-23): 0.3.92 worked: 2 exception tables, 4 entries fixed up, and the Mac's kernel booted far on
Gunyah: "Darwin Kernel Version 24.6.0 ... RELEASE_ARM64_VMAPPLE", the VM, IOKit, AMFI, sandbox, AES, PSCI, the
credential manager, BSD ("The Regents of the University of California"), up to "[ PCI configuration begin ]", where
IOPCIFamily (from AppleVirtualPlatform) took another external abort, far ...08000 (PCIe ECAM, device 1). The fix-up now
also carries out integer LDP/STP and SIMD&FP LDR/STR/LDP/STP (any addressing) on devices, the likely forms of a
config-space copy. If it is something else: the panic's backtrace is read (the frame after the exception's) and the
call and the called function are decoded; the shim also reads ECAM at 0x40000000/0x40008000 and PCIe MMIO
0x50000000 to show whether Gunyah passes those windows to Henge at all.

0.3.94 (2026-11-23): 0.3.93's probes found it: ECAM 0x40000000 read fine, but ECAM 0x40008000 and PCIe MMIO 0x50000000
fault in the shim too, with a plain LDR (so not an instruction form). Gunyah hands a fault to the host only inside the
VM's VMMIO ranges, and the resource manager's default when the VM's tree names none is [0, 1 GB) ("vmmio-ranges not
set, using legacy default"). The VM's gunyah-vm-config node now has vmmio-ranges = [0, 0x6fe00000): every vmapple
device window, PCIe ECAM and MMIO included, reaches Henge.

0.3.95 (2026-11-23): 0.3.94's vmmio-ranges changed nothing (the tree grew by it, the shim's ECAM 0x40008000 and MMIO
0x50000000 reads still fault): this tablet's resource manager keeps the [0, 1 GB) VMMIO default. So under Gunyah the
PCIe bus moves below 1 GB, after vmapple's last device: ECAM at 0x31000000 (16 MB, buses 0-15), MMIO window at
0x32000000 (224 MB). Henge's PCIe model also answers there (hv_pcie_alias; the window finds BARs by old or new
address), and the kernel's tree (each pci node's reg, ranges, bus-range, as arm-io offsets or absolute) is rewritten
before the kernel reads it; the node is logged whole. The shim reads 0x31000000 to show the host bridge is there.

0.3.96 (2026-11-23): 0.3.95 got the kernel through PCI configuration (8 devices), virtio storage and APFS, then CPU 0
panicked "cpu 1 failed to boot for the first time" with vCPU 1 at 0 exits. A secondary starts from PSCI CPU_ON with
its MMU and caches off, and Gunyah has no forced write-back at stage 2 (no S2FWB), so it reads memory past the caches:
ResetHandlerData, const_boot_args and cpu_ttep, written by CPU 0 with its caches on and never cleaned by XNU (Apple's
hypervisor forces write-back), can be stale there, and the CPU dies before its first device access. The host now cleans
the lines to memory itself (DC by address from EL0 reaches every core's cache): all of the Mac's resident RAM once
before the vCPUs run, then the kernel's image and data every 50 ms for 3 minutes and every second after. The kernel's
reset_vector reports each CPU it starts and what it read with its caches off against what the caches hold. The helper
threads are joined before the engine returns (detached, they read the RAM after the machine was gone: the segfault
after a stop).

0.3.97 (2026-11-23): 0.3.96 started CPUs 1-3 (each reached the reset vector and read the right CPU table: no stale
lines) and the kernel went on past "Creating 7 util devices", then waited for good, CPU 0 polling two virtio devices
(0x32015000, 0x32019000) 150,000 times a second: "interrupt 71 has no doorbell". Apple's pcie,vmapple1 gives slot s
the INTx line interrupt-base + s (64 + s) and the doorbells stopped at 67, so devices in slots 4-7 (the util storage
device among them) could not interrupt. Doorbells now cover 64-95, every slot.

0.3.98 (2026-11-23): 0.3.97 had every slot's doorbell (127) and CPUs 1-3 up, then the kernel's serial output stopped
right after PCI configuration while CPU 0 read two virtio devices' ISR registers (0x32001000, 0x32019000) 150,000
times a second; at 75 s the engine died of SIGSEGV with no report. Now: after 5 s of serial silence with the CPUs
still running (and every 60 s of it, 4 times), the log names the addresses each vCPU hits most (device, BAR offset,
register, last value), every PCI device's state (virtio status and ISR, its INTx line and doorbell rings, each queue:
made available by the driver, taken and handed back by the device), how often the device thread ran, and the
kernel's own log from its memory (what serial had not printed yet). Every Gunyah thread has a signal stack and 8 MB
of stack, so a crash names its place. A finished disk request wakes the device thread at once instead of waiting
for its next 1 ms pass (hv_machine_wake_hook).

0.3.99 (2026-11-23): 0.3.98's report found it: the two devices CPU 0 polls (slot 2's disk, slot 7's sound) are the
two whose interrupt lines had rung (66 and 71, twice each); their ISR reads 0 every time and their requests were all
handed back. An interrupt storm: a Gunyah doorbell's flags stay set until its receiver clears them or its ack mask
does, and only a receiver may set the masks. The host's irqfd asks for level behaviour (doorbell_mask) with the send
capability the resource manager gave it, which has no RECEIVE right unless the node says source-can-clear: refused,
so after its first ring each line stayed asserted for good and XNU (INTx as level) re-entered its handler at once,
forever. Now the VM, the doorbells' receiver, sets them itself: the shim finds each "qcom,gunyah-doorbell" in its
tree and calls doorbell_mask(cap, all, all) (every ring one assertion that clears itself) and doorbell_receive(cap,
all); the bell nodes also say source-can-clear, so the host's own call works where the resource manager honours it.
If the kernel is still silent 45 s on, vCPU 0's most-read device address is answered once with an abort (MMIO
resume "fault"), so its panic backtrace names the loop. The silence report knows the disks' Apple IDs (106b:1a00).

0.3.100 (2026-11-23): 0.3.99 ended the interrupt storm (127 doorbells clear on delivery) and the kernel ran on: PCI,
the util storage devices, AppleCredentialManager, networking, NFS, the USB keyboard and Magic Trackpad enumerated,
BootPolicy, and APFS mounting disk0s1-s3 for the root volume. Then CPU 0 panicked "cpu 1 failed to boot for the first
time": CPUs 1-3 had reached the reset vector and read the right CPU table, and then nothing (PC 0 in the panic); in a
second run CPU 2 came up and worked (xHCI), 1 and 3 did not. XNU's flush_dcache is only a DSB on Apple platforms
(__ARM_COHERENT_CACHE__), so cpu_start's flush of the new CPU's cpu_data and CpuDataEntries cleans nothing: written
just before CPU_ON, after the last periodic clean, they are read stale by the CPU with its caches off (reset handler,
stacks), and it hangs on reset_vector's "b .". Now the reset vector's first report stops the CPU while the whole of the
Mac's RAM is cleaned to memory (about 70 ms), before it reads any of it. Also: the kicker and a stopping vCPU called
pthread_kill on vCPU threads already joined (the engine's segfault after "the VM stopped"); a thread is signalled
only while it lives now.

0.3.101 (2026-11-23): 0.3.100 brought CPUs 2 and 3 up (each stopped at the reset vector for the RAM clean, then worked:
xHCI, PCI) but not CPU 1: its first report never reached Henge (its first exit seen was the second store), so nothing
was cleaned for it, it read stale CPU data and hung, and XNU panicked "cpu 1 failed to boot" at the root mount. The
Gunyah driver ends GH_VCPU_RUN with -ERESTARTSYS whenever a signal is pending, also right after filling in an MMIO
exit, and resumes that access as handled on the next run: every kick (SIGUSR1 each 2 ms) that landed so dropped a
store or answered a load with stale data, on every vCPU, devices included. exit_reason is now cleared before each run
and an interrupted run that left an exit behind is handled as one (counted per vCPU at the end). The reset vector's
second report also cleans the RAM if the first was missed.

0.3.102 (2026-11-23): 0.3.101 booted macOS's kernel to the end on Gunyah: all 4 CPUs up (113/38/26/30 MMIO exits came
back with a signal and were handled, each once lost), the root volume Macintosh HD mounted (snapshot, root hash
validated), then the kernel pivoted to the Recovery volume's BaseSystem and started launchd ("BaseSystem environment
starting": recoveryOS, chosen by iBoot before the hand-over), which ran its boot tasks up to mount-phase-1. Then the
serial port went quiet, the disk idle and the CPUs nearly so, the kernel's timers still firing. Now: once serial is
quiet 3 s the kernel's own log (msgbuf) is copied to this log line by line ("mac:"), its CoreAnalyticsHub timer
spam counted instead; the Mac's screen is written to gunyah-screen.bmp every 30 s, its place from boot_args' Video
(the last initialize_screen in msgbuf when there is one); the watch thread runs as long as the Mac does.

0.3.103 (2026-11-23): 0.3.102's run stopped at the root device: APFS mounted disk0s1, then waited (the disk had handed
back all 168 requests, nothing pending), AppleFileSystemDriver never published boot-uuid-media, and after 60 s the
kernel wrote boot-command=recover-system and "About to reboot into Recovery!" (that is also why 0.3.101 came up in
recoveryOS). The screen showed the boot progress bar at a third. The Gunyah driver takes a load's answer from the run
area as the next GH_VCPU_RUN starts, then drops it if a signal is already pending (-ERESTARTSYS before running the
vCPU; the next run resumes the load with 0): a kick landing between an exit's handling and the next run turned
device reads into zeros, here a virtio ISR read, so a completion was never seen. Kicks now go only to a vCPU inside
one GH_VCPU_RUN for 20 ms or more (long runs and WFI waits, what they are for); a vCPU busy with device accesses
returns to the host on each of them anyway.

0.3.104 (2026-11-23): 0.3.103 booted recoveryOS on Gunyah to the Boot Picker, but the app's GPU never came ("guest
RAM @henge-ram-N: Protocol error"), so pvg_lite drew it. On Gunyah the Mac's RAM is a memfd made by root
(henge-reserve, or the engine under su); its SELinux label is none an app may read or map, so the kernel dropped the
descriptor from the hand-over message and the app got the byte alone. Root now relabels the memfd at hand-over to an
app's own tmpfs type (appdomain_tmpfs) at the connecting app's exact level (SO_PEERSEC), what a memfd the app made
would carry; no policy is added. The engine logs the labels before and after; the app reports a dropped descriptor
(MSG_CTRUNC) as "Permission denied" rather than "Protocol error".

0.3.105 (2026-11-24): 0.3.104's reims fix worked on Gunyah (the RAM relabelled, handed over, mapped; "the app's GPU
is up"), but the kernel died at once: "kernel collection at pa 0x714d4000 (kernel, __TEXT at 0x100000000): 1 code
ranges, 0 MB" was a user program's Mach-O, not the kernel's, so 0 PAC hypercalls and 0 SCTLR writes were patched and
the kernel's first AUTDA failed (ESR 0x72000002, FPAC), panicking in a loop. The header is now taken only where
boot_args' own mapping puts the kernel's __TEXT (virtBase + (pa - physBase)), looked for from 64 MB below physBase to
the kernel's data end, each candidate logged; with none, or under 1 MB of code, the hand-over is off and Henge's CPUs
go on. henge-reserve's pages, which held the last run's RAM (its kernels and the old panic texts this log kept
finding), are cleared at the engine's start.

0.3.106 (2026-11-24): 0.3.105 cleared the reserved RAM (119 ms) and its stricter search then passed over the real
kernel: the fileset at pa 0x7497c000 has __TEXT at 0xfffffe0007004000, the unslid address its load commands carry,
not virtBase + (pa - physBase) (physBase's page held zeros). So the hand-over was off and the JIT booted recoveryOS.
The kernel's header is now the first fileset (or plain kernel) linked in the kernel's half of the address space;
with the RAM cleared, the user program 0.3.104 patched can no longer come first.

0.3.107 (2026-11-24): read in full, 0.3.105's run (the JIT booting recoveryOS after the hand-over was declined) shows
reims never working on the reserved RAM: the GPU saw 34 register accesses in 3 minutes, "ring of 0x4000 bytes at
0x7ae98000 could not be mapped", "root page 0x1ed0c not mapped: no root node", "root FIFO not created", and
WindowServer died of SIGABRT after 63 s, twice (waiting on a GPU that never started). Every view reims-vgpu makes of
guest pages (the RAM's file mapped again, shared, at a page's offset) was refused, with the reason at debug level.
The app now logs the OS layer's debug lines (the first 60) and, once the RAM is mapped, maps a page of the RAM's file
again itself, anywhere and at a fixed address, with the file's size, access mode and seals: the next log says which
part refuses.

0.3.108 (2026-11-24): 0.3.107 on Gunyah: the hand-over worked (the fileset at pa 0x73818000; 19 PAC hypercalls and 6
SCTLR writes patched) and recoveryOS ran on Gunyah's CPUs to loginwindow, but reims still never started: the ring
(0x79bd8000) and the root page (0x1e813) "could not be mapped", WindowServer aborted every 60 s. The app's own
probe mapped a page of the RAM's file again at a fixed address fine (3072 MB, read-write, seals 0x6, same bytes), and
none of the library's OS-layer debug lines named a refusal, so the view is refused before or around the kernel's
mmap, silently. reims_view_why.py patches the library to say why at warn level: an address in no range of the
memory map (with the ranges), the allocation, the host's remap, no file behind the source, or the mmap's errno.
The panic texts the log showed are the earlier run's report on disk, read by recoveryOS, not a panic of this boot.

0.3.109 (2026-11-24): 0.3.108 named it: every view reims-vgpu makes of guest pages (the IOSurface ring, its
descriptor table, the root page) is "mmap of 0x4000 bytes of fd N at offset ..., prot 0x3, fixed: Permission denied",
with Zero-copy on and off alike, where the app's own map of the same file at a fixed address had passed at the GPU's
start (before the Mac ran on Gunyah). So the GPU's root FIFO was never made and WindowServer aborted every 60 s. A
view refused so is now a duplicate of the registered shared mapping of the RAM (mremap with an old size of 0), which
maps no file anew; its first use is logged. The app re-probes a new map of the file every 10 s and says when that
changes, to time the refusal. The panic finder skips reports kept on disk (their JSON escapes): recoveryOS reads the
old panic report in, and 0.3.107/0.3.108 printed it six times a pass.
