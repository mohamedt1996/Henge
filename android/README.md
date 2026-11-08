# Henge on Android (bring-up)

The engine (arm64 JIT, MMU, vmapple devices) builds for Linux/Android arm64 and passes its tests.
Headless only for now: no GPU (Metal replay needs a Vulkan port), no network (libslirp), no sound,
and no on-device install (the restore stack is iOS/Mac). Boot a disk installed by Henge on an iPhone/iPad.

    make -f android/Makefile          # build/android/hengevm-android, libhengevm.a, test_*
    make -f android/Makefile test     # under qemu-aarch64 when not on arm64

Run in Termux (or `adb shell`) with the files from the iOS install:

    ./hengevm-android --rom AVPBooter.vmapple2.bin --disk Disk.img --aux AuxiliaryStorage \
        --ecid <ECID> --seed <SEED> --ram 4096 --cpus 2

Guest RAM is backed by `<disk>.ram` (set `HV_RAM_FILE` to move it).
`test_mem`'s three `dczva` failures under `qemu -cpu max` are emulator-only (it reports 512-byte DC ZVA).
