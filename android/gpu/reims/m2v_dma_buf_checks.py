#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's dma-bufs of host pages on Android: a source's answer the runtime cannot check is
a copy of the pages, said in the log, not the app's death. Run by android_port.py on the
metal2vulkan checkout, after m2v_no_host_import.py.

The tablet, 2026-11-12 (build 55, Turnip, /dev/udmabuf open): the first IOSurface texture ended the
app with `newTextureWithDescriptor:iosurface:plane:: the Vulkan runtime refused it: invalid use of
new_buffer_with_descriptor: DmaBuf`, and the next start fell back to Qualcomm's driver, where every
no-copy object is a copy. `Problem::DmaBuf` has one site, `Device::import_from_source`: the
dma-bufs reims-vgpu's source made did not pass `answers`, the runtime's check of its source's
contract, which the runtime treats as the caller's misuse. `answers` is two checks:

  - `chunks_cover`: the chunks' lengths against the request. For one chunk of the request's own
    length (what reims-vgpu makes where the window fits udmabuf's limits, 8192 pieces and 256 MB on
    the tablet, against a 0x7d0000-byte surface) it holds by construction.
  - each descriptor "is a dma-buf of at least its chunk's length": `fstatfs` on the descriptor must
    succeed and name the kernel's dma-buf file system. On Android `fstatfs` is a `getattr` on that
    file system's superblock, which has no SELinux label of its own (no genfscon for "dmabuf"), and
    an app's domain is not allowed it: the call fails with EACCES and the check answers "no dma-buf"
    for a descriptor udmabuf just made. That is the likeliest cause, and not one the log could show.

