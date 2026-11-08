First Android build of Henge: macOS in an emulator on arm64 Android tablets and phones. **Experimental — not yet tested on a real device.**

### Downloads
- **Henge-android-<version>.apk** — the app. Android 10 or later, arm64.
- **hengevm-android-arm64-<version>.tar.gz** — the engine alone, for Termux or `adb shell`.

### What works
- **Install:** fetches the boot ROM and macOS Sequoia 15.6.1 from Apple and restores it on the device. No iPhone or Mac is needed.
- **Engine:** the arm64 emulator's tests pass under arm64 emulation (only the `dczva` checks fail, and only there).
- **Network:** NAT, with DNS over HTTPS so it works on Android.
- **Screen:** the boot screen, with touch, the on-screen keyboard and hardware keyboards.

### Known limits
- The macOS desktop doesn't render after boot. macOS draws through Apple's GPU and its shader format, which Android GPUs can't run.
- No sound.
- Needs about 45 GB free, about 8 GB RAM on the device (give the Mac 4 GB), and hours to install. Keep the device plugged in.
- Signed with a debug key.

Source: branch `android-port` (`android/`). Build: `android/build_deps.sh`, then `make -f android/Makefile`. The CI workflow builds the APK.
