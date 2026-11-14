#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The shader cache directory made optional, as macOS-uke's metal2vulkan has it
(METAL2VULKAN_SHADER_CACHE_OPTIONAL, gpu/plugin/0001-macos-windowserver.patch there).
Run by android_port.py on the metal2vulkan checkout.

Upstream, a device is refused unless its cache directory can be made and takes a probe write
(`Directory::open`, shader_cache/store.rs): "the Vulkan runtime refused it", a reims-vgpu fatal that
ends the app's process and with it the Mac. On a tablet that is a full storage (the Mac's disk fills
it as macOS writes) or a folder Android or a cleaner app took away. With
METAL2VULKAN_SHADER_CACHE_OPTIONAL set (host.c sets it), such a cache costs speed, not the run:

- a folder that cannot be made: METAL2VULKAN_SHADER_CACHE_FALLBACK is tried instead (host.c gives the
  app's cache folder), once;
- a folder that exists but refuses the probe (storage full): taken anyway. The runtime already treats
  every later failed read or write as a miss it counts, never an error (shader_cache/mod.rs), so the
  earlier boots' entries are still found and new ones are compiled again each boot until there is room.

Each is said on stderr, which host.c sends to henge.log.
"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/shader_cache/store.rs': [
        ("""        create_private_dir_all(&directory.root).map_err(failed(&directory.root))?;
        let build_dir = directory.build_dir();
        create_private_dir_all(&build_dir).map_err(failed(&build_dir))?;
        let probe = build_dir.join(format!(".probe.{}.tmp", unique()));
        let written = new_private_file(&probe).and_then(|mut file| file.write_all(b"probe"));
        let removed = fs::remove_file(&probe);
        written.and(removed).map_err(failed(&directory.root))?;
        Ok(directory)""",
         """        // Henge (m2v_cache_optional.py): with METAL2VULKAN_SHADER_CACHE_OPTIONAL, a cache that cannot be
        // made tries the fallback folder once, and one that refuses the probe is taken anyway.
        let optional = std::env::var_os("METAL2VULKAN_SHADER_CACHE_OPTIONAL").is_some();
        let build_dir = directory.build_dir();
        let made = create_private_dir_all(&directory.root)
            .map_err(failed(&directory.root))
            .and_then(|()| create_private_dir_all(&build_dir).map_err(failed(&build_dir)));
        if let Err(not_made) = made {
            let fallback = std::env::var_os("METAL2VULKAN_SHADER_CACHE_FALLBACK").map(PathBuf::from);
            match fallback {
                Some(fallback) if optional && fallback != directory.root => {
                    eprintln!(
                        "metal2vulkan: shader cache {}: {}; the fallback {} instead",
                        not_made.path.display(),
                        not_made.error,
                        fallback.display()
                    );
                    return Directory::open(fallback, build, capacity);
                }
                _ => return Err(not_made),
            }
        }
        let probe = build_dir.join(format!(".probe.{}.tmp", unique()));
        let written = new_private_file(&probe).and_then(|mut file| file.write_all(b"probe"));
        let removed = fs::remove_file(&probe);
        if let Err(error) = written.and(removed) {
            if !optional {
                return Err(Failed { path: directory.root.clone(), error });
            }
            eprintln!(
                "metal2vulkan: shader cache {} takes no write ({}); kept for its earlier entries, \\
                 new ones compiled again each run until it does",
                directory.root.display(),
                error
            );
        }
        Ok(directory)"""),
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
                sys.exit(f'm2v_cache_optional: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_cache_optional: {changed} edits applied')


if __name__ == '__main__':
    main()
