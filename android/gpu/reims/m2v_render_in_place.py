#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's textures over the Mac's pages that the GPU writes (the Mac's framebuffer): in place,
where the app asks for it (M2V_RENDER_IN_PLACE=1, Settings > Framebuffer in place). An experiment in
0.3.53, off unless asked; the app asks by default since 0.3.54 (the tablet's run is at the end). Run
by android_port.py on the metal2vulkan checkout, after m2v_dma_buf_checks.py.

What the tablet's log says at every start (build 57, Henge's Turnip, the Mac's pages imported as
dma-bufs): "a 2D RGBA16Float texture of 1280x800 over a buffer at offset 0 with rows of 10240 bytes:
... its usage renders into it or writes it ... its texels are copied into an image before each use and
back after each write". That texture is the Mac's framebuffer. The runtime takes a texture over host
pages in place (a linear image bound to a dma-buf of its own pages) only where its declared usage
samples or reads it (`in_place.rs`); one the GPU renders into or writes is a shadow: an image of the
runtime's own, the pages' texels copied into it before a use, kept aside before a write and merged back
after it (`shadow.rs`, `shadow_merge.rs`). All of that runs on the GPU, so it costs the Mac's CPUs
nothing; it costs the GPU a few copies of the 8 MB framebuffer at every frame.

