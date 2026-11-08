# Henge on Android (bring-up)

The engine (arm64 JIT, MMU, vmapple devices) builds for Linux/Android arm64 and passes its tests.
- **Network:** libslirp NAT, like the iOS app (`android/build_deps.sh` builds the static deps). DNS comes from
  `/etc/resolv.conf`, Termux's resolv.conf, `SLIRP_RESOLV_CONF`, or falls back to 1.1.1.1/8.8.8.8.
- **Display:** a built-in VNC server (`--vnc 5900`, localhost; set `HV_VNC_ANY=1` to listen on all interfaces)
  shows the boot framebuffer and forwards pointer and keys. The macOS desktop itself is drawn by Apple-GPU
  shader binaries that only Apple's Metal stack can load, so it does not render here.
- **Install on the device:** `--install FOLDER` fetches the boot ROM from Apple (under 1 MB), downloads the
  macOS restore image (resumable), and restores it with the vendored idevicerestore/usbmuxd stack
  (`android/build_restore.sh`), as the iOS app does. About 45 GB free: 17 GB image (deletable after), ~26 GB Mac.
- **Not yet:** sound.

    android/build_deps.sh             # static zlib/libffi/pcre2/glib/libslirp in ~/deps/arm64
    make -f android/Makefile          # build/android/hengevm-android, libhengevm.a, test_*
    make -f android/Makefile test     # under qemu-aarch64 when not on arm64

Install macOS on the device (keep it plugged in; hours under emulation):

    ./hengevm-android --install ~/mac --release 15.6.1 --ram 4096 --cpus 2

Then boot it:

    ./hengevm-android --mac ~/mac --vnc 5900

Or boot explicit files (e.g. from an iOS install):

    ./hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage \
        --ecid <ECID> --seed <SEED> --ram 4096 --cpus 2 --vnc 5900

Then open a VNC viewer (e.g. AVNC) to 127.0.0.1:5900.

Guest RAM is backed by `<disk>.ram` (set `HV_RAM_FILE` to move it).
`test_mem`'s three `dczva` failures under `qemu -cpu max` are emulator-only (it reports 512-byte DC ZVA).
