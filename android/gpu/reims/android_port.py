#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ports reims-vgpu and metal2vulkan (checked out at the commits build.sh pins) to Android.

Rust calls Android target_os = "android", not "linux", though its kernel, libc calls and Vulkan
loader are Linux's. So every Linux-only cfg of the library and the runtime also takes Android,
except what Android has no use for here: SDL3's window (the app draws through a frame target; the
window crate gets a stub, window_android.rs) and the video devices' VA-API and NVIDIA backends.

usage: android_port.py REIMS_DIR METAL2VULKAN_DIR"""
import pathlib
import re
import sys

LINUX = 'target_os = "linux"'
BOTH = 'any(target_os = "linux", target_os = "android")'
HERE = pathlib.Path(__file__).resolve().parent


def widen(path: pathlib.Path) -> int:
    text = path.read_text()
    if LINUX not in text or BOTH in text:
        return 0
    n = text.count(LINUX)
    path.write_text(text.replace(LINUX, BOTH))
    return n


def main() -> None:
    reims, m2v = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    total = 0
    # Every source of the runtime and of the library's core, Vulkan platform, API and C API...
    roots = [m2v / 'crates' / c for c in ('metal2vulkan', 'metal2vulkan-runtime', 'metal2vulkan-air')]
    roots += [reims / 'crates' / c for c in ('reims-vgpu-core', 'reims-vgpu-vulkan', 'reims-vgpu', 'reims-vgpu-c')]
    for root in roots:
        for f in root.rglob('*.rs'):
            # ...but the video devices' backend choice, whose Linux backends Android does not build.
            if 'video' in f.parts:
                continue
            total += widen(f)
    # Linux-only dependencies the runtime and the Vulkan platform need on Android too.
    for toml in [m2v / 'crates' / 'metal2vulkan-runtime' / 'Cargo.toml', m2v / 'crates' / 'metal2vulkan' / 'Cargo.toml',
                 reims / 'crates' / 'reims-vgpu-vulkan' / 'Cargo.toml']:
        if toml.exists():
            total += widen(toml)

    # The window crate: the Vulkan platform on Android too (its clock), without SDL3, and the stub.
    win = reims / 'crates' / 'reims-vgpu-window'
    toml = win / 'Cargo.toml'
    t = toml.read_text()
    if "target_os = \"android\"" not in t:
        t += '\n# Henge\'s Android port: no window, so no SDL3; the Vulkan platform\'s clock.\n' \
             '[target.\'cfg(target_os = "android")\'.dependencies]\nreims-vgpu-vulkan.workspace = true\n'
        toml.write_text(t)
    native = win / 'src' / 'native.rs'
    t = native.read_text()
    if 'mod android' not in t:
        t += ('\n#[cfg(target_os = "android")]\nmod android;\n#[cfg(target_os = "android")]\npub use android::FrameSink;\n'
              '#[cfg(target_os = "android")]\npub(crate) use android::{Surface, close, cursor_changed, guest_mode_changed, host_screens, now, open};\n')
        native.write_text(t)
    (win / 'src' / 'native' / 'android.rs').write_text((HERE / 'window_android.rs').read_text())
    # The window's frame sink on the Vulkan platform implements the stub's trait as Linux's.
    sink = reims / 'crates' / 'reims-vgpu' / 'src' / 'window' / 'vulkan.rs'
    total += widen(sink)
    # The shader cache ($HOME/.cache/reims-vgpu-vulkan, HOME being the folder the app gives host.c) may hold
    # 25 GiB by default, on a tablet whose storage the Mac's disk already fills: 4 GiB here. The whole
    # constant is matched so a changed upstream stops the build instead of silently keeping 25 GiB.
    platform = reims / 'crates' / 'reims-vgpu-vulkan' / 'src' / 'platform.rs'
    t = platform.read_text()
    old = 'const SHADER_CACHE_CAPACITY: u64 = rt::DEFAULT_CAPACITY;'
    new = 'const SHADER_CACHE_CAPACITY: u64 = 4 << 30; // Henge\'s Android port: 4 GiB, not 25'
    if new not in t:
        if t.count(old) != 1:
            sys.exit(f'android_port: {platform}: SHADER_CACHE_CAPACITY not found once; upstream changed')
        platform.write_text(t.replace(old, new))
    # No window on Android, so none of the window system's extensions. The probe for them opens the
    # system's Vulkan loader, which lists VK_KHR_surface and the rest; asked of a driver opened without
    # that loader (Turnip, through its hardware module), which has none of them, the instance was refused:
    # "MTLCreateSystemDefaultDevice(): the Vulkan runtime refused it: Vulkan failure -7"
    # (VK_ERROR_EXTENSION_NOT_PRESENT), the app's death at the first start on Turnip (the tablet, 2026-11-10).
    t = platform.read_text()
    old = '        crate::os::add_window_extensions(&mut options);\n'
    new = ('        // Henge\'s Android port: no window, so no window-system extensions (android_port.py).\n'
           '        #[cfg(not(target_os = "android"))]\n'
           '        crate::os::add_window_extensions(&mut options);\n')
    if new not in t:
        if t.count(old) != 1:
            sys.exit(f'android_port: {platform}: add_window_extensions not called once; upstream changed')
        platform.write_text(t.replace(old, new))
    # The IOSurface mapper's ring: the library maps ring size x 16 bytes and dies unless that is whole
    # 16 KiB pages (docs/testing.md 6.23). On the tablet macOS wrote a size that is not
    # ("reims-vgpu: fatal: memory.map.mapping_length_unaligned", the app's first death past device
    # creation). The mapping is of guest RAM, so it is rounded up to whole pages: the ring's own index
    # math keeps the guest's size, and a mapping past the end of RAM is refused, not fatal, as before.
    regs = reims / 'crates' / 'reims-vgpu-core' / 'src' / 'iosurface' / 'regs.rs'
    t = regs.read_text()
    anchor = '        let (length, overflow) = ring_size.overflowing_mul(RING_COMMAND_SIZE);\n'
    rounding = (
        '        // Henge\'s Android port: whole guest pages, not host death, for a ring of another size.\n'
        '        let length = {\n'
        '            let page = crate::guest::records::APV_PAGE_SIZE;\n'
        '            let rounded = length.div_ceil(page).saturating_mul(page);\n'
        '            if rounded != length {\n'
        '                log::warn!(\n'
        '                    "ring size {ring_size} ({length} bytes) is not whole guest pages; mapping {rounded} bytes"\n'
        '                );\n'
        '            }\n'
        '            rounded\n'
        '        };\n'
    )
    if rounding not in t:
        if t.count(anchor) != 1:
            sys.exit(f'android_port: {regs}: the ring-size write not found once; upstream changed')
        # After the overflow check that follows the anchor: its `return;` and closing brace.
        at = t.index(anchor)
        end = t.index('            return;\n        }\n', at) + len('            return;\n        }\n')
        t = t[:end] + rounding + t[end:]
        regs.write_text(t)
    # The IOSurface mapper device sizes its descriptor table, its page mappings and the features it
    # offers by the HOST's page size (sysconf), which equals the guest's on every host the library
    # supports (Apple silicon: 16 KiB both; x86-64 Linux: 4 KiB both). Android arm64 is 4 KiB host
    # pages under a 16 KiB-page macOS guest: the 4 KiB table mapping died in the memory map
    # ("memory.map.mapping_length_unaligned", the tablet, 2026-11-09, right after the mapper wrote its
    # descriptor table base), and the guest was offered v1 descriptors. The device's pages are the
    # guest's: APV_PAGE_SIZE and APV_PAGE_SHIFT, whole host pages either way.
    device = reims / 'crates' / 'reims-vgpu-core' / 'src' / 'iosurface' / 'device.rs'
    descriptor = reims / 'crates' / 'reims-vgpu-core' / 'src' / 'iosurface' / 'descriptor.rs'
    for path, edits in [
        (device, [
            ('        let page = platform.vm().page_size();\n        let table_len = page /',
             '        let page = crate::guest::records::APV_PAGE_SIZE; // Henge: the guest\'s page\n        let table_len = page /'),
            ('    pub(super) fn page_size(&self) -> u64 {\n        self.platform.vm().page_size()\n    }',
             '    pub(super) fn page_size(&self) -> u64 {\n        crate::guest::records::APV_PAGE_SIZE // Henge\'s Android port: the guest\'s page, not the host\'s\n    }'),
            ('    pub(super) fn page_shift(&self) -> u32 {\n        self.platform.vm().page_shift()\n    }',
             '    pub(super) fn page_shift(&self) -> u32 {\n        crate::guest::records::APV_PAGE_SHIFT // Henge\'s Android port: the guest\'s page, not the host\'s\n    }'),
        ]),
        (descriptor, [
            ('    let page = p.vm().page_size();\n    spec.client_address_ranges',
             '    let _ = p; // Henge\'s Android port: the guest\'s page, not the host\'s\n    let page = crate::guest::records::APV_PAGE_SIZE;\n    spec.client_address_ranges'),
        ]),
    ]:
        t = path.read_text()
        for old, new in edits:
            if new in t:
                continue
            if t.count(old) != 1:
                sys.exit(f'android_port: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            t = t.replace(old, new)
        path.write_text(t)
    # metal2vulkan on a device without VK_EXT_external_memory_host (Qualcomm's driver): host memory as
    # copies, not a refusal that ends the app at the first IOSurface texture (m2v_no_host_import.py).
    import subprocess
    subprocess.run([sys.executable, '-I', str(HERE / 'm2v_no_host_import.py'), str(m2v)], check=True)
    # Framebuffer fetch on a device with neither ordered form (Qualcomm): input attachments without the
    # order flags, best effort (m2v_unordered_fetch.py), instead of refusing WindowServer's pipelines.
    subprocess.run([sys.executable, '-I', str(HERE / 'm2v_unordered_fetch.py'), str(m2v)], check=True)
    # reims-vgpu: what this device lacks is an error the guest sees (Metal's NSError, nil), not the
    # app's death (reims_lenient.py).
    subprocess.run([sys.executable, '-I', str(HERE / 'reims_lenient.py'), str(reims)], check=True)
    # A Vulkan driver of the app's own (Turnip), opened directly as the ICD (m2v_icd_library.py).
    subprocess.run([sys.executable, '-I', str(HERE / 'm2v_icd_library.py'), str(m2v)], check=True)
    print(f'android_port: {total} Linux-only places now take Android')


if __name__ == '__main__':
    main()
