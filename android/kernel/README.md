# A kernel with udmabuf for the Xiaomi Pad 7 (uke)

The tablet (DerpFest 17, 2026-10-21) boots Google's generic kernel image unchanged:
`6.1.138-android14-11-g0c3d559bcd85-ab14529422`, `# CONFIG_UDMABUF is not set`. Its `boot` partition holds
only that kernel (header v4, no ramdisk; Magisk lives in `init_boot`), and Xiaomi's drivers are modules
loaded into it.

`.github/workflows/kernel-udmabuf.yml` rebuilds that exact commit of `android14-6.1` with Google's own build
(Kleaf) and two changes to `gki_defconfig`:

- `CONFIG_UDMABUF=y`: `/dev/udmabuf`, a dma-buf of memfd pages, which is what lets a Vulkan driver use the
  Mac's memory in place (`android/gpu/REIMS.md`).
- `CONFIG_MODULE_SIG_PROTECT` off: the modules in `system_dlkm` are signed with the key of Google's build,
  which a rebuilt kernel does not have; with the protection on it would refuse them the symbols they need.

The release string is forced to the original's, so module directories named after it are still found. The
result is `boot-udmabuf.img` (100663296 bytes, as the partition), released as `kernel-udmabuf`.

Untested until someone boots it. Keep the original `boot` image and a computer with fastboot at hand:

    fastboot flash boot boot-udmabuf.img      # try it
    fastboot flash boot boot-uke-original.img # back, from the bootloader, if it does not boot

A system update replaces `boot`; flash again (and rebuild if the kernel's version changed).
