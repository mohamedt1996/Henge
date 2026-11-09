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

Not run yet on the tablet. What to expect, in order:

- the build for Android itself (CI's "reims-vgpu for the tablet" step);
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
