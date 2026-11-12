#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan's copied host buffers: a commit copies in the copies its commands use, not every one.
Run by android_port.py on the metal2vulkan checkout.

On a device without VK_EXT_external_memory_host (both Adreno drivers) every no-copy buffer and
IOSurface texture of the guest's is a copy of its pages (`copied.rs`, m2v_no_host_import.py), and at
each command buffer's commit the runtime compares EVERY live copy's pages with the bytes they last
agreed on, whether or not the command buffer touches it: O(all copied bytes) per commit, behind one
lock. The runtime skips unchanged pages only where the pages came with a log of the writes to them,
which reims-vgpu takes from KVM alone. On the tablet (2026-11-10, the thread samples of 0.3.21) the
guest's driver waited tens of seconds for each command buffer, and the FIFO threads that were not
idle were in `CopiedBuffers::copy_in` or waiting for its lock.

A completion already compares only the copies its batches' commands wrote, by the keys of their
uses (`Written`, `DirtyCommit::copies_written`). This gives a commit the same: the keys of every use
its commands noted, reads and writes (`DirtyCommit::copies_used`), and every copy where a command
may reach memory no use names (`reaches_unnamed`, `unnoted`), or the keys were too many to keep. A
copy a command buffer does not use keeps the bytes it last agreed on; the CPU's writes to its pages
are copied in at the commit of the first command buffer that uses it, before any of that command
buffer's work is submitted, which is what the GPU's reads need.

