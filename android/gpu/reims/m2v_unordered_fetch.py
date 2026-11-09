#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's framebuffer fetch on a device with neither ordered form (Qualcomm's Adreno
driver): input attachments without the rasterization-order flags, best effort. Run by
android_port.py on the metal2vulkan checkout.

Metal's programmable blending (a fragment function reading its own color attachment) is exact in
metal2vulkan through one of two forms: input attachments under VK_EXT_rasterization_order_attachment_access,
or storage images in VK_EXT_fragment_shader_interlock. Qualcomm's driver on the tablet exposes neither,
and the compile layer refuses the pipeline: WindowServer's `inplace_copy_lph` ended the app
("newRenderPipelineStateWithDescriptor: ... Unsupported { feature: FramebufferFetch, needs:
AnyOf([RasterizationOrderColorAttachmentAccess], [FragmentShaderPixelInterlock]) }", 2026-11-09),
and macOS-uke found the same on the same GPU: without framebuffer fetch, WindowServer's rounded
corners, blur and backgrounds are black or refused.

On a tile-based GPU an input attachment reads the tile's memory, and the fragments of one pixel in
one subpass are shaded in order in practice; the rasterization-order extension exists to promise
what such hardware already does (ARM wrote it for exactly this). This patch takes that bet on
Android: the compile layer emits the input-attachment form as if the device had the feature, and
the runtime leaves out the two flags that need the extension where the device did not enable it
(the subpass's and the pipeline's; setting them without the extension is invalid). Where the bet
is wrong the picture is wrong, never the process: an unordered read sees a stale pixel.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_unordered_fetch.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    # The profile: the device "has" rasterization-order color access on Android where it has no
    # ordered form at all, so the compile layer emits input-attachment fetches.
    'crates/metal2vulkan/src/device/derive.rs': [
        ('''        rasterization_order_color_access: has(VkFeature::RasterizationOrderColorAttachmentAccess),
''', '''        // Henge's Android port: input-attachment fetches without the order promise, where the device
        // has neither ordered form (m2v_unordered_fetch.py).
        rasterization_order_color_access: has(VkFeature::RasterizationOrderColorAttachmentAccess)
            || (cfg!(target_os = "android") && !has(VkFeature::FragmentShaderPixelInterlock)),
'''),
    ],
    # The pipeline's requirements name the feature only where the device enabled it: the check
    # against the enable set would refuse the pipeline otherwise.
    'crates/metal2vulkan/src/pipeline/compile/api.rs': [
        ('''    // Ordered framebuffer fetch: the subpass and pipeline flags need the feature.
    if graphics.rasterization_order_attachment_access {
        requirements
            .features
            .insert(VkFeature::RasterizationOrderColorAttachmentAccess);
    }
''', '''    // Ordered framebuffer fetch: the subpass and pipeline flags need the feature. Henge's Android
    // port: only where the device enabled it (else the fetch is unordered, m2v_unordered_fetch.py).
    if graphics.rasterization_order_attachment_access
        && enabled
            .features
            .contains(&VkFeature::RasterizationOrderColorAttachmentAccess)
    {
        requirements
            .features
            .insert(VkFeature::RasterizationOrderColorAttachmentAccess);
    }
'''),
    ],
    # The runtime's two flags, only where the extension is enabled.
    'crates/metal2vulkan-runtime/src/pipeline/graphics.rs': [
        ('''        let blend_flags = match key.pass.fetch_inputs {
            true => {
                vk::PipelineColorBlendStateCreateFlags::RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXT
            }
            false => vk::PipelineColorBlendStateCreateFlags::empty(),
        };
''', '''        // Henge's Android port: the flag needs the extension; without it the fetch is unordered
        // (m2v_unordered_fetch.py).
        let orders = self.device.enabled().extensions.contains(
            &metal2vulkan::device::VkExtension::ExtRasterizationOrderAttachmentAccess,
        );
        let blend_flags = match key.pass.fetch_inputs && orders {
            true => {
                vk::PipelineColorBlendStateCreateFlags::RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXT
            }
            false => vk::PipelineColorBlendStateCreateFlags::empty(),
        };
'''),
    ],
    'crates/metal2vulkan-runtime/src/render_target.rs': [
        ('''    if shape.fetch_inputs {
        draws = draws
            .input_attachments(&inputs)
            .flags(vk::SubpassDescriptionFlags::RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_EXT);
    }
''', '''    if shape.fetch_inputs {
        draws = draws.input_attachments(&inputs);
        // Henge's Android port: the flag needs the extension; without it the fetch is unordered
        // (m2v_unordered_fetch.py).
        if device.enabled().extensions.contains(
            &metal2vulkan::device::VkExtension::ExtRasterizationOrderAttachmentAccess,
        ) {
            draws = draws
                .flags(vk::SubpassDescriptionFlags::RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_EXT);
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
                sys.exit(f'm2v_unordered_fetch: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_unordered_fetch: {changed} edits applied')


if __name__ == '__main__':
    main()
