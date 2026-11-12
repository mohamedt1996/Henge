# Henge's Turnip

Mesa's open Adreno Vulkan driver, built for Android's KGSL from the source of the stock release the APK
also bundles (whitebelyash/mesa-unified, branch `turnip/gen8`, at the commit `build.sh` pins), with one
change:

- `kgsl_coherent_import.py`: a dma-buf imported with `TU_KGSL_IMPORT_COHERENT` set is mapped for the GPU
  I/O-coherent (`KGSL_MEMFLAGS_IOCOHERENT`, write-back cache mode), as the driver's own cached memory is.
  `=require` refuses an import the kernel did not keep coherent (`VK_ERROR_INVALID_EXTERNAL_HANDLE`).

Why: Graphics: reims with Zero-copy hands the driver dma-bufs of the Mac's own RAM, which the Mac's CPUs
write through cached mappings and nobody flushes. See `../REIMS.md`, "0.3.52".

    android/gpu/turnip/build.sh OUT      # OUT/libvulkan_freedreno.so; needs an NDK, meson, ninja, mako,
                                         # pyyaml, glslangValidator, bison, flex

The APK's workflow builds it into `libvulkan_freedreno_henge.so` (cached by this folder's contents);
`.github/workflows/turnip-henge.yml` builds it alone when this folder changes. The app uses it before the
stock driver (`VmService`), and lets the GPU use the Mac's memory in place only on a driver file that
carries the variable's name (`../reims/host.c`, `in_place_policy`).
