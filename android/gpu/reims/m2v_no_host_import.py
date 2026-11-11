#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan on a Vulkan device without VK_EXT_external_memory_host (Qualcomm's Adreno driver):
host memory as copies, not a refusal. Run by android_port.py on the metal2vulkan checkout.

The runtime makes buffers and textures over the guest's pages ("no-copy" buffers, IOSurface
textures) by importing the pages as host memory through VK_EXT_external_memory_host. Qualcomm's
driver does not expose it, and the runtime's Importer refused to exist without it, so the first
IOSurface texture ended the app: "newTextureWithDescriptor:iosurface:plane:: ... Unsupported {
feature: HostMemory, needs: VK_EXT_external_memory_host }" (the tablet, 2026-11-09).

The runtime already has a second way to back such objects: a copy of the pages in device-visible
memory, compared with the pages on the CPU at each commit and completion (`copied.rs`, announced
`copied-host-pages`), taken where the host's import gate refuses the import or the import fails.
This patch gives a device without the extension an Importer that imports nothing (`absent`):

  - Importer::new returns such an importer instead of the refusal, with the host page as its
    alignment, and with the dma-buf route where the device has its extensions (0.3.37: Turnip
    does, and with /dev/udmabuf reims-vgpu gives the pages a source of dma-bufs);
  - import_backing, the one place the pages' pointer reaches the driver, answers the runtime's own
    `unpinnable` refusal first, which every caller already turns into dma-bufs of the pages where
    the device imports them and the pages come with a source of them, and into a copy otherwise;
  - memory_types never calls the (unloaded) extension function;
  - fresh_page_buffer and use_host_ram, which need real imports, decline on it.

Each edit must match exactly once, so a changed upstream stops the build instead of silently
building a runtime that still refuses.

usage: m2v_no_host_import.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/host_memory.rs': [
        # The field.
        ('''    /// `minImportedHostPointerAlignment`.
    pub(super) alignment: u64,
''', '''    /// `minImportedHostPointerAlignment`.
    pub(super) alignment: u64,
    /// Henge's Android port: the device has no VK_EXT_external_memory_host, so this importer
    /// imports nothing; every no-copy object is a copy of its pages (`copied.rs`).
    pub(super) absent: bool,
'''),
        # The refusal becomes an absent importer.
        ('''        if !enabled
            .extensions
            .contains(&VkExtension::ExtExternalMemoryHost)
        {
            return Err(Unsupported::needs(
                Feature::HostMemory,
                metal2vulkan::error::Capability::Extension(VkExtension::ExtExternalMemoryHost),
            ));
        }
        let page = host_page().ok_or_else(|| Unsupported::not_implemented(Feature::HostMemory))?;
''', '''        let page = host_page().ok_or_else(|| Unsupported::not_implemented(Feature::HostMemory))?;
        if !enabled
            .extensions
            .contains(&VkExtension::ExtExternalMemoryHost)
        {
            // Henge's Android port: no import of the pointer, so a no-copy object is a copy of its
            // pages, or, where the device imports dma-bufs (Turnip) and its pages come with a
            // dma-buf source (reims-vgpu's, of /dev/udmabuf), those dma-bufs: the pages in place.
            let handle = device.handle();
            let load = |name: &std::ffi::CStr| {
                // SAFETY: as below: the device is live and the extensions are enabled.
                let function = unsafe {
                    loader_word.restore();
                    instance.get_device_proc_addr(handle, name.as_ptr())
                };
                function.map_or(std::ptr::null(), |function| function as *const c_void)
            };
            let dma_buf = dma_buf.then(|| ash::khr::external_memory_fd::DeviceFn::load(load));
            let dma_buf_images = dma_buf.is_some() && dma_buf_images;
            eprintln!(
                "metal2vulkan: no VK_EXT_external_memory_host; dma-bufs of host pages: {}, images over them: {}",
                if dma_buf.is_some() { "imported" } else { "not offered by this driver (copies)" },
                dma_buf_images,
            );
            return Ok(Importer {
                functions: ash::ext::external_memory_host::DeviceFn::load(|_| std::ptr::null()),
                page,
                alignment: page,
                absent: true,
                dma_buf,
                dma_buf_images,
                sparse_granule: std::sync::OnceLock::new(),
            });
        }
'''),
        ('''        Ok(Importer {
            functions,
            page,
            alignment,
            dma_buf,
''', '''        Ok(Importer {
            functions,
            page,
            alignment,
            absent: false,
            dma_buf,
'''),
        # The extension's function is never called on an absent importer.
        ('''    ) -> Result<u32, vk::Result> {
        let mut properties = vk::MemoryHostPointerPropertiesEXT::default();
''', '''    ) -> Result<u32, vk::Result> {
        if self.absent {
            return Err(vk::Result::ERROR_INVALID_EXTERNAL_HANDLE); // Henge: nothing imports
        }
        let mut properties = vk::MemoryHostPointerPropertiesEXT::default();
'''),
    ],
    'crates/metal2vulkan-runtime/src/buffer_descriptor.rs': [
        ('''        self.refuse_unpinnable(imported, window.size)?;
''', '''        // Henge's Android port: a device that imports no host memory refuses every pointer as it
        // refuses unpinnable pages, which each caller turns into a copy of the pages.
        if importer.absent {
            return Err(unpinnable());
        }
        self.refuse_unpinnable(imported, window.size)?;
'''),
    ],
    'crates/metal2vulkan-runtime/src/fresh_pages.rs': [
        ('''        let Ok(importer) = &shared.host_import else {
            return Ok(None);
        };
''', '''        let Ok(importer) = &shared.host_import else {
            return Ok(None);
        };
        if importer.absent {
            return Ok(None); // Henge's Android port: no host import to put fresh pages in
        }
'''),
    ],
    'crates/metal2vulkan-runtime/src/host_ram.rs': [
        ('''        if let Err(unsupported) = &shared.host_import {
            return Err(Error::Unsupported(unsupported.clone()));
        }
''', '''        if let Err(unsupported) = &shared.host_import {
            return Err(Error::Unsupported(unsupported.clone()));
        }
        if shared.host_import.as_ref().is_ok_and(|importer| importer.absent) {
            // Henge's Android port: the RAM's chunks are imports, which this device makes none of.
            return Err(Error::Unsupported(Unsupported::not_representable(Feature::HostMemory)));
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
                sys.exit(f'm2v_no_host_import: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_no_host_import: {changed} edits applied')


if __name__ == '__main__':
    main()
