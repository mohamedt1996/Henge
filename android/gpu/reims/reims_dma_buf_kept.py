#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Backports of reims-vgpu's dma-buf answers of 2026-11-15 to the last revision with Apple's
ParavirtualizedGraphics glue (dae3e6b, 2026-11-07), which Henge's Android port builds.
Run by android_port.py on the reims-vgpu checkout (0.3.69).

  - 56c760874 "Read udmabuf's limits at most once a second": every dma-buf of a no-copy import's
    pages read udmabuf's two module parameters from sysfs (9% of a texture's making over a buffer's
    pages, upstream measured). They are kept for a second here. Android's limits never change while
    the app runs, so upstream's second try at a refusal with freshly read limits is left out.
  - c52a5532e "Answer a texture's pages asked for again with the answer kept": each texture over a
    no-copy buffer's pages asks the buffer's import source for a dma-buf of its pages, which walked
    the backing table and made a udmabuf every time (upstream: 1,244 of 1,368 asks were asked
    before; 73% of those jobs' kernel time). Each source keeps its latest four answers: dma-bufs
    made are answered again as duplicates of their descriptors (the same udmabuf, so the same
    pages, which stay mapped while the source lives), and a refusal for udmabuf's limits is answered
    again while the limits are those it was refused at.

  - e8ee29fdf "Join a dma-buf's runs as the backing table is walked", its second half: runs one
    dma-buf takes are made into it as they are, not copied into a chunk first (a texture's pages,
    at a granule of their length, always are). The walk itself stays this revision's (its files of
    zeros, which upstream deleted, sit in it).

