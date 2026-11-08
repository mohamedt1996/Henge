# air2spv: Metal shaders to SPIR-V

`air2spv` translates one Metal function (its AIR bitcode, as macOS hands it to the paravirtual
GPU) to SPIR-V for the Android Vulkan replay ([../VULKAN.md](../VULKAN.md)). It is the general
translator that VULKAN.md lists as step 2.

    AIR bitcode ──air2ir3 front end──▶ NIR ──Vulkan lowering (air2spv.cpp)──▶ NIR ──Mesa nir_to_spirv──▶ SPIR-V

- **Front end:** [third_party/air2ir3](../../third_party/air2ir3), from the Snapintosh project
  (MIT). It parses AIR with LLVM, specialises function constants, refuses what it can't
  reproduce exactly, and translates to Mesa NIR. Snapintosh feeds that NIR to Mesa's Adreno
  back end for its own Metal driver; here it goes to SPIR-V instead.
- **Back end:** Mesa's `nir_to_spirv` (from the zink driver, MIT), built from the Mesa checkout.
- **Between them:** the passes in `air2spv.cpp` that move the front end's resource layout
  (air2ir3's Adreno binding ABI, `third_party/air2ir3/BINDING-ABI.md`) onto the Vulkan layout
  below.

## Build and test

    android/air2spv/build.sh deps    # LLVM 22, Mesa at air2ir3's pinned commit, Mesa's NIR libraries
    android/air2spv/build.sh         # build/air2spv/air2spv and libair2spv.a
    android/air2spv/build.sh test    # tests/*.ll -> AIR -> SPIR-V, checked with spirv-val (vulkan1.2)

Translate a capture from the tablet:

    adb pull /sdcard/Android/data/dev.henge.android/files/shaders shaders
    android/air2spv/translate_capture.sh shaders out    # out/spv/*.spv, *.json, refused.txt

One function by hand: `build/air2spv/air2spv [--fc idx=v] [--fc-name n=v] --json f.json -o f.spv f.bc`.
`--nir` prints the NIR handed to `nir_to_spirv`.

The tool runs on the build machine for now: it produces SPIR-V ahead of time, keyed by function
and bitcode hash, which is the substitution table of VULKAN.md step 1 filled by a compiler rather
than by hand. `libair2spv.a` and `air2spv.h` are the same compiler as a library, for when the
replay compiles at pipeline creation; that needs LLVM and Mesa built with the NDK.

## Layout (ABI version 1)

SPIR-V 1.5, Vulkan 1.2 or later. Required: `bufferDeviceAddress`, `shaderInt64`; for shaders that
use them, `shaderFloat16`, `storageBuffer16BitAccess`, `shaderStorageImageWriteWithoutFormat`,
demote-to-helper (Vulkan 1.3 core).

| Metal | Vulkan |
| --- | --- |
| `constant T& x [[buffer(n)]]` | Push-constant slot n: the buffer's device address (plus the Metal offset). Loads are `PhysicalStorageBuffer` loads. |
| `device T* x [[buffer(n)]]` | The driver table: a buffer whose u64 at byte `8*n` is that buffer's address. The table's own address goes in push slot `driver_table_ubo` (= 1 + the highest constant buffer the function declares). Threadgroup offsets at byte `256 + 4*n`, as in BINDING-ABI.md 3.8. |
| constant tables (`constant` globals) | Push slot 15: address of a buffer holding the function's constant data (`<out>.spv.const`, or `constant_data` from the library). |
| `texture(n)`, sample or read | Set 0, binding n: sampled image. |
| `sampler(n)` and constexpr samplers | Set 0, binding 16 + n: sampler. A constexpr sampler's state is in the sidecar (`reflection.bindings[]` with kind `constexpr_sampler`, `state`). |
| `texture(n)`, write | Set 0, binding 32 + n: storage image, format unknown. |
| `[[color(n)]]` read (framebuffer fetch) | Set 0, binding 48 + n: input attachment, `InputAttachmentIndex` n. |
| `[[attribute(n)]]` | Vertex input `Location` n. |
| `[[user(name)]]` varyings | `Location` = rank of the name in sorted order (both stages). |
| `[[color(n)]]` output | Fragment output `Location` n. |

Push constants: 128 bytes, 16 little-endian u64 addresses, all stages. A function whose constant
buffers or driver table reach slot 15 is refused when it also has constant data.

The sidecar (`--json`) lists the descriptors (`descriptors[]`, set and binding), the push slots in
use (`push_constants.slots[]`), and air2ir3's reflection of the function (varyings, attributes,
render targets, function constants). The reflection's `slot` fields are air2ir3's Adreno slots;
the Vulkan places are the ones in `descriptors[]` and `push_constants`.

Coordinates: Metal's clip space has +y up and Vulkan's +y down, both with z in [0, 1]. The shader
is not changed; the replay flips y with a negative viewport height.

## What it refuses

Whatever air2ir3's front end refuses (its BINDING-ABI.md section 2): argument buffers, visible
function tables, imageblocks, some XR pixel formats, and a variant in which an unset function
constant decides anything. Pipelines set their function constants, so translate with the
pipeline's values (`--fc`) where the capture records them.
