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

Guest memory stays the one truth: everything is done in it, on the CPU or copied through the GPU.

- render passes: clears on the CPU; a pass that **draws** runs on the GPU (`draw.c`, Vulkan 1.3 with
  dynamic rendering): its attachments are uploaded (or cleared), each draw's buffers copied into one
  host-visible arena and passed to the shaders as device addresses, its textures uploaded and
  sampled with the pipeline's samplers, and what the pass stores read back into guest memory;
- the functions a draw uses are translated on the tablet by air2spv when first needed (or found in
  the cache), with depth/stencil state, viewport, scissor, culling and the Metal draw calls
  (indexed, instanced) mapped across; Metal's +y-up clip space by a flipped viewport;
- blits: buffer↔texture, texture→texture, buffer→buffer, fill;
- the guest's object queries (tags 450–469) get consistent answers;
- swaps show the surface on screen, with the pointer drawn over it.

`replay/drawtest.c` checks a draw end to end: two real AIR functions (air2spv's tests) in .metallib
containers, a pipeline naming them, vertex and constant buffers, a texture and a sampler, a textured
quad drawn into a cleared surface the right way up.

### Guessed, until the pipeline description is decoded

How Metal serializes a render pipeline (the `pipe-*.pipe*` records in a `--gpu capture`) is not
published by anyone. Until it is decoded:

- the vertex and fragment functions are found by scanning the description for refs to function
  objects (as [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) does); the log
  says when it holds more than two;
- blending is premultiplied source-over for every colour attachment;
- vertex attributes (`[[stage_in]]`) are read packed, in attribute order, from the lowest bound
  vertex buffer the shader does not take as an argument.

### Not done

Compute dispatches; draws that read the framebuffer (fb fetch) or write textures; tessellation and
indirect draws; multisampling (a pass draws straight into its resolve texture). Depth and stencil
contents last for one pass. Everything is copied each pass: keeping buffers and textures resident
on the GPU is the next step for speed.

The log (`adb logcat -s henge-gpu`) prints counts every ten seconds (passes on the GPU, draws drawn
and skipped, pipelines, shaders translated / refused) and each new reason a draw was skipped.
