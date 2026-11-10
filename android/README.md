# Henge on Android

macOS on an arm64 Android tablet or phone. Henge emulates a Mac (the same `vmapple` machine the iOS
app runs), installs macOS on the device from Apple's restore image, and shows its desktop in the app.

**Experimental.** It boots macOS Sequoia 15.6 to the desktop on one tablet, slowly. This page says
what works there, what does not, and what nobody has checked.

- Status as of **0.3.27 (2026-11-10)**.
- Tested on one device: Xiaomi Pad 7 (Snapdragon 7+ Gen 3, Adreno 732, Android 14, Linux 6.1), rooted.
  Nothing here is known about any other device.
- Branch `android-port`. Each push builds an APK and publishes it as the
  [`android-latest`](https://github.com/mohamedt1996/Henge/releases/tag/android-latest) release.

## What works, and what does not

"Works" means it was seen working in a log from the test tablet. "Unconfirmed" means the code is
there and builds, and no run has shown it working (or failing).

| Area | Status | Notes |
|---|---|---|
| Boot to the macOS desktop | **Works** (0.3.11 to 0.3.24, 0.3.27) | JIT engine, Graphics: reims, the system's Vulkan driver, 4 CPUs, 4 GB. |
| Desktop drawing (Graphics: reims) | **Works, slow** | 17 to 27 frames a second at best in animations, with long stalls. |
| 0.3.26 | **Broken: draws nothing** | Every GPU task failed to start. Do not use it. |
| 0.3.27 | **Works** | Fixes 0.3.26. The Mac's CPUs wait 2 to 4 s of 30 on the GPU (18 to 22 before); about 27 frames a second at best. |
| Wallpaper | **Not drawn** | The desktop picture does not appear. Cause unknown. |
| Touch, pointer, on-screen and hardware keyboard | **Works** | Touch: Direct or Trackpad. A captured mouse moves the Mac's pointer itself. |
| Mac's RAM in memory (not on flash) | **Works** (0.3.24) | Handed from the engine to the app as a descriptor. |
| Internet in the Mac | **Works** (0.3.25) | Safari loaded apple.com on the tablet. Heavy pages slow the whole Mac to a crawl. |
| Sound | **Works** | Played through the app since 0.3.16. |
| Install macOS on the device | **Unconfirmed here** | Downloads the restore image and restores it, as the iOS app does. Takes hours; needs about 45 GB free. |
| Vulkan driver: Turnip | **Unconfirmed** | Bundled since 0.3.15. It took the app down on 0.3.16; fixes went in after, and the logged runs since used the system's driver. |
| CPU engine: Gunyah | **Broken: resets the tablet** | Starting the Mac on it panics the Xiaomi Pad 7. No crash record has been recovered yet (`/sys/fs/pstore` was empty). |
| Gunyah with Graphics: reims | **Not done** | The engine switches reims off under Gunyah. |
| Graphics: Vulkan (the older renderer) | **Partial** | Clears, copies and the pointer; no draws. Superseded by reims. |
| Video decoding, camera, USB devices | **Not done** | |

## Why it is slow

Two costs set the ceiling, and neither is fixed:

1. **The CPU is emulated.** Android gives apps no hypervisor, so every instruction of the Mac's
   runs through Henge's JIT. Apps open slowly because of this, whatever the graphics do. Gunyah
   would run the Mac's code directly, and it resets the test tablet.
2. **Every surface is copied.** Neither Adreno driver (Qualcomm's or Turnip) can use the Mac's
   memory directly (`VK_EXT_external_memory_host`), so each window surface is a copy that is
   compared on the CPU at every GPU command buffer.

What has been taken out so far is in [gpu/REIMS.md](gpu/REIMS.md), run by run.

## Requirements

- arm64, Android 10 or later.
- About 8 GB of RAM in the device (the Mac gets 4 GB) and about 45 GB free to install: a 17 GB
  restore image, deletable afterwards, and a Mac of about 26 GB.
- **Root for Graphics: reims in practice.** The app raises `vm.max_map_count` through `su`; without
  it the GPU's views of the Mac's memory run out after about 190 MB. Root is also what Gunyah and
  the engine's scheduling boost need.

## Install and run

1. Install `Henge-android.apk` from the `android-latest` release. Every build is signed with the
   same debug key, so a new APK installs over the old one and keeps the Mac.
2. In the app, install macOS (keep the tablet plugged in), or import an `.ipsw` you already have.
3. In Settings choose **Graphics: reims** and **CPU engine: JIT**, then start the Mac.

Settings that matter:

| Setting | Choices | Use |
|---|---|---|
| Graphics | GPU, Off, Capture, Vulkan, reims | **reims** is the only one that draws the desktop. |
| Mac screen | 1024, 1280, 1600, 1920 | Fewer pixels is faster: every pixel is copied. Default 1280x800. |
| Vulkan driver | Turnip, System | System is the one seen working. |
| CPU engine | JIT, Gunyah | JIT. Gunyah resets the Xiaomi Pad 7. |
| Mac's memory | In RAM, On storage | In RAM. On storage is the fallback where the hand-over fails. |
| GPU memory checks | Used, Everything | Used. Everything is for a window that shows stale pixels. |
| Touch | Direct, Trackpad | |
| Sound | On, Off | |

In macOS, a still wallpaper, no screen saver, and Reduce Motion and Reduce Transparency
(System Settings, Accessibility) lower the GPU work.

## Reporting a problem

The app's log is the evidence: Settings saves it to `Download/Henge/`. The lines that say the most:

- `reims: guest RAM mapped at ... (on the guest's 16 KiB pages)`: must say "on" (0.3.27).
- `command 56 faulted: task N not created`: no GPU task could start, so nothing is drawn.
- `reims: ... frames/s, each ... ms to draw`: the frame rate and each frame's cost, every 10 s.
- `reims: registers: ...`: what a GPU register access costs, and the slowest ones.
- `henge: guest N s: ... CPUs waited X s on it`: how long the Mac's CPUs stood waiting for the GPU
  in each 30 s.
- `metal2vulkan: copied host buffers: ...` and `host mappings: ...`: the cost of the copies.

For a Gunyah reset, the kernel's last messages are needed. `/sys/fs/pstore` was empty on the test
tablet, so record them live before starting the Mac, as root:

    dmesg -w | while read l; do echo "$l" >> /sdcard/Henge/gunyah-dmesg.txt; sync; done

## Building

CI does all of this on each push (`.github/workflows/android-apk.yml`).

    android/build_deps.sh             # static zlib, libffi, pcre2, glib, libslirp, mbedtls, curl in ~/deps/arm64
    make -f android/Makefile          # build/android/hengevm-android, libhengevm.a, test_*
    make -f android/Makefile test     # under qemu-aarch64 when not on arm64
    android/gpu/reims/build.sh        # reims-vgpu and metal2vulkan for the tablet (needs rustup and an NDK)

The engine alone runs from Termux or `adb shell`, with a VNC viewer on 127.0.0.1:5900:

    ./hengevm-android --install ~/mac --release 15.6.1 --ram 4096 --cpus 2
    ./hengevm-android --mac ~/mac --vnc 5900

`test_mem`'s three `dczva` failures under `qemu -cpu max` are the emulator's (it reports a 512-byte DC ZVA).

## How it is put together

- **Engine** (`hengevm-android`, a static arm64 program started by the app): the JIT or Gunyah, the
  MMU, the vmapple devices, the disk, NAT networking through libslirp, a VNC server for input, and
  sound over a local socket.
- **App** (`apk/`): settings, the install, the screen, and the Mac's GPU. With Graphics: reims the
  paravirtual GPU device runs in the app on [reims-vgpu](https://github.com/steelbrain/reims-vgpu),
  which draws the Mac's Metal work on Vulkan through
  [metal2vulkan](https://github.com/steelbrain/metal2vulkan). Both are fetched at pinned commits and
  patched for Android by `gpu/reims/android_port.py`.
- The engine and the app share the Mac's RAM and a small file of register slots (`gpu/reims_link.h`).

More: [gpu/REIMS.md](gpu/REIMS.md) (Graphics: reims, every run on the tablet),
[gpu/README.md](gpu/README.md) and [VULKAN.md](VULKAN.md) (the older renderer).
