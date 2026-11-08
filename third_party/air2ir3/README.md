# air2ir3 front end (vendored)

The AIR → NIR front end of [air2ir3](https://github.com/Snapintosh/air2ir3), the shader
compiler of the Snapintosh project, at commit `55865cc84c1523d48fa469fff693c2164bf9c1b9`.
MIT ([LICENSE](LICENSE)).

Only the front end is kept: `src/air2ir3/*.cpp` (parse, specialise function constants,
translate one AIR function to NIR), `src/air2ir3.h`, and the constexpr-sampler decoder
`src/a8xx_sampler.*`. air2ir3's ir3 (Adreno ISA) back end, CLI and tests are not
vendored. `android/air2spv/` compiles these files into one translation unit, in the
order air2ir3's own `src/air2ir3.cpp` includes them, and adds a SPIR-V back end.

`BINDING-ABI.md` is air2ir3's Adreno binding ABI, kept because the front end's NIR
follows it (UBO n, driver table, T#/S# indices); `android/air2spv/ABI.md` says how
air2spv maps it to Vulkan.

Files here are unmodified copies. To update: copy the same files from a newer
air2ir3 commit and change the hash above.
