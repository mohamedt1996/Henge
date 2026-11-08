# Henge on Android (bring-up)

The engine (arm64 JIT, MMU, vmapple devices) builds for Linux/Android arm64 and passes its tests.
- **Network:** libslirp NAT, like the iOS app (`android/build_deps.sh` builds the static deps). DNS comes from
  `/etc/resolv.conf`, Termux's resolv.conf, `SLIRP_RESOLV_CONF`, or falls back to 1.1.1.1/8.8.8.8.
- **Display:** a built-in VNC server (`--vnc 5900`, localhost; set `HV_VNC_ANY=1` to listen on all interfaces)
  shows the boot framebuffer and forwards pointer and keys. The macOS desktop itself is drawn by Apple-GPU
  shader binaries that only Apple's Metal stack can load, so it does not render here.
- **Not yet:** sound, on-device install (the restore stack is iOS/Mac). Boot a disk installed by Henge on an iPhone/iPad.

    android/build_deps.sh             # static zlib/libffi/pcre2/glib/libslirp in ~/deps/arm64
    make -f android/Makefile          # build/android/hengevm-android, libhengevm.a, test_*
    make -f android/Makefile test     # under qemu-aarch64 when not on arm64

Run in Termux (or `adb shell`) with the files from the iOS install:

    ./hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage \
        --ecid <ECID> --seed <SEED> --ram 4096 --cpus 2 --vnc 5900

Then open a VNC viewer (e.g. AVNC) to 127.0.0.1:5900.

Guest RAM is backed by `<disk>.ram` (set `HV_RAM_FILE` to move it).
`test_mem`'s three `dczva` failures under `qemu -cpu max` are emulator-only (it reports 512-byte DC ZVA).