`M2V_COPY_IN_ALL` (any value) restores the comparison of every copy. Every 10 s of commits a line
says what the copies in cost: `metal2vulkan: copied host buffers: ...`; and (0.3.52) every 10 s of
completions another says what the merges back cost: `... N completions compared X MiB and wrote Y
MiB of the GPU's changes back to the Mac's pages, in Z ms`.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_copy_in_used.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/copied.rs': [
        ("""    pub(crate) fn copy_in(&self, counts: &Counters, tally: &DirtyTally) {
        if self.live.load(Ordering::Relaxed) == 0 {
            return;
        }
        let asked: Vec<Asked> = (lock(&self.entries).iter())
            .filter_map(|entry| {
""", """    pub(crate) fn copy_in(&self, counts: &Counters, tally: &DirtyTally, used: &Written) {
        if self.live.load(Ordering::Relaxed) == 0 {
            return;
        }
        // Henge's Android port (m2v_copy_in_used.py): only the copies the command buffer's commands
        // use are compared (`used`), every copy where M2V_COPY_IN_ALL is set.
        static EVERY: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let every = *EVERY.get_or_init(|| std::env::var_os("M2V_COPY_IN_ALL").is_some());
        let began = std::time::Instant::now();
        let (mut live_bytes, mut unused_bytes) = (0u64, 0u64);
        let asked: Vec<Asked> = (lock(&self.entries).iter())
            .filter(|entry| every || used.meets(&entry.keys))
            .filter_map(|entry| {
"""),
        ("""        for entry in lock(&self.entries).iter_mut() {
            let plan = match &entry.log {
""", """        for entry in lock(&self.entries).iter_mut() {
            live_bytes += entry.snapshot.len() as u64;
            if !every && !used.meets(&entry.keys) {
                unused_bytes += entry.snapshot.len() as u64;
                continue;
            }
            let plan = match &entry.log {
"""),
        ("""        counts.copied_copied_in(moved);
        counts.copied_copy_in_skipped(skipped);
    }
""", """        counts.copied_copied_in(moved);
        counts.copied_copy_in_skipped(skipped);
        // Henge's Android port (m2v_copy_in_used.py): what the copies in cost, every 10 s of commits.
        {
            static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
            static COMMITS: AtomicU64 = AtomicU64::new(0);
            static NANOS: AtomicU64 = AtomicU64::new(0);
            static COMPARED: AtomicU64 = AtomicU64::new(0);
            static UNUSED: AtomicU64 = AtomicU64::new(0);
            static SAID: AtomicU64 = AtomicU64::new(0);
            let start = *START.get_or_init(std::time::Instant::now);
            COMMITS.fetch_add(1, Ordering::Relaxed);
            NANOS.fetch_add(
                u64::try_from(began.elapsed().as_nanos()).unwrap_or(u64::MAX),
                Ordering::Relaxed,
            );
            COMPARED.fetch_add(live_bytes - unused_bytes, Ordering::Relaxed);
            UNUSED.fetch_add(unused_bytes, Ordering::Relaxed);
            let now = start.elapsed().as_secs();
            let said = SAID.load(Ordering::Relaxed);
            if now >= said + 10
                && SAID
                    .compare_exchange(said, now, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                let commits = COMMITS.swap(0, Ordering::Relaxed);
                let nanos = NANOS.swap(0, Ordering::Relaxed);
                let compared = COMPARED.swap(0, Ordering::Relaxed);
                let unused = UNUSED.swap(0, Ordering::Relaxed);
                eprintln!(
                    "metal2vulkan: copied host buffers: {} MiB live; {commits} commits compared {} MiB \\
                     and passed over {} MiB their commands do not use, in {} ms",
                    live_bytes >> 20,
                    compared >> 20,
                    unused >> 20,
                    nanos / 1_000_000,
                );
            }
        }
    }
"""),
    ],
    'crates/metal2vulkan-runtime/src/copied.rs#merges': [
        # What the merges back cost (0.3.52): the other half of a copy's upkeep, which no line showed.
        ("""        let owed = self.owed.swap(false, Ordering::AcqRel);
        let (mut moved, mut dropped) = (0u64, 0u64);
""", """        let owed = self.owed.swap(false, Ordering::AcqRel);
        let (mut moved, mut dropped) = (0u64, 0u64);
        // Henge's Android port (m2v_copy_in_used.py): what the merges back cost, every 10 s.
        let henge_began = std::time::Instant::now();
        let mut henge_compared = 0u64;
"""),
        ("""            compared += 1;
            let (copy, pages) = (entry.copy, entry.pages);
""", """            compared += 1;
            henge_compared += entry.snapshot.len() as u64;
            let (copy, pages) = (entry.copy, entry.pages);
"""),
        ("""        counts.copied_merges(compared, skipped);
    }
""", """        counts.copied_merges(compared, skipped);
        // Henge's Android port (m2v_copy_in_used.py): a line every 10 s of completions that compared
        // a copy: how many did, the bytes compared, the bytes the GPU changed that went back to the
        // pages (or were dropped), and the time, the reports to the pages' logs included.
        if compared > 0 {
            static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
            static TURNS: AtomicU64 = AtomicU64::new(0);
            static NANOS: AtomicU64 = AtomicU64::new(0);
            static COMPARED: AtomicU64 = AtomicU64::new(0);
            static WROTE: AtomicU64 = AtomicU64::new(0);
            static SAID: AtomicU64 = AtomicU64::new(0);
            let start = *START.get_or_init(std::time::Instant::now);
            TURNS.fetch_add(1, Ordering::Relaxed);
            NANOS.fetch_add(
                u64::try_from(henge_began.elapsed().as_nanos()).unwrap_or(u64::MAX),
                Ordering::Relaxed,
            );
            COMPARED.fetch_add(henge_compared, Ordering::Relaxed);
            WROTE.fetch_add(moved + dropped, Ordering::Relaxed);
            let now = start.elapsed().as_secs();
            let said = SAID.load(Ordering::Relaxed);
            if now >= said + 10
                && SAID
                    .compare_exchange(said, now, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
            {
                eprintln!(
                    "metal2vulkan: copied host buffers: {} completions compared {} MiB and wrote {} MiB \\
                     of the GPU's changes back to the Mac's pages, in {} ms",
                    TURNS.swap(0, Ordering::Relaxed),
                    COMPARED.swap(0, Ordering::Relaxed) >> 20,
                    WROTE.swap(0, Ordering::Relaxed) >> 20,
                    NANOS.swap(0, Ordering::Relaxed) / 1_000_000,
                );
            }
        }
    }
"""),
    ],
    'crates/metal2vulkan-runtime/src/dirty_pages.rs': [
        ("""pub(crate) struct DirtyCommit {
    written: Vec<Use<ResourceKey>>,
""", """pub(crate) struct DirtyCommit {
    /// Henge's Android port (m2v_copy_in_used.py): the key of every use its commands noted, reads
    /// and writes, for its commit's copy in of copied host buffers ([`Self::copies_used`]).
    used: KeyMap<ResourceKey, ()>,
    /// Those keys were more than [`USED`]: every copy counts as used.
    used_overflowed: bool,
    written: Vec<Use<ResourceKey>>,
"""),
        ("""/// The most writes a command buffer keeps apart, past which they are one write of everything.
const WRITES: usize = 4096;
""", """/// The most writes a command buffer keeps apart, past which they are one write of everything.
const WRITES: usize = 4096;

/// Henge's Android port (m2v_copy_in_used.py): the most keys of uses a command buffer keeps, past
/// which its commit copies in every copied host buffer.
const USED: usize = 1 << 16;
"""),
        ("""    pub(crate) fn note(&mut self, uses: &[Use<ResourceKey>], shadows: &DirtyShadows) {
        let imports = shadows.in_place().then(|| lock(&shadows.imports));
""", """    pub(crate) fn note(&mut self, uses: &[Use<ResourceKey>], shadows: &DirtyShadows) {
        // Henge's Android port (m2v_copy_in_used.py): every use's key, for the commit's copy in.
        if !self.used_overflowed {
            for used in uses {
                self.used.insert(used.key, ());
            }
            if self.used.len() > USED {
                self.used_overflowed = true;
                self.used.clear();
            }
        }
        let imports = shadows.in_place().then(|| lock(&shadows.imports));
"""),
        ("""    /// Its commands' writes, for the merge back of copied host buffers (`copied.rs`): the keys
    /// they wrote, or every copy where it kept them not all apart.
    pub(crate) fn copies_written(&self, into: &mut Written) {
""", """    /// Henge's Android port (m2v_copy_in_used.py): its commands' uses, for its commit's copy in
    /// of copied host buffers (`copied.rs`): the keys they used, or every copy where they were
    /// too many to keep.
    pub(crate) fn copies_used(&self, into: &mut Written) {
        if self.used_overflowed {
            into.wrote_all();
            return;
        }
        for key in self.used.keys() {
            into.wrote(*key);
        }
    }

    /// Its commands' writes, for the merge back of copied host buffers (`copied.rs`): the keys
    /// they wrote, or every copy where it kept them not all apart.
    pub(crate) fn copies_written(&self, into: &mut Written) {
"""),
    ],
    'crates/metal2vulkan-runtime/src/command_buffer/mod.rs': [
        ("""        shared.copied.copy_in(&shared.counts, shared.dirty.tally());
""", """        // Henge's Android port (m2v_copy_in_used.py): the copies its commands use, by the keys of
        // the uses they noted; every copy where a command may reach memory no use names, as the
        // merge back takes every copy as written then (`completion.rs`).
        let reaches_unnamed = self.state().reaches_unnamed;
        let used = if self.0.unnoted.load(Ordering::Relaxed) || reaches_unnamed {
            crate::copied::Written::all()
        } else {
            let mut used = crate::copied::Written::default();
            lock(&self.0.dirty).copies_used(&mut used);
            used
        };
        shared.copied.copy_in(&shared.counts, shared.dirty.tally(), &used);
"""),
    ],
}


def main() -> None:
    root = pathlib.Path(sys.argv[1])
    changed = 0
    for rel, edits in EDITS.items():
        path = root / rel.split('#')[0] # a file's second group of edits is keyed 'path#name'
        text = path.read_text()
        for old, new in edits:
            if new in text:
                continue
            if text.count(old) != 1:
                sys.exit(f'm2v_copy_in_used: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_copy_in_used: {changed} edits applied')


if __name__ == '__main__':
    main()
