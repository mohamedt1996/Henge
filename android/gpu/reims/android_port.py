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
    print(f'android_port: {total} Linux-only places now take Android')


if __name__ == '__main__':
    main()
