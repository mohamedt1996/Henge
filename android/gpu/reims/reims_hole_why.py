#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Why a no-copy buffer over a GPU task's pages is refused, said. Run by android_port.py on the reims-vgpu checkout.

0.3.111's run (2026-11-24): right after login, 145 no-copy buffers over task pages 0xf5 and 0xf9 answered nil ("pages
it cannot read"), each GPU task over them faulted, and macOS restarted its GPU 224 times (the black box where its
"shut down because of a problem" window was). metal2vulkan's own question about those pages was answered by the gap
check (0.3.111): part of the range is not mapped in the app at all. A task's pages are mapped into a fresh anonymous
allocation (`MemoryMap::new_range_mapping`), readable on Linux even where the page table has holes, so the gap comes
from somewhere else. This says where, for the first refusals:
- `LinuxVm::henge_hole_report`: of the buffer's pages, how many do not read, the first such page, whether the kernel
  maps it at all (msync), what the backing table holds there, and the kernel's mappings around it (/proc/self/maps,
  read only for these 4 reports).
- `new_virtual_mapping`: how many of the task range's pages the GPU page table maps (first 8 short walks).

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_hole_why.py REIMS_DIR"""
import pathlib
import sys

EDITS = {
    'crates/reims-vgpu-vulkan/src/gpu/device.rs': [
        ("""                Err(e) if !OsVm::new().readable(bytes.ptr(), bytes.len()) => {
                    no_buffer(sel, &format_args!("pages it cannot read: {e}"));
                    Ok(None)
                }""", """                Err(e) if !OsVm::new().readable(bytes.ptr(), bytes.len()) => {
                    no_buffer(sel, &format_args!("pages it cannot read: {e}"));
                    // Henge's Android port (reims_hole_why.py): which pages, and why.
                    #[cfg(any(target_os = "linux", target_os = "android"))]
                    OsVm::new().henge_hole_report(bytes.ptr(), bytes.len());
                    Ok(None)
                }"""),
    ],
    'crates/reims-vgpu-vulkan/src/os/linux/vm.rs': [
        ("""        files::readable(first, length).unwrap_or_else(|| read_probe(first, length))
    }
}
""", """        files::readable(first, length).unwrap_or_else(|| read_probe(first, length))
    }

    /// Henge's Android port (reims_hole_why.py): why `len` bytes at `addr` do not all read, for the first 4
    /// no-copy buffers refused so: the pages that do not, the first of them, whether the kernel maps it (msync
    /// fails ENOMEM only where nothing is mapped), the backing table's answer and file there, and the kernel's
    /// mappings around it.
    pub fn henge_hole_report(&self, addr: HostAddr, len: u64) {
        use std::io::BufRead as _;
        use std::sync::atomic::{AtomicU32, Ordering};
        static SAID: AtomicU32 = AtomicU32::new(0);
        if SAID.fetch_add(1, Ordering::Relaxed) >= 4 {
            return;
        }
        let Some((first, length)) = pages(addr, len) else {
            return;
        };
        let page = page_size();
        let (mut bad, mut runs, mut was_bad, mut first_bad) = (0u64, 0u64, false, None);
        let mut at = first;
        while at < first + length {
            let ok = files::readable(at, page).unwrap_or_else(|| read_probe(at, page));
            if !ok {
                bad += 1;
                first_bad.get_or_insert(at);
            }
            runs += u64::from(!ok && !was_bad);
            was_bad = !ok;
            at += page;
        }
        let Some(hole) = first_bad else {
            log::warn!(
                target: "reims_vgpu_vulkan::os",
                "(Henge) a no-copy buffer of {len:#x} bytes at {first:#x}: every page reads now"
            );
            return;
        };
        // SAFETY: MS_ASYNC writes nothing back; the call only walks the mappings over the page.
        let mapped = unsafe { libc::msync(ptr(hole), page.to_usize(), libc::MS_ASYNC) } == 0
            || std::io::Error::last_os_error().raw_os_error() != Some(libc::ENOMEM);
        let table = files::readable(hole, page);
        let filed = files::pieces(hole, page).map(|p| {
            p.first()
                .map(|p| format!("fd {} at {:#x}, prot {:#x}", p.backing.fd.as_raw_fd(), p.backing.offset, p.prot))
                .unwrap_or_default()
        });
        let (from, to) = (hole.saturating_sub(0x10000), hole.saturating_add(0x10000));
        let mut around = Vec::new();
        if let Ok(maps) = std::fs::File::open("/proc/self/maps") {
            for line in std::io::BufReader::new(maps).lines().map_while(Result::ok) {
                let Some((a, b)) = line.split_whitespace().next().and_then(|r| r.split_once('-')) else {
                    continue;
                };
                let (Ok(a), Ok(b)) = (u64::from_str_radix(a, 16), u64::from_str_radix(b, 16)) else {
                    continue;
                };
                if a >= to {
                    break;
                }
                if b > from && around.len() < 12 {
                    around.push(line);
                }
            }
        }
        log::warn!(
            target: "reims_vgpu_vulkan::os",
            "(Henge) a no-copy buffer of {len:#x} bytes at {first:#x}: {bad} of {} pages do not read, in {runs} \\
             run(s), the first at +{:#x} ({}); the backing table there: {table:?}, file {filed:?}; the kernel's \\
             mappings around it: {}",
            length / page,
            hole - first,
            if mapped { "mapped, without read access" } else { "NOT MAPPED: a gap" },
            if around.is_empty() { "none".to_string() } else { around.join(" | ") }
        );
    }
}
"""),
    ],
    'crates/reims-vgpu-core/src/gpu_task/memory.rs': [
        ("""        if range.physical_length != 0 {
            ranges.push(range);
        }
        if need_writable && !writable {
            return Ok(None);
        }""", """        if range.physical_length != 0 {
            ranges.push(range);
        }
        // Henge's Android port (reims_hole_why.py): a walk that found fewer pages than asked, said (first 8).
        {
            use std::sync::atomic::{AtomicU32, Ordering};
            static SAID: AtomicU32 = AtomicU32::new(0);
            let found: u64 = ranges.iter().map(|r| r.physical_length).sum();
            if found < length && SAID.fetch_add(1, Ordering::Relaxed) < 8 {
                log::warn!(
                    "(Henge) task offset {offset:#x}: the GPU page table maps {} of the {} pages asked ({} run(s)); \\
                     the mapping's tail is fresh memory",
                    found / PAGE,
                    length / PAGE,
                    ranges.len()
                );
            }
        }
        if need_writable && !writable {
            return Ok(None);
        }"""),
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
                sys.exit(f'reims_hole_why: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_hole_why: {changed} edits applied')


if __name__ == '__main__':
    main()