Upstream's reason is Vulkan's letter, not something a driver was seen doing: an image bound to
imported memory starts UNDEFINED, and "its layout transitions and a render pass's load and store
operations [are] free to read and write every byte bound to it", where Metal keeps every byte no
command writes. Two things narrow that here:

  - The first move of such an image is what the runtime's own check measures on the device
    (`in_place_check.rs`, per driver and release, class `DmaBuf`), and its verdict is asked as for any
    texture in place: a device that failed it keeps the shadow.
  - This takes only a texture whose rows hold nothing but texels (the row pitch is the row's bytes, as
    the framebuffer's 1280 x 8 = 10240): a render pass's store or a shader's write then has no byte
    beside a texel to touch inside the rows. Bytes after the last row in its last page are the one
    place left, and a driver writes no row past an image's height.

So, with M2V_RENDER_IN_PLACE=1, a texture over pages imported as dma-bufs whose usage has
RenderTarget or ShaderWrite (and not ShaderAtomic), with such rows, is asked for in place like one that
is only read: the same image (`Device::dma_buf_in_place`, with the texture's own usage, so with the
attachment and storage usage it needs, as a no-copy heap's texture over dma-bufs already is), the same
verdict, and the same refusals, each of which falls back to the shadow with the runtime's notice
saying why. Where the check's verdict is not known yet it is waited for, three seconds at most, once:
the framebuffer is made early in a boot, and a texture made before the verdict stays a shadow for its
life.

Never run on a device before it shipped (no /dev/udmabuf off the tablet). The log says, eight times at
most, what each such texture's usage was and whether it was asked for in place.

The tablet, 2026-11-13 (build 58, two runs of seven minutes, the second with this on): the framebuffer
(Metal usage 0x5: ShaderRead and RenderTarget) "is an image in place: the GPU renders into the Mac's
pages"; no refusal, no crash; the user found the second run the better one. A frame's drawing took 3.4
ms against 4.0 in the run without (the two runs drew different things, so the figure is a pointer, not
a measurement), and the notices "a render pass of 2 color attachments in 2 instances: its draws keep
different attachments out of their subpass" and its 8 MB scratch image did not appear: that split was
the shadow's. Two small RG8 and 40-row textures whose rows hold bytes between the texels stayed shadows,
as meant. So the app asks for it wherever the Mac's memory is used in place, and turns it off by itself
after a crash of the GPU's side (VmService), or where Settings says Off.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_render_in_place.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/in_place.rs': [
        ('''/// Whether a texture over host pages of `class`, of Metal `usage` (as Metal reports it), on a
/// device of `policy`, is tried as an image in place; else why it is a shadow (the module docs).
''', '''/// Henge's Android port (m2v_render_in_place.py): whether the process asked for the textures the
/// GPU writes over dma-buf pages in place too (`M2V_RENDER_IN_PLACE=1`), read once.
pub(crate) fn henge_render_in_place() -> bool {
    static ASKED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ASKED.get_or_init(|| std::env::var_os("M2V_RENDER_IN_PLACE").is_some_and(|v| v == "1"))
}

/// Henge's Android port (m2v_render_in_place.py): a line in the log for a texture the GPU writes
/// over dma-buf pages, the first eight.
pub(crate) fn henge_note_written(line: std::fmt::Arguments<'_>) {
    static SAID: AtomicU8 = AtomicU8::new(0);
    if SAID.fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| (n < 8).then_some(n + 1)).is_ok() {
        eprintln!("metal2vulkan: (Henge) {line}");
    }
}

/// Whether a texture over host pages of `class`, of Metal `usage` (as Metal reports it), on a
/// device of `policy`, is tried as an image in place; else why it is a shadow (the module docs).
'''),
    ],
    'crates/metal2vulkan-runtime/src/texture.rs': [
        ('''                    match in_place::in_place(policy, usage, class, verdict) {
                        Ok(()) => {
                            match self.in_place_image(&plan, descriptor, &texels, class, &facts)? {
                                Ok((image, pitched)) => {
                                    if !checking {
                                        self.shared().counts.host_texture_in_place(class, pitched);
                                    }
''', '''                    // Henge's Android port (m2v_render_in_place.py): a texture the GPU renders
                    // into or writes, over pages imported as dma-bufs, whose rows hold texels alone,
                    // is asked for in place as one that is only read, where the process asked
                    // (`M2V_RENDER_IN_PLACE=1`): the usage `in_place` is asked about leaves the two
                    // writes out, and the image keeps the texture's own plan. The verdict of the
                    // device's check is waited for where it is not known yet.
                    let henge_writes = metal2vulkan::metal::TextureUsage::RENDER_TARGET
                        .union(metal2vulkan::metal::TextureUsage::SHADER_WRITE);
                    let henge_written =
                        !checking && class == PagesClass::DmaBuf && usage.intersects(henge_writes);
                    let henge_tight =
                        texels.layout.rows <= 1 || texels.layout.row_pitch == texels.layout.row_bytes;
                    let henge_takes = henge_written
                        && in_place::henge_render_in_place()
                        && henge_tight
                        && !usage.contains(metal2vulkan::metal::TextureUsage::SHADER_ATOMIC);
                    let henge_asked = match henge_takes {
                        true => usage.difference(henge_writes),
                        false => usage,
                    };
                    if henge_takes && self.in_place_verdict(class) == Verdict::Pending {
                        self.settle_in_place_checks(std::time::Duration::from_secs(3));
                    }
                    if henge_written {
                        in_place::henge_note_written(format_args!(
                            "a texture the GPU writes over the Mac's pages: Metal usage {:#x}, {} \\
                             rows of {} bytes, {} bytes apart; in place: {}",
                            usage.as_raw(),
                            texels.layout.rows,
                            texels.layout.row_bytes,
                            texels.layout.row_pitch,
                            match (in_place::henge_render_in_place(), henge_takes) {
                                (false, _) => "not asked for (M2V_RENDER_IN_PLACE is not 1): a shadow",
                                (true, true) => "asked for; a notice follows where the device makes \\
                                                 no such image and it is a shadow after all",
                                (true, false) => "asked for, but its rows hold bytes that are not \\
                                                  texels, or it takes atomic writes: a shadow",
                            }
                        ));
                    }
                    match in_place::in_place(policy, henge_asked, class, verdict) {
                        Ok(()) => {
                            match self.in_place_image(&plan, descriptor, &texels, class, &facts)? {
                                Ok((image, pitched)) => {
                                    if !checking {
                                        self.shared().counts.host_texture_in_place(class, pitched);
                                    }
                                    if henge_takes {
                                        in_place::henge_note_written(format_args!(
                                            "that texture is an image in place: the GPU renders \\
                                             into the Mac's pages"
                                        ));
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
                sys.exit(f'm2v_render_in_place: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_render_in_place: {changed} edits applied')


if __name__ == '__main__':
    main()
