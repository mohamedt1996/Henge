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
