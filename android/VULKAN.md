# Drawing macOS on Android: the Vulkan path

On iOS, macOS's GPU work is replayed on Metal (`devices/pvg_metal.m`). Android has no Metal, so the
desktop needs a Vulkan replay. This is the plan and where it stands.

## Why the renderer can't live in the engine

The engine is a static glibc binary. Android's Vulkan driver is a bionic shared library, which a
static glibc process cannot load. So the renderer has to run in the app's own process (NDK, bionic),
fed by the engine:

    engine (glibc)                                app process (bionic, NDK)
    pvg_lite.c: guest driver protocol             vk_replay: decode records, run them on Vulkan
    pvg_android.c: exec walk ── shared memory ──▶  shader table: Metal function → SPIR-V
                   + guest memory (memfd)          swapchain on ScreenActivity's Surface

Guest RAM is already file-backed (`HV_RAM_FILE`), so the app can map the same pages the guest's
command buffers, buffers and textures live in; only the command records and completions need a ring.

## Shaders

macOS sends Metal libraries (`.metallib`, AIR: Apple's LLVM bitcode) and pipelines in Metal's private
serialized format. There is no AIR → SPIR-V translator. Two ways in, in order:

1. **A substitution table.** The desktop is drawn by a small set of WindowServer / Core Animation
   shaders (composite, blend, blur, color-convert). Capture them (`--gpu capture`), rank them by use
   (`tools/metallib_inventory.py`), and write each as GLSL → SPIR-V by hand, keyed by function name
   and bitcode hash. Anything not in the table is skipped (a missing effect, as pvg_metal.m already
   does for pipelines Metal refuses).
2. **A general translator**, AIR bitcode → SPIR-V: [air2spv](air2spv/ABI.md). It reuses the AIR → NIR
   front end of Snapintosh's air2ir3 (the compiler of its Adreno Metal driver) and Mesa's NIR → SPIR-V
   back end, so captured WindowServer shaders and apps' own both go through one compiler. The
   substitution table stays the fallback for anything it refuses.

## Status

- [x] `--gpu off`: no GPU device, to see whether macOS falls back to a plain framebuffer.
- [x] `--gpu capture`: every library and pipeline macOS uses, saved once, with bind counts
      (`Android/data/dev.henge.android/files/shaders`, reachable over USB).
- [x] `tools/metallib_inventory.py SHADER_DIR [--extract OUT]`: functions, kinds, bind counts, and
      each function's bitcode as `.bc` for `llvm-dis`.
- [x] Shared-memory ring from engine to app (`gpu/link.h`, `--gpu vulkan`); record decoder in C
      (`gpu/replay`): passes, blits, object queries, swaps, the pointer. See [gpu/README.md](gpu/README.md).
- [x] Vulkan device, swapchain on ScreenActivity's Surface (GpuView), buffers/textures/surfaces over guest memory.
- [x] air2spv on the tablet (static arm64, `libair2spv.so`): shaders translated as macOS loads them.
- [ ] Draws: Vulkan pipelines and the passes that draw moved onto the GPU.
- [ ] Pipeline deserializer (Metal's private format) for the fields Vulkan needs: functions, vertex
      layout, blend, formats.
- [x] `android/air2spv`: AIR → SPIR-V compiler, with a Vulkan resource layout (ABI.md) and a sidecar per
      function (descriptors, push slots, varyings). Builds on Linux; its tests pass `spirv-val`.
- [ ] Run `air2spv/translate_capture.sh` on a real `--gpu capture` and fix what it refuses.
- [ ] Hand-written SPIR-V only for desktop shaders air2spv refuses.
