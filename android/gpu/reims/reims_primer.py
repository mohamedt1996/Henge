#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The shader primer: metal2vulkan's own pipeline records, kept across runs and warmed at the next device.
Run by android_port.py on the reims-vgpu checkout (0.3.59).

metal2vulkan can hand every pipeline creation the program makes to a hook as a record
(`DeviceOptions::pipeline_records`: the descriptor, never a translation, so a record outlives a
translator or driver update), and make them again in the background on a later device
(`Device::warm`), its translations into the shader cache and its VkPipelines into the driver's cache,
so the program's own creations find them. reims-vgpu wires neither. The shader cache alone keeps the
translations, but every VkPipeline is made again by the driver at its first use in each run (Turnip's
own cache is per process), and after a metal2vulkan or Turnip update the translations go too: the
first open of each window and effect then waits on them (the log's "pipeline" waits; past 4.5 s the
runtime drops the draws).

  - the hook appends each new record (length, bytes) to $HOME/.cache/henge-pipelines.bin, at most
    32 MiB, skipping records it holds already;
  - the first device reims-vgpu opens is handed the file's records in a warm pass, with a 256 MiB
    budget of driver cache data; HENGE_PRIMER=0 turns both off;
  - the warm pass's counts go to the log after 60 s ("metal2vulkan: (Henge) primer: ...").

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_primer.py REIMS_VGPU_DIR"""
import pathlib
import sys

PRIMER_MOD = '''
/// Henge (reims_primer.py): metal2vulkan's pipeline records kept in a file across runs and warmed on
/// the first device of the next.
pub(crate) mod henge_primer {
    use metal2vulkan_runtime as rt;
    use std::collections::HashSet;
    use std::io::{Read, Write};
    use std::path::PathBuf;
    use std::sync::{Mutex, OnceLock};

    const MAX_FILE: u64 = 32 << 20;
    const WARM_BUDGET: u64 = 256 << 20;

    fn on() -> bool {
        std::env::var("HENGE_PRIMER").map(|v| v != "0").unwrap_or(true)
    }

    fn file() -> Option<PathBuf> {
        std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache/henge-pipelines.bin"))
    }

    fn key(bytes: &[u8]) -> u64 {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        for b in bytes {
            h = (h ^ u64::from(*b)).wrapping_mul(0x100_0000_01b3);
        }
        h
    }

    struct Kept {
        seen: HashSet<u64>,
        size: u64,
    }

    /// The records the file holds, read once: (their bytes, what the hook skips).
    fn read() -> &'static Mutex<(Vec<Vec<u8>>, Kept)> {
        static READ: OnceLock<Mutex<(Vec<Vec<u8>>, Kept)>> = OnceLock::new();
        READ.get_or_init(|| {
            let mut records = Vec::new();
            let mut kept = Kept { seen: HashSet::new(), size: 0 };
            if let Some(path) = file() {
                let mut all = Vec::new();
                if std::fs::File::open(&path).and_then(|mut f| f.read_to_end(&mut all)).is_ok() {
                    kept.size = all.len() as u64;
                    let mut at = 0usize;
                    while at + 4 <= all.len() {
                        let n = u32::from_le_bytes([all[at], all[at + 1], all[at + 2], all[at + 3]]) as usize;
                        at += 4;
                        if n == 0 || at + n > all.len() {
                            break;
                        }
                        let bytes = all[at..at + n].to_vec();
                        at += n;
                        if kept.seen.insert(key(&bytes)) {
                            records.push(bytes);
                        }
                    }
                }
            }
            Mutex::new((records, kept))
        })
    }

    /// The hook: a record the file does not hold yet, appended.
    pub(crate) fn hook() -> Option<std::sync::Arc<dyn Fn(rt::PipelineRecord) + Send + Sync>> {
        if !on() {
            return None;
        }
        let path = file()?;
        Some(std::sync::Arc::new(move |record: rt::PipelineRecord| {
            let bytes = record.to_bytes();
            if bytes.is_empty() || bytes.len() > (1 << 20) {
                return;
            }
            let mut guard = match read().lock() {
                Ok(g) => g,
                Err(_) => return,
            };
            let kept = &mut guard.1;
            let k = key(&bytes);
            if kept.seen.contains(&k) || kept.size + 4 + bytes.len() as u64 > MAX_FILE {
                return;
            }
            let mut f = match std::fs::OpenOptions::new().create(true).append(true).open(&path) {
                Ok(f) => f,
                Err(_) => return,
            };
            let mut out = Vec::with_capacity(4 + bytes.len());
            out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
            out.extend_from_slice(&bytes);
            if f.write_all(&out).is_ok() {
                kept.seen.insert(k);
                kept.size += out.len() as u64;
            }
        }))
    }

    /// The first device opened: the file's records warmed on it, once per process.
    pub(crate) fn warm(device: &rt::Device) {
        static DONE: OnceLock<()> = OnceLock::new();
        if !on() || DONE.set(()).is_err() {
            return;
        }
        let records: Vec<rt::PipelineRecord> = match read().lock() {
            Ok(g) => g.0.iter().filter_map(|b| rt::PipelineRecord::from_bytes(b)).collect(),
            Err(_) => return,
        };
        let n = records.len();
        eprintln!("metal2vulkan: (Henge) primer: warming {n} pipeline records from earlier runs");
        if n == 0 {
            return;
        }
        device.warm(records, WARM_BUDGET);
        let device = device.clone();
        let _ = std::thread::Builder::new().name("henge-primer-log".into()).spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(60));
            eprintln!("metal2vulkan: (Henge) primer after 60 s: {:?}", device.warm_statistics());
        });
    }
}
'''

EDITS = {
    'crates/reims-vgpu-vulkan/src/platform.rs': [
        ('''        options.shader_cache.capacity = SHADER_CACHE_CAPACITY;
        options
    });
    &OPTIONS
}''',
         '''        options.shader_cache.capacity = SHADER_CACHE_CAPACITY;
        // Henge (reims_primer.py): every pipeline creation recorded for the next run's warm pass.
        options.pipeline_records = henge_primer::hook();
        options
    });
    &OPTIONS
}
''' + PRIMER_MOD),
        ('''                made_devices(OPEN_DEVICE, || {
                    let default = rt::Device::system_default_with(device_options());
                    Ok(default.ok().into_iter().collect())
                })''',
         '''                made_devices(OPEN_DEVICE, || {
                    let default = rt::Device::system_default_with(device_options());
                    if let Ok(device) = &default {
                        henge_primer::warm(device); // Henge (reims_primer.py)
                    }
                    Ok(default.ok().into_iter().collect())
                })'''),
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
                sys.exit(f'reims_primer: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_primer: {changed} edits applied')


if __name__ == '__main__':
    main()
