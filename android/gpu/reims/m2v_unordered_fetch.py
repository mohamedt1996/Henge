#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's framebuffer fetch and raster order groups on a device with neither ordered form
(Qualcomm's Adreno driver): the storage form, without the interlock, best effort. Run by
android_port.py on the metal2vulkan checkout.

Metal's programmable blending (a fragment function reading its own colour attachment) and raster
order groups are exact in metal2vulkan through VK_EXT_rasterization_order_attachment_access (the
inputs form: the attachment read as an input attachment of its own subpass) or
VK_EXT_fragment_shader_interlock (the storage form: the attachment left out of the subpass, read
and written as a storage image inside the interlock, a barrier between draws). Qualcomm's driver
on the tablet exposes neither. Run 5 refused WindowServer's `inplace_copy_lph` pipeline; run 6,
with the inputs form forced, segfaulted inside vulkan.adreno.so at the commit of that pass (an
attachment read as an input while written as a colour in one subpass, legal with the declared
self-dependency, and a driver bug); run 7 refused a raster order group.

So the Android port takes the storage form: the profile claims the pixel interlock where the
device has neither feature, which routes the fetch through storage images (the path RADV, ANV and
NVIDIA take), and the two places that emit the interlock instructions answer by the ENABLED
features, which stay honest, so no critical section is emitted on a device without the extension.
Within one draw, overlapping fragments are then unordered: a tiler shades one pixel's fragments in
order in practice, and where that is wrong the picture is wrong, never the process. The inputs
form's edits stay, each conditional on the real extension, for a device that has it (Turnip).

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_unordered_fetch.py METAL2VULKAN_DIR"""
import pathlib
import sys


EDITS = {
    # The profile: on Android a device with neither ordered form "has" the pixel interlock, so the
    # compile layer and the runtime take the storage form (`interlock_orders`, `through_storage`).
    'crates/metal2vulkan/src/device/derive.rs': [
        ("""        pixel_interlock: has(VkFeature::FragmentShaderPixelInterlock),
""", """        // Henge's Android port: the storage form of the fetch without the interlock's order
        // promise, where the device has neither ordered form (m2v_unordered_fetch.py).
        pixel_interlock: has(VkFeature::FragmentShaderPixelInterlock)
            || (cfg!(target_os = "android")
                && !has(VkFeature::RasterizationOrderColorAttachmentAccess)),
"""),
    ],
    # The interlock mode of a function in a raster order group, an imageblock or the storage form:
    # the instructions need the enabled feature, whatever the profile claims.
    'crates/metal2vulkan/src/lower/fragment/rog.rs': [
        ("""    let f = cx.profile.fields().fragment;
    let per_sample = sample_rate(&cx.families.stage_io.objects);
    if f.pixel_interlock {
        return Ok(ExecMode::PixelInterlockOrdered);
    }
    if f.sample_interlock && per_sample {
        return Ok(ExecMode::SampleInterlockOrdered);
    }
""", """    let f = cx.profile.fields().fragment;
    let per_sample = sample_rate(&cx.families.stage_io.objects);
    // Henge's Android port: the profile may claim the interlock for the storage form; the
    // instructions need the enabled feature (m2v_unordered_fetch.py).
    let enabled = &cx.profile.fields().enabled.features;
    if f.pixel_interlock && enabled.contains(&VkFeature::FragmentShaderPixelInterlock) {
        return Ok(ExecMode::PixelInterlockOrdered);
    }
    if f.sample_interlock
        && per_sample
        && enabled.contains(&VkFeature::FragmentShaderSampleInterlock)
    {
        return Ok(ExecMode::SampleInterlockOrdered);
    }
"""),
    ],
    # Where the mode is refused for want of the interlock, the function runs with no critical
    # section on Android: unordered. A non-fragment stage's refusal stands.
    'crates/metal2vulkan/src/lower/fragment/mod.rs': [
        ("""    let interlock =
        if rog::declared(cx.entry) || cx.families.fragment.imageblock.is_some() || storage {
            Some(rog::mode(cx)?)
        } else {
            None
        };
""", """    let interlock =
        if rog::declared(cx.entry) || cx.families.fragment.imageblock.is_some() || storage {
            // Henge's Android port: no interlock on the device, no critical section, unordered
            // (m2v_unordered_fetch.py).
            let mode = rog::mode(cx);
            let no_interlock = {
                use crate::device::requirements::VkFeature;
                let enabled = &cx.profile.fields().enabled.features;
                !enabled.contains(&VkFeature::FragmentShaderPixelInterlock)
                    && !enabled.contains(&VkFeature::FragmentShaderSampleInterlock)
            };
            match mode {
                Err(_) if cfg!(target_os = "android") && cx.stage == Stage::Fragment && no_interlock => {
                    None
                }
                other => Some(other?),
            }
        } else {
            None
        };
"""),
    ],
    # Snapshot reads (a draw reading a texture its own subpass renders) emit the interlock on their
    # own: refused, as upstream refuses them, unless the feature is really enabled.
    'crates/metal2vulkan/src/lower/fragment/snapshot.rs': [
        ("""    if !cx.profile.fields().fragment.pixel_interlock {
        return Err(Unsupported::needs(
            Feature::RasterOrderGroups,
""", """    // Henge's Android port: by the enabled feature, not the profile's claim (m2v_unordered_fetch.py).
    if !cx
        .profile
        .fields()
        .enabled
        .features
        .contains(&VkFeature::FragmentShaderPixelInterlock)
    {
        return Err(Unsupported::needs(
            Feature::RasterOrderGroups,
"""),
    ],
    # The inputs form, for a device that has rasterization-order access: its pipeline requirement
    # and its two flags only where the extension is really enabled (harmless otherwise).
    'crates/metal2vulkan/src/pipeline/compile/api.rs': [
        ("""    // Ordered framebuffer fetch: the subpass and pipeline flags need the feature.
    if graphics.rasterization_order_attachment_access {
        requirements
            .features
            .insert(VkFeature::RasterizationOrderColorAttachmentAccess);
    }
""", """    // Ordered framebuffer fetch: the subpass and pipeline flags need the feature. Henge's Android
    // port: only where the device enabled it (m2v_unordered_fetch.py).
    if graphics.rasterization_order_attachment_access
        && enabled
            .features
            .contains(&VkFeature::RasterizationOrderColorAttachmentAccess)
    {
        requirements
            .features
            .insert(VkFeature::RasterizationOrderColorAttachmentAccess);
    }
"""),
    ],
    'crates/metal2vulkan-runtime/src/pipeline/graphics.rs': [
        ("""        let blend_flags = match key.pass.fetch_inputs {
            true => {
                vk::PipelineColorBlendStateCreateFlags::RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXT
            }
            false => vk::PipelineColorBlendStateCreateFlags::empty(),
        };
""", """        // Henge's Android port: the flag needs the extension (m2v_unordered_fetch.py).
        let orders = self.device.enabled().extensions.contains(
            &metal2vulkan::device::VkExtension::ExtRasterizationOrderAttachmentAccess,
        );
        let blend_flags = match key.pass.fetch_inputs && orders {
            true => {
                vk::PipelineColorBlendStateCreateFlags::RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXT
            }
            false => vk::PipelineColorBlendStateCreateFlags::empty(),
        };
"""),
    ],
    'crates/metal2vulkan-runtime/src/render_target.rs': [
        ("""    if shape.fetch_inputs {
        draws = draws
            .input_attachments(&inputs)
            .flags(vk::SubpassDescriptionFlags::RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_EXT);
    }
""", """    if shape.fetch_inputs {
        draws = draws.input_attachments(&inputs);
        // Henge's Android port: the flag needs the extension (m2v_unordered_fetch.py).
        if device.enabled().extensions.contains(
            &metal2vulkan::device::VkExtension::ExtRasterizationOrderAttachmentAccess,
        ) {
            draws = draws
                .flags(vk::SubpassDescriptionFlags::RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_EXT);
        }
    }
"""),
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