So:

  1. A descriptor is a dma-buf where `fstatfs` says so OR where the kernel names its file
     `/dmabuf:...` (`/proc/self/fd/N`, the dma-buf file system's own dentry name) or shows a
     dma-buf's `exp_name:` in its fdinfo: both are the process's own to read, and no memfd or other
     file answers either.
  2. Where the answer still fails the check, the log says what was asked and what came back (the
     chunks' lengths, each descriptor's size, its file system or `fstatfs`'s error, its name), eight
     times at most, and the import keeps the pointer's refusal, which every caller turns into a
     copy of the pages (`copied-host-pages`), as on a driver that imports no dma-bufs at all.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_dma_buf_checks.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/dma_buf.rs': [
        # 1. What a dma-buf is, where fstatfs is refused.
        ('''    let mut stat = std::mem::MaybeUninit::<libc::statfs>::uninit();
    // SAFETY: the descriptor is open for the call, and `fstatfs` writes the structure.
    if unsafe { libc::fstatfs(fd.as_raw_fd(), stat.as_mut_ptr()) } != 0 {
        return None;
    }
    // SAFETY: the call succeeded, so it wrote the structure.
    let stat = unsafe { stat.assume_init() };
    // The field's type differs between architectures and C libraries.
    if i128::from(stat.f_type) != i128::from(DMA_BUF_MAGIC) {
        return None;
    }
''', '''    let mut stat = std::mem::MaybeUninit::<libc::statfs>::uninit();
    // SAFETY: the descriptor is open for the call, and `fstatfs` writes the structure.
    let on_dma_buf_fs = unsafe { libc::fstatfs(fd.as_raw_fd(), stat.as_mut_ptr()) } == 0 && {
        // SAFETY: the call succeeded, so it wrote the structure.
        let stat = unsafe { stat.assume_init() };
        // The field's type differs between architectures and C libraries.
        i128::from(stat.f_type) == i128::from(DMA_BUF_MAGIC)
    };
    // Henge's Android port (m2v_dma_buf_checks.py): Android's SELinux policy refuses an app the
    // `fstatfs` of the dma-buf file system, so the kernel's name for the file answers too.
    if !on_dma_buf_fs && !henge_named_dma_buf(fd.as_raw_fd()) {
        return None;
    }
'''),
        # The helpers, before the importer's code.
        ('''/// How long one wait for the sparse binding lasts before it is asked again.
const WAIT_SLICE: Duration = Duration::from_millis(100);
''', '''/// Henge's Android port (m2v_dma_buf_checks.py): whether the kernel names the open file `raw` a
/// dma-buf: its `/proc/self/fd` link reads `/dmabuf:...` (the dma-buf file system's dentry name),
/// or its fdinfo carries a dma-buf's `exp_name:` line. Read where `fstatfs` cannot say (Android
/// refuses an app that call on the dma-buf file system). No other kind of file answers either.
fn henge_named_dma_buf(raw: i32) -> bool {
    let linked = std::fs::read_link(format!("/proc/self/fd/{raw}"))
        .is_ok_and(|name| name.as_os_str().as_encoded_bytes().starts_with(b"/dmabuf:"));
    linked
        || std::fs::read_to_string(format!("/proc/self/fdinfo/{raw}"))
            .is_ok_and(|info| info.lines().any(|line| line.starts_with("exp_name:")))
}

/// Henge's Android port (m2v_dma_buf_checks.py): say in the log how the dma-bufs a source made
/// for `request` fail `answers`, for `call`: the chunks against the request, and for each of the
/// first four descriptors its size, its file system (or why `fstatfs` failed) and its name. Eight
/// times a process at most.
fn henge_describe_unanswered(
    call: &str,
    chunks: &[DmaBufChunk],
    request: &DmaBufRequest,
    page: u64,
) {
    use std::os::fd::AsRawFd;
    use std::sync::atomic::{AtomicU32, Ordering};
    static SAID: AtomicU32 = AtomicU32::new(0);
    if SAID.fetch_add(1, Ordering::Relaxed) >= 8 {
        return;
    }
    let lengths: Vec<u64> = chunks.iter().map(|chunk| chunk.length).collect();
    let covers = chunks_cover(&lengths, request, page);
    let mut each = String::new();
    for chunk in chunks.iter().take(4) {
        let raw = chunk.fd.as_raw_fd();
        let mut stat = std::mem::MaybeUninit::<libc::statfs>::uninit();
        // SAFETY: the descriptor is open for the call, and `fstatfs` writes the structure.
        let file_system = if unsafe { libc::fstatfs(raw, stat.as_mut_ptr()) } == 0 {
            // SAFETY: the call succeeded, so it wrote the structure.
            let stat = unsafe { stat.assume_init() };
            format!("file system {:#x}", i128::from(stat.f_type))
        } else {
            format!("fstatfs: {}", std::io::Error::last_os_error())
        };
        // SAFETY: a query of an open descriptor; nothing reads through its position.
        let end = unsafe { libc::lseek(raw, 0, libc::SEEK_END) };
        let name = std::fs::read_link(format!("/proc/self/fd/{raw}"))
            .map_or_else(|error| format!("no name ({error})"), |name| name.display().to_string());
        each.push_str(&format!(
            " [{:#x} bytes of a file of {end} bytes, {file_system}, {name}{}]",
            chunk.length,
            if henge_named_dma_buf(raw) { ", a dma-buf by name" } else { "" },
        ));
    }
    eprintln!(
        "metal2vulkan: {call}: the dma-bufs made of {:#x} bytes of host pages at {:p} (granule \\
         {:#x}, host page {page:#x}) do not answer the request: {} chunk(s) of {lengths:?} bytes, \\
         which {} it:{each}; the pages are copied instead",
        request.length,
        request.start,
        request.granule,
        chunks.len(),
        if covers { "cover" } else { "do not cover" },
    );
}

/// How long one wait for the sparse binding lasts before it is asked again.
const WAIT_SLICE: Duration = Duration::from_millis(100);
'''),
        # 2. An answer that fails the check is a copy, said, not misuse.
        ('''        let chunks = source(request).map_err(|_| not_importable())?;
        if !answers(&chunks, &request, importer.page) {
            return Err(invalid(call, Problem::DmaBuf));
        }
''', '''        let chunks = source(request).map_err(|_| not_importable())?;
        if !answers(&chunks, &request, importer.page) {
            // Henge's Android port (m2v_dma_buf_checks.py): upstream answers misuse here
            // (`invalid(call, Problem::DmaBuf)`), which reims-vgpu makes the app's death. The
            // log says what the source answered, and the pages keep the pointer's refusal: a
            // copy of them, as where the device imports no dma-bufs. The descriptors close here.
            henge_describe_unanswered(call, &chunks, &request, importer.page);
            let _misuse_upstream = invalid(call, Problem::DmaBuf);
            return Err(not_importable());
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
                sys.exit(f'm2v_dma_buf_checks: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_dma_buf_checks: {changed} edits applied')


if __name__ == '__main__':
    main()
