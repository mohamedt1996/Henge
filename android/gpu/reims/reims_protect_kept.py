#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""reims-vgpu where the kernel refuses to change a view's protection: the view stays, as it is.
Run by android_port.py on the reims-vgpu checkout.

0.3.113's run on Gunyah (2026-11-24): 229 no-copy buffers over GPU task pages (0xad, 0xf5, 0xf9, all
task 2's, WindowServer's) answered nil, "pages it cannot read", and macOS restarted its GPU at each.
0.3.112's report names the hole: the buffer's 16 KB lie in a gap of the app's address space, with no
entry in the library's backing table. No view was refused and nothing else was logged.

The path: a mapping of task pages is made of views of the Mac's RAM (read and write, as the RAM is
registered), and core then lowers a mapping over a read-only leaf to read-only (`protect_max`). Core
keeps Apple's own flaw there: where the protection is refused it frees the whole allocation "then the
mapping is made anyway", so the mapping's address is a gap. On this tablet the kernel refuses every new
map of the RAM's file once the Mac runs on Gunyah (EACCES, 0.3.108), and mprotect of a file's mapping
passes the same check (SELinux's file_mprotect), so every read-only mapping lost its pages, silently
(`refused` logs nothing for EACCES).

On Android a protection the kernel refuses with EACCES or EPERM now leaves the view as it is, read and
write: the mapping itself still says read-only to its users. The first, and each 256th, is logged; any
other errno is logged (first 8) and refused as before, so the next log says whether this was the cause.

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_protect_kept.py REIMS_VGPU_DIR"""
import pathlib
import sys

EDITS = {
    'crates/reims-vgpu-vulkan/src/os/linux/vm.rs': [
        ("""        if unsafe { libc::mprotect(ptr(first), length.to_usize(), bits) } != 0 {
            return Err(refused("protect_max", true));
        }
        files::protect(first, length, bits);
        Ok(())
""", """        if unsafe { libc::mprotect(ptr(first), length.to_usize(), bits) } != 0 {
            // Henge's Android port (reims_protect_kept.py): the kernel's reason, taken before anything
            // else can change errno.
            let why = std::io::Error::last_os_error();
            let kr = refused("protect_max", true);
            static REFUSED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let n = REFUSED.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
            if cfg!(target_os = "android")
                && matches!(why.raw_os_error(), Some(libc::EACCES) | Some(libc::EPERM))
            {
                // The view stays as it is (the table too): core would free the allocation and hand
                // out a mapping over the gap.
                if n == 1 || n % 256 == 0 {
                    log::warn!(
                        target: "reims_vgpu_vulkan::os",
                        "(Henge) protect_max: the kernel refuses protection {bits:#x} for {length:#x} bytes at \\
                         {first:#x} ({why}); the view stays as it is ({n} so far)"
                    );
                }
                return Ok(());
            }
            if n <= 8 {
                log::warn!(
                    target: "reims_vgpu_vulkan::os",
                    "(Henge) protect_max: mprotect of {length:#x} bytes at {first:#x} to {bits:#x}: {why}"
                );
            }
            return Err(kr);
        }
        files::protect(first, length, bits);
        Ok(())
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
                sys.exit(f'reims_protect_kept: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_protect_kept: {changed} edits applied')


if __name__ == '__main__':
    main()
