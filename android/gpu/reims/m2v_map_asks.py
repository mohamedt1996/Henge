#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's questions about the process's mappings, answered without reading /proc/self/maps.
Run by android_port.py on the metal2vulkan checkout.

Before a no-copy buffer or an IOSurface texture is made over the guest's pages, the runtime asks
whether the process maps every page of them writable (`refuse_unwritable`, before an import; and
`mapped_writable`, as a copy of the pages is registered, `copied.rs`), and readable where the
kernel's own read of them failed (`mapped_readable`). On Linux 6.11 and later that is one
`PROCMAP_QUERY` a mapping of the window. The tablet's kernel is older, so the runtime reads the
lines of /proc/self/maps instead, from the first line to the window: every mapping of the process
below the pages, formatted by the kernel a page of text at a time, at every question. Upstream
measured 4.5 ms an import in a process of 34,000 mappings. On Android reims-vgpu's views of the
guest's scattered pages are a kernel mapping a run (no /dev/udmabuf; vm.max_map_count is raised to
2,097,152 for them), so the process holds hundreds of thousands, each no-copy object asks twice (the
import that fails, then its copy), and macOS makes the display's surfaces again and again. The
kernel holds the process's map lock while it formats each page of lines, which the views' own
mmap and munmap, on the threads that answer the guest's GPU registers, wait for.

The tablet's log of 0.3.23 (2026-11-10): with the copies' comparisons down to what the commands use,
the Mac's CPUs still waited 20 s of 30 on GPU registers at 17 frames a second, the guest's driver
still gave up on a stamp after 2 s, and the one FIFO thread at work in that report's samples was in
`Device::new_buffer_with_descriptor`, in `std::fs::File::read`: the maps.