Every 10 s with answers, one line: "reims-vgpu: (Henge) dma-buf answers: N made, M kept".

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_dma_buf_kept.py REIMS_VGPU_DIR"""
import pathlib
import sys

KEPT_TYPES = '''
/// Henge's Android port (reims_dma_buf_kept.py, upstream c52a5532e): a source's latest answers.
const HENGE_KEPT_ANSWERS: usize = 4;

/// Henge's Android port (reims_dma_buf_kept.py): the answers a source made and answered again.
static HENGE_ANSWERS_MADE: AtomicU64 = AtomicU64::new(0);
static HENGE_ANSWERS_KEPT: AtomicU64 = AtomicU64::new(0);

/// Henge's Android port (reims_dma_buf_kept.py): one line every 10 s with answers.
fn henge_note_answer(kept: bool) {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    static SAID: AtomicU64 = AtomicU64::new(0);
    let start = *START.get_or_init(std::time::Instant::now);
    match kept {
        true => HENGE_ANSWERS_KEPT.fetch_add(1, Ordering::Relaxed),
        false => HENGE_ANSWERS_MADE.fetch_add(1, Ordering::Relaxed),
    };
    let now = start.elapsed().as_secs();
    let said = SAID.load(Ordering::Relaxed);
    if now >= said + 10
        && SAID
            .compare_exchange(said, now, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
    {
        eprintln!(
            "reims-vgpu: (Henge) dma-buf answers: {} made, {} kept",
            HENGE_ANSWERS_MADE.swap(0, Ordering::Relaxed),
            HENGE_ANSWERS_KEPT.swap(0, Ordering::Relaxed),
        );
    }
}

/// Henge's Android port (reims_dma_buf_kept.py): a dma-buf source's latest answers, the latest
/// last, at most [`HENGE_KEPT_ANSWERS`].
#[derive(Default)]
struct HengeKept {
    answers: Vec<((u64, u64, u64), HengeAnswer)>,
}

/// A kept answer: the dma-bufs made (their descriptors and lengths), or a refusal for udmabuf's
/// limits.
enum HengeAnswer {
    Made(Vec<(OwnedFd, u64)>),
    Refused {
        pieces: u64,
        bytes: u64,
        granule: u64,
        limits: super::dma_buf::Limits,
    },
}

impl HengeKept {
    /// The answer kept for these pages, again: duplicates of the dma-bufs made, or the refusal
    /// where the limits are still those it was refused at. `None` where none is kept, or a
    /// descriptor would not duplicate.
    fn again(
        &mut self,
        first: u64,
        length: u64,
        granule: u64,
    ) -> Option<Result<Vec<rt::DmaBufChunk>, super::dma_buf::Refusal>> {
        let asked = (first, length, granule);
        let at = self.answers.iter().position(|(pages, _)| *pages == asked)?;
        let answer = match &self.answers[at].1 {
            HengeAnswer::Made(files) => Ok((files.iter())
                .map(|(fd, length)| {
                    Some(rt::DmaBufChunk {
                        fd: fd.try_clone().ok()?,
                        length: *length,
                    })
                })
                .collect::<Option<Vec<_>>>()?),
            HengeAnswer::Refused {
                pieces,
                bytes,
                granule,
                limits,
            } => {
                if *limits != super::dma_buf::limits() {
                    return None;
                }
                Err(super::dma_buf::Refusal::Limit {
                    pieces: *pieces,
                    bytes: *bytes,
                    granule: *granule,
                    limits: *limits,
                })
            }
        };
        let latest = self.answers.remove(at);
        self.answers.push(latest);
        Some(answer)
    }

    /// Keep `made`, the answer just made for these pages: dma-bufs, or a refusal for udmabuf's
    /// limits. Any other refusal is not kept, and drops what was kept for them.
    fn keep(
        &mut self,
        first: u64,
        length: u64,
        granule: u64,
        made: &Result<Vec<rt::DmaBufChunk>, super::dma_buf::Refusal>,
    ) {
        let answer = match made {
            Ok(chunks) => (chunks.iter())
                .map(|chunk| Some((chunk.fd.try_clone().ok()?, chunk.length)))
                .collect::<Option<Vec<_>>>()
                .map(HengeAnswer::Made),
            Err(super::dma_buf::Refusal::Limit {
                pieces,
                bytes,
                granule,
                limits,
            }) => Some(HengeAnswer::Refused {
                pieces: *pieces,
                bytes: *bytes,
                granule: *granule,
                limits: *limits,
            }),
            Err(_) => None,
        };
        let asked = (first, length, granule);
        self.answers.retain(|(pages, _)| *pages != asked);
        let Some(answer) = answer else {
            return;
        };
        if self.answers.len() == HENGE_KEPT_ANSWERS {
            self.answers.remove(0);
        }
        self.answers.push((asked, answer));
    }
}

thread_local! {
    /// The last refusal of a dma-buf source on this thread ([`host_memory`]).
'''

EDITS = {
    'crates/reims-vgpu-vulkan/src/os/linux/dma_buf.rs': [
        ('''/// udmabuf's limits as the host has them now, its defaults where it shows none.
pub(super) fn limits() -> Limits {
''', '''/// udmabuf's limits as the host has them, read at most once a second: Henge's Android port
/// (reims_dma_buf_kept.py, upstream 56c760874), each read of the two parameters costing a texture
/// over a no-copy buffer's pages about 9% of its making.
pub(super) fn limits() -> Limits {
    static KEPT: Mutex<Option<(std::time::Instant, Limits)>> = Mutex::new(None);
    let now = std::time::Instant::now();
    if let Some((read, limits)) = *KEPT.lock()
        && now.duration_since(read) < std::time::Duration::from_secs(1)
    {
        return limits;
    }
    let limits = host_limits();
    *KEPT.lock() = Some((now, limits));
    limits
}

/// udmabuf's limits as the host has them now, its defaults where it shows none.
fn host_limits() -> Limits {
'''),
        ('''    let granule = granule.max(page);
    let refused = |runs: &[Run<Arc<OwnedFd>>]| Refusal::Limit {
        pieces: runs.len() as u64,
        bytes: length,
        granule,
        limits,
    };
''', '''    let granule = granule.max(page);
    // Henge's Android port (reims_dma_buf_kept.py, upstream e8ee29fdf): runs one dma-buf takes are
    // one chunk of them all, as `padded` cuts them, made of the runs themselves.
    if !runs.is_empty() && runs.len() as u64 <= limits.pieces && length <= limits.bytes {
        return Ok(vec![DmaBufChunk {
            fd: create(device, &runs)?,
            length,
        }]);
    }
    let refused = |runs: &[Run<Arc<OwnedFd>>]| Refusal::Limit {
        pieces: runs.len() as u64,
        bytes: length,
        granule,
        limits,
    };
'''),
    ],
    'crates/reims-vgpu-vulkan/src/os/linux/import.rs': [
        ('''    let zeros = super::dma_buf::Zeros::default();
    let source: rt::DmaBufSource = Arc::new(move |request: rt::DmaBufRequest| {
''', '''    let zeros = super::dma_buf::Zeros::default();
    // Henge's Android port (reims_dma_buf_kept.py): the source's latest answers.
    let kept: Mutex<HengeKept> = Mutex::new(HengeKept::default());
    let source: rt::DmaBufSource = Arc::new(move |request: rt::DmaBufRequest| {
'''),
        ('''        match super::dma_buf::of_pages(first, length, granule, &zeros) {
            Ok(chunks) => {
''', '''        let again = kept.lock().again(first, length, granule);
        henge_note_answer(again.is_some());
        let made = match again {
            Some(answer) => answer,
            None => {
                let made = super::dma_buf::of_pages(first, length, granule, &zeros);
                kept.lock().keep(first, length, granule, &made);
                made
            }
        };
        match made {
            Ok(chunks) => {
'''),
        ('''
thread_local! {
    /// The last refusal of a dma-buf source on this thread ([`host_memory`]).
''', KEPT_TYPES),
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
                sys.exit(f'reims_dma_buf_kept: {path}: {old.splitlines()[1 if not old.splitlines()[0] else 0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_dma_buf_kept: {changed} edits applied')


if __name__ == '__main__':
    main()
