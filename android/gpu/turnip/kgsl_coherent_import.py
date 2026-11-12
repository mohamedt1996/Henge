#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turnip on KGSL: an imported dma-buf mapped for the GPU coherently with the CPU's caches, where the
app asks (TU_KGSL_IMPORT_COHERENT). Run by build.sh on the Mesa checkout.

Why. Graphics: reims with Zero-copy gives Turnip dma-bufs of the Mac's own memory (udmabuf of the
guest RAM's pages) to use in place of copies. The Mac's CPUs (the engine's JIT) write that memory
through ordinary cached mappings, and metal2vulkan never flushes or invalidates around an import: it
takes the import for coherent, as it is on the desktop GPUs it was measured on. Turnip's own
allocations on KGSL are: cached memory is asked for with KGSL_MEMFLAGS_IOCOHERENT, so the GPU's
accesses see the CPU's caches. Its *import* of a dma-buf passes no flags at all
(`kgsl_bo_init_dmabuf`, `.flags = 0`), which KGSL takes as write-combined, never I/O-coherent
("We can't enable I/O coherency on uncached surfaces", kgsl's memdesc init): the GPU would read main
memory while what the Mac just drew still sits in a CPU cache, and show stale blocks. Android's own
buffers get away with it because gralloc's lock and unlock flush; nothing flushes guest RAM.

What. With TU_KGSL_IMPORT_COHERENT set (to anything but "0"), an import asks KGSL for
KGSL_MEMFLAGS_IOCOHERENT and the write-back cache mode, the pair the driver already probes at start
(`has_cached_coherent_memory`), and reads the flags KGSL kept back (IOCTL_KGSL_GPUOBJ_INFO):

  - TU_KGSL_IMPORT_COHERENT=require: an import KGSL did not keep I/O-coherent (a GPU without it, a
    kernel that strips or refuses the flag) fails with VK_ERROR_INVALID_EXTERNAL_HANDLE, the
    object released, so the caller copies the pages instead of reading stale ones. The app sets
    this (android/gpu/reims/host.c) and metal2vulkan answers that error with a copy.
  - any other value: best effort: without I/O coherence the import is made as upstream makes it.
  - unset: upstream's behaviour, byte for byte in what it asks of the kernel.

A refused import answers VK_ERROR_INVALID_EXTERNAL_HANDLE (upstream: OUT_OF_DEVICE_MEMORY for every
failure), which is what a refusal is and what metal2vulkan turns into a copy; ENOMEM keeps
OUT_OF_DEVICE_MEMORY. The first import says in stderr (the app's log) which way it went.

The variable's name in the built library is how the app knows this build honours it (host.c looks
for the string in the file), so a stock Turnip is never asked to use the Mac's pages in place.

Each edit must match exactly once, so a changed Mesa stops the build.

usage: kgsl_coherent_import.py MESA_DIR"""
import pathlib
import sys

EDITS = {
    'src/freedreno/vulkan/tu_knl_kgsl.cc': [
        # getenv, strcmp and fprintf, by their own headers.
        ('''#include <stdint.h>
#include <sys/ioctl.h>
''', '''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
'''),
        ('''   int ret;

   req.flags |= kgsl_align_flag(align);

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_IMPORT, &req);
   if (ret)
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to import dma-buf (%s)\\n", strerror(errno));

   struct kgsl_gpuobj_info info_req = {
      .id = req.id,
   };

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_INFO, &info_req);
   if (ret)
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to get dma-buf info (%s)\\n", strerror(errno));
''', '''   int ret;

   req.flags |= kgsl_align_flag(align);

   /* Henge (android/gpu/turnip/kgsl_coherent_import.py): TU_KGSL_IMPORT_COHERENT asks for the
    * import to be I/O-coherent, as the driver's own cached allocations are: the importer's CPU
    * writes the pages through cached mappings and never flushes. "require" fails an import
    * KGSL does not keep coherent, so the caller copies instead of reading stale pages.
    */
   static const char *henge_coherent = getenv("TU_KGSL_IMPORT_COHERENT");
   const bool henge_asked = henge_coherent && strcmp(henge_coherent, "0") != 0;
   const bool henge_required = henge_asked && strcmp(henge_coherent, "require") == 0;
   const uint64_t henge_flags =
      KGSL_MEMFLAGS_IOCOHERENT |
      ((uint64_t) KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT);
   const bool henge_can =
      henge_asked && dev->physical_device->has_cached_coherent_memory;
   static bool henge_said;

   if (henge_required && !henge_can) {
      if (!henge_said) {
         henge_said = true;
         fprintf(stderr, "turnip: dma-buf imports: this GPU has no I/O-coherent memory "
                         "(KGSL); imports are refused, as TU_KGSL_IMPORT_COHERENT=require asks\\n");
      }
      return vk_errorf(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "dma-buf import: no I/O-coherent memory on this GPU\\n");
   }
   if (henge_can)
      req.flags |= henge_flags;

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_IMPORT, &req);
   if (ret && henge_can && !henge_required) {
      /* Best effort: as upstream asks for it. */
      req.flags &= ~henge_flags;
      req.id = 0;
      ret = safe_ioctl(dev->physical_device->local_fd,
                       IOCTL_KGSL_GPUOBJ_IMPORT, &req);
   }
   if (ret) {
      const int henge_errno = errno;
      if (henge_asked && !henge_said) {
         henge_said = true;
         fprintf(stderr, "turnip: dma-buf imports: KGSL refused the first one (%s)\\n",
                 strerror(henge_errno));
      }
      return vk_errorf(dev,
                       henge_errno == ENOMEM ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                             : VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "Failed to import dma-buf (%s)\\n", strerror(henge_errno));
   }

   struct kgsl_gpuobj_info info_req = {
      .id = req.id,
   };

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_INFO, &info_req);
   if (ret) {
      const int henge_errno = errno;
      struct kgsl_gpumem_free_id henge_free = { .id = req.id };
      safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUMEM_FREE_ID, &henge_free);
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to get dma-buf info (%s)\\n", strerror(henge_errno));
   }

   if (henge_asked) {
      const bool henge_kept = (info_req.flags & KGSL_MEMFLAGS_IOCOHERENT) != 0;
      if (!henge_said) {
         henge_said = true;
         fprintf(stderr, "turnip: dma-buf imports: KGSL keeps them %s (flags %#llx)%s\\n",
                 henge_kept ? "I/O-coherent: the GPU sees the CPU's caches"
                            : "NOT I/O-coherent",
                 (unsigned long long) info_req.flags,
                 henge_kept || !henge_required ? "" : "; imports are refused, as "
                    "TU_KGSL_IMPORT_COHERENT=require asks");
      }
      if (henge_required && !henge_kept) {
         struct kgsl_gpumem_free_id henge_free = { .id = req.id };
         safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUMEM_FREE_ID, &henge_free);
         return vk_errorf(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                          "dma-buf import: KGSL did not keep it I/O-coherent\\n");
      }
   }
'''),
    ],
}


def main() -> None:
    root = pathlib.Path(sys.argv[1])
    changed = 0
    for rel, edits in EDITS.items():
        path = root / rel
        text = path.read_text()
        for old, new in edits:
            if new in text:
                continue
            if text.count(old) != 1:
                sys.exit(f'kgsl_coherent_import: {path}: {old.splitlines()[0].strip()!r} not found once; Mesa changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'kgsl_coherent_import: {changed} edits applied')


if __name__ == '__main__':
    main()
