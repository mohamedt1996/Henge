#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Why reims-vgpu refuses a view of guest pages, said at warn level. Run by android_port.py on the
reims-vgpu checkout.

0.3.105 and 0.3.107 on the Gunyah engine's reserved RAM: "ring of 0x4000 bytes at 0x79bd8000 could
not be mapped" and "root page 0x1e813 not mapped: no root node", so the GPU never started and
WindowServer aborted every 60 s; the app itself mapped a page of the same file again at a fixed
address without trouble. A view is refused in core's `MemoryMap::new_range_mapping` (an address in
no range of the map, the allocation, the host's remap) or in the Linux VM's remap (no file behind
the source, the kernel's mmap), each silently or at debug level. Each now logs, the first 20.

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_view_why.py REIMS_VGPU_DIR"""
import pathlib
import sys

WHY = """    /// Henge's Android port (reims_view_why.py): why a view of guest pages was refused, the first 20.
    fn henge_view_why(args: std::fmt::Arguments<'_>) {
        use std::sync::atomic::{AtomicU32, Ordering};
        static SHOWN: AtomicU32 = AtomicU32::new(0);
        if SHOWN.fetch_add(1, Ordering::Relaxed) < 20 {
            log::warn!("a view of guest pages refused: {args}");
        }
    }

    /// A fresh VM allocation with each request range remapped into it"""

EDITS = {
    'crates/reims-vgpu-core/src/memory/map.rs': [
        ("""    /// A fresh VM allocation with each request range remapped into it""", WHY),
        ("""        let Ok(address) = vm.allocate_anywhere(total_length) else {
            return None;
        };""", """        let Ok(address) = vm.allocate_anywhere(total_length) else {
            Self::henge_view_why(format_args!("no address space for {total_length:#x} bytes"));
            return None;
        };"""),
        ("""            if range.physical_length != 0 {
                refuse(&mut pieces);
                return None;
            }
        }
        if pieces.remap(vm).is_err() {
            free_all();
            return None;
        }""", """            if range.physical_length != 0 {
                Self::henge_view_why(format_args!(
                    "guest {:#x} (+{:#x}) is in no range of the memory map {:x?}",
                    range.physical_address,
                    range.physical_length,
                    self.ranges.iter().map(|r| (r.physical_address, r.physical_end)).collect::<Vec<_>>()
                ));
                refuse(&mut pieces);
                return None;
            }
        }
        if let Err((guest, length)) = pieces.remap(vm) {
            Self::henge_view_why(format_args!(
                "the host's remap of guest {guest:#x} (+{length:#x}) was refused"
            ));
            free_all();
            return None;
        }"""),
    ],
    'crates/reims-vgpu-vulkan/src/os/linux/vm.rs': [
        ("""    if mapped == libc::MAP_FAILED {
        return Err(refused("a view of guest pages", false));
    }
    Ok(mapped.addr().to_u64())""", """    if mapped == libc::MAP_FAILED {
        // Henge's Android port (reims_view_why.py): the kernel's reason, said.
        let why = std::io::Error::last_os_error();
        let kr = refused("a view of guest pages", false);
        // 0.3.109: the tablet refuses a new map of the Mac's RAM file once Gunyah runs (0.3.108's run: EACCES on
        // every view, where the app's own map of the same page had passed at its start). The pages are already
        // mapped, shared, where the guest's RAM is registered: a view is then a second mapping of those, made by
        // mremap with an old size of 0 (the kernel's duplicate of a shared mapping), which maps no file anew.
        if let Some(target) = at {
            if matches!(why.raw_os_error(), Some(libc::EACCES) | Some(libc::EPERM)) {
                if let Some(src) = files::henge_registered_host(&backing.fd, backing.offset, length) {
                    unsafe extern "C" {
                        #[link_name = "mremap"]
                        fn henge_mremap(
                            addr: *mut libc::c_void,
                            old: libc::size_t,
                            new: libc::size_t,
                            flags: libc::c_int,
                            ...
                        ) -> *mut libc::c_void;
                    }
                    // SAFETY: a duplicate of the registered shared mapping over the caller's range, which a
                    // view replaces (MREMAP_MAYMOVE 1 | MREMAP_FIXED 2).
                    let dup = unsafe { henge_mremap(ptr(src), 0, length.to_usize(), 3, ptr(target)) };
                    let rw = libc::PROT_READ | libc::PROT_WRITE;
                    // SAFETY: the duplicate just made, its protection lowered where the view asks.
                    if dup != libc::MAP_FAILED
                        && (prot == rw || unsafe { libc::mprotect(ptr(target), length.to_usize(), prot) } == 0)
                    {
                        static NOTED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
                        if !NOTED.swap(true, std::sync::atomic::Ordering::Relaxed) {
                            log::warn!(
                                target: "reims_vgpu_vulkan::os",
                                "views of guest pages are duplicates of the RAM's mapping: the kernel refuses new maps \
                                 of its file ({why})"
                            );
                        }
                        return Ok(target);
                    }
                    let again = std::io::Error::last_os_error();
                    log::warn!(
                        target: "reims_vgpu_vulkan::os",
                        "a view of guest pages: the duplicate of the RAM's mapping at {src:#x} was refused too: {again}"
                    );
                } else {
                    log::warn!(
                        target: "reims_vgpu_vulkan::os",
                        "a view of guest pages: no registered RAM mapping holds offset {:#x} (+{length:#x}) of fd {}",
                        backing.offset,
                        backing.fd.as_raw_fd()
                    );
                }
            }
        }
        log::warn!(
            target: "reims_vgpu_vulkan::os",
            "a view of guest pages: mmap of {length:#x} bytes of fd {} at offset {offset:#x}, prot {prot:#x}, \\
             {}: {why}",
            backing.fd.as_raw_fd(),
            if at.is_some() { "fixed" } else { "anywhere" }
        );
        return Err(kr);
    }
    Ok(mapped.addr().to_u64())"""),
        ("""                log::debug!(
                    target: "reims_vgpu_vulkan::os",
                    "remap of {length:#x} bytes at {source:#x}: no file behind them\"""",
         """                log::warn!(
                    target: "reims_vgpu_vulkan::os",
                    "remap of {length:#x} bytes at {source:#x}: no file behind them\""""),
        ("""                    log::debug!(
                        target: "reims_vgpu_vulkan::os",
                        "remap of {len:#x} bytes at {source:#x}: no file behind them\"""",
         """                    log::warn!(
                        target: "reims_vgpu_vulkan::os",
                        "remap of {len:#x} bytes at {source:#x}: no file behind them\""""),
    ],
    'crates/reims-vgpu-vulkan/src/os/linux/files.rs': [
        ("""#[cfg(test)]
mod tests {""", """/// Henge's Android port (reims_view_why.py): the host address where a registration of the guest's RAM maps
/// `length` bytes of `fd` from `offset`, for a view made as a duplicate of that mapping. None where the table
/// is busy (no wait: a caller may hold it) or no registration holds them.
pub(crate) fn henge_registered_host(fd: &Arc<OwnedFd>, offset: u64, length: u64) -> Option<u64> {
    let table = TABLE.try_lock()?;
    table.registered.iter().find_map(|r| {
        let end = offset.checked_add(length)?;
        let held = r.backing.offset.checked_add(r.end - r.first)?;
        (Arc::ptr_eq(&r.backing.fd, fd) && offset >= r.backing.offset && end <= held)
            .then(|| r.first + (offset - r.backing.offset))
    })
}

#[cfg(test)]
mod tests {"""),
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
                sys.exit(f'reims_view_why: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_view_why: {changed} edits applied')


if __name__ == '__main__':
    main()
