# A kernel with udmabuf for the Xiaomi Pad 7 (uke)

The tablet (DerpFest 17, 2026-10-21) boots Google's generic kernel image unchanged:
`6.1.138-android14-11-g0c3d559bcd85-ab14529422`, `# CONFIG_UDMABUF is not set`. Its `boot` partition holds
only that kernel (header v4, no ramdisk; Magisk lives in `init_boot`), and Xiaomi's drivers are modules
loaded into it.

`.github/workflows/kernel-udmabuf.yml` rebuilds that exact commit of `android14-6.1` with Google's own build
(Kleaf) and two changes to `gki_defconfig`:

- `CONFIG_UDMABUF=y`: `/dev/udmabuf`, a dma-buf of memfd pages, which is what lets a Vulkan driver use the
  Mac's memory in place (`android/gpu/REIMS.md`).
- `CONFIG_SYSTEM_TRUSTED_KEYS`: the module-signing certificate of Google's build, taken from the stock kernel
  image (Google CI build 14529422, or an `Image`/`boot.img` given to the workflow). The modules in `system_dlkm`
  are signed with that build's key; trusted here, they verify as signed, so `CONFIG_MODULE_SIG_PROTECT` stays
  on and module protection is the same as the stock kernel's. The build checks the certificate is in the result.

`/dev/udmabuf` is root's alone as the kernel makes it (mode 0600, SELinux type `device`). With Settings > Zero-copy
on, the app (with root) gives it a SELinux type of its own and its own UID as owner, mode 0600: only Henge can
open it. Both last until reboot and are redone at each Mac start; with the setting off it is root's alone again.

The release string is forced to the original's, so module directories named after it are still found. The
result is `boot-udmabuf.img` (100663296 bytes, as the partition), released as `kernel-udmabuf`.

Untested until someone boots it. Keep the original `boot` image and a computer with fastboot at hand:

    fastboot flash boot boot-udmabuf.img      # try it
    fastboot flash boot boot-uke-original.img # back, from the bootloader, if it does not boot

A system update replaces `boot`; flash again (and rebuild if the kernel's version changed).
