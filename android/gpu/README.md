# The Vulkan renderer (Graphics: Vulkan)

The engine is a static glibc program; Android's Vulkan driver is a bionic library that only an app
process can load. So the app draws, and the engine hands it macOS's GPU commands:

    engine (hengevm, --gpu vulkan)                    app process (libhengegpu.so)
    pvg_lite.c: the guest driver's protocol
    pvg_android.c: link mode ──── gpu.link ──────▶   replay.c: commands, in order
      task roots, surface table, display page          objects.c: buffers, textures, surfaces over guest RAM
      waits for "done" before completing a stamp       screen_vk.c: the swapped-to surface, letterboxed, on Vulkan
    guest RAM: disk.img.ram (HV_RAM_KEEP) ◀── mmap ──  shaders.c: each .metallib function → air2spv → SPIR-V
                                                        (libair2spv.so, run on the tablet, cached in files/spirv)

- `link.h`: the shared file: a header (guest RAM file, task page-table roots, surface table, display
  page) and a 16 MB ring of commands. The engine completes a command only when the app has done it,
  or at once while no app is attached (its heartbeat is over two seconds old), so macOS never stalls
  on a closed screen.
- `replay/`: the app side. `selftest.sh` runs it on Linux (any Vulkan driver; CI uses lavapipe)
  against a hand-made guest: a render-pass clear, a buffer-to-texture blit, a swap, and the same
  exec through the ring.

## What it does today

Everything runs on the CPU, straight in guest memory, so the guest and the screen see the same bytes:

- render passes: attachments whose load action is Clear are cleared (common colour formats);
- blits: buffer↔texture, texture→texture, buffer→buffer, fill;
- the guest's object queries (tags 450–469) get consistent answers;
- swaps show the surface on screen, with the pointer drawn over it;
- every shader library macOS loads is translated on the tablet by air2spv, in the background.

Draws and compute dispatches are counted and skipped. They need, in order:

1. a reader for Metal's serialized pipelines (`pipe-*.pipe*` in a `--gpu capture`), giving each
   pipeline's functions, vertex layout, blend state and attachment formats;
2. Vulkan pipelines from those plus air2spv's SPIR-V (layout in `../air2spv/ABI.md`);
3. moving the passes that draw onto the GPU, with guest memory synchronised around them.

The log (`adb logcat -s henge-gpu`) prints counts every ten seconds: execs, passes, clears, blits,
draws skipped, swaps, frames, and shaders translated / refused.