`MADV_POPULATE_WRITE` and `MADV_POPULATE_READ` (Linux 5.14) answer the same question for the pages
alone: the kernel walks the mappings over the range, and succeeds only where every page of it lies
in a mapping with the permission (`check_vma_flags`: no write permission is EINVAL, a gap ENOMEM),
faulting each page in as the copy's own read and the GPU's written-back bytes would a moment later.
So a success is the answer "permitted", with no line read. Anything else (a refusal, a mapping the
kernel cannot pin such as a dma-buf's, a failed fault) reads the lines as before, so every answer
that is not "permitted" is still the maps' own. Whether the kernel has the advice is tried once, on
two pages of the runtime's own (one writable, one not).

`M2V_MAPS_LINES` (any value) reads the lines for every question, as before. Every 10 s of
questions a line says what they cost: `metal2vulkan: host mappings: ...`.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_map_asks.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/host_memory.rs': [
        ("""fn mappings_permitted(start: u64, end: u64, access: Access, asked: &mut u64) -> Option<bool> {
    use std::io::BufRead;
    let maps = kept_maps()?;
    if let Some(permitted) = queried_permitted(&maps, start, end, access, asked) {
        return Some(permitted);
    }
    // The lines are read from a file of their own: reading moves its position, which the kept
    // one's queries share among threads.
    let maps = std::fs::File::open("/proc/self/maps").ok()?;
    let lines = std::io::BufReader::new(maps).lines().map_while(Result::ok);
    Some(permitted(lines, start, end, access, asked))
}
""", """fn mappings_permitted(start: u64, end: u64, access: Access, asked: &mut u64) -> Option<bool> {
    use std::io::BufRead;
    let began = std::time::Instant::now();
    let maps = kept_maps()?;
    if let Some(permitted) = queried_permitted(&maps, start, end, access, asked) {
        return Some(permitted);
    }
    // Henge's Android port (m2v_map_asks.py): a kernel without the query answers "permitted" for
    // the pages alone, with no line of the maps read; every other answer is the lines'.
    if henge_populates(start, end, access) {
        *asked += 1;
        henge_map_asks(false, 0, began);
        return Some(true);
    }
    // The lines are read from a file of their own: reading moves its position, which the kept
    // one's queries share among threads.
    let maps = std::fs::File::open("/proc/self/maps").ok()?;
    let mut read = 0u64;
    let lines = (std::io::BufReader::new(maps).lines().map_while(Result::ok)).inspect(|_| read += 1);
    let answer = permitted(lines, start, end, access, asked);
    henge_map_asks(true, read, began);
    Some(answer)
}

/// Henge's Android port (m2v_map_asks.py): whether the kernel populates every page of
/// `[start, end)` for `access` (`MADV_POPULATE_WRITE` for [`Access::Write`], `MADV_POPULATE_READ`
/// for [`Access::Read`]; Linux 5.14), which it does only where the process maps every page of the
/// range, without a gap, in a mapping with the permission: `true` is [`mappings_permitted`]'s
/// "permitted". `false` says nothing (no permission, a gap, a mapping the kernel cannot pin, a
/// fault that failed, a kernel without the advice, [`Access::Pin`], `M2V_MAPS_LINES` set): the
/// caller reads the lines then. The pages are faulted in, as the read or write that follows the
/// question would fault them.
#[cfg(any(target_os = "linux", target_os = "android"))]
fn henge_populates(start: u64, end: u64, access: Access) -> bool {
    const MADV_POPULATE_READ: libc::c_int = 22;
    const MADV_POPULATE_WRITE: libc::c_int = 23;
    let advice = match access {
        Access::Read => MADV_POPULATE_READ,
        Access::Write => MADV_POPULATE_WRITE,
        Access::Pin => return false,
    };
    // SAFETY: `sysconf` reads a constant of the system's.
    let page = match u64::try_from(unsafe { libc::sysconf(libc::_SC_PAGESIZE) }) {
        Ok(page) if page.is_power_of_two() => page,
        _ => return false,
    };
    // Whether this kernel has the advice and answers it as Linux 5.14 does, tried once on two
    // pages of the runtime's own: a writable one is populated for writing and for reading, one
    // without write permission is not populated for writing.
    static WORKS: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let works = *WORKS.get_or_init(|| {
        if std::env::var_os("M2V_MAPS_LINES").is_some() {
            return false;
        }
        let length = 2 * page as usize;
        // SAFETY: two pages of anonymous memory of this call's own, unmapped before it returns;
        // the advice reads and writes nothing of the process's but their page table entries.
        unsafe {
            let pages = libc::mmap(
                std::ptr::null_mut(),
                length,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            );
            if pages == libc::MAP_FAILED {
                return false;
            }
            let second = pages.cast::<u8>().add(page as usize).cast::<c_void>();
            let works = libc::mprotect(second, page as usize, libc::PROT_READ) == 0
                && libc::madvise(pages, page as usize, MADV_POPULATE_WRITE) == 0
                && libc::madvise(pages, length, MADV_POPULATE_READ) == 0
                && libc::madvise(second, page as usize, MADV_POPULATE_WRITE) != 0
                && libc::madvise(pages, length, MADV_POPULATE_WRITE) != 0;
            libc::munmap(pages, length);
            works
        }
    });
    if !works || end <= start {
        return false;
    }
    let first = start - start % page;
    let Some(last) = end.checked_next_multiple_of(page) else {
        return false;
    };
    let Ok(length) = usize::try_from(last - first) else {
        return false;
    };
    // SAFETY: the advice changes no byte and no mapping: it faults the range's pages in, and
    // fails where a page of it is not mapped with the permission.
    unsafe { libc::madvise(first as *mut c_void, length, advice) == 0 }
}

/// Henge's Android port (m2v_map_asks.py): what the questions about the process's mappings cost,
/// a line every 10 s of them: how many were asked, how many of them read the lines of
/// `/proc/self/maps` and how many lines, and the time in all.
#[cfg(any(target_os = "linux", target_os = "android"))]
fn henge_map_asks(lines: bool, read: u64, began: std::time::Instant) {
    use std::sync::atomic::{AtomicU64, Ordering};
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    static ASKS: AtomicU64 = AtomicU64::new(0);
    static BY_LINES: AtomicU64 = AtomicU64::new(0);
    static LINES: AtomicU64 = AtomicU64::new(0);
    static NANOS: AtomicU64 = AtomicU64::new(0);
    static SAID: AtomicU64 = AtomicU64::new(0);
    let start = *START.get_or_init(std::time::Instant::now);
    ASKS.fetch_add(1, Ordering::Relaxed);
    BY_LINES.fetch_add(u64::from(lines), Ordering::Relaxed);
    LINES.fetch_add(read, Ordering::Relaxed);
    NANOS.fetch_add(
        u64::try_from(began.elapsed().as_nanos()).unwrap_or(u64::MAX),
        Ordering::Relaxed,
    );
    let now = start.elapsed().as_secs();
    let said = SAID.load(Ordering::Relaxed);
    if now >= said + 10
        && SAID
            .compare_exchange(said, now, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
    {
        let asks = ASKS.swap(0, Ordering::Relaxed);
        let by_lines = BY_LINES.swap(0, Ordering::Relaxed);
        let read = LINES.swap(0, Ordering::Relaxed);
        let nanos = NANOS.swap(0, Ordering::Relaxed);
        eprintln!(
            "metal2vulkan: host mappings: {asks} questions about the pages of no-copy objects, \\
             {by_lines} of them read /proc/self/maps ({read} lines), in {} ms",
            nanos / 1_000_000,
        );
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
                sys.exit(f'm2v_map_asks: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_map_asks: {changed} edits applied')


if __name__ == '__main__':
    main()
