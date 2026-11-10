#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""metal2vulkan on a Vulkan driver of the app's own (Turnip), loaded directly as the ICD. Run by
android_port.py on the metal2vulkan checkout.

The runtime opens the platform's Vulkan loader by name (`libvulkan.so` on Android), which hands it
the system's driver: Qualcomm's, on the tablet. To run on Turnip (Mesa's open Adreno driver, which
the APK may bundle as `libvulkan_freedreno.so`), the runtime opens the library `M2V_VULKAN_LIBRARY`
names instead, and where that library is a driver rather than a loader (it exports the ICD's
`vk_icdGetInstanceProcAddr` and no `vkGetInstanceProcAddr`), it is used through that entry point
directly: an ICD's `vkGetInstanceProcAddr` answers the global functions for a null instance as the
loader's does, and the runtime presents no window, so it needs none of the loader's WSI glue.
The library is leaked on purpose: its functions are called for the rest of the process.

Each edit must match exactly once, so a changed upstream stops the build.

usage: m2v_icd_library.py METAL2VULKAN_DIR"""
import pathlib
import sys

EDITS = {
    'crates/metal2vulkan-runtime/src/device/create.rs': [
        ("""pub(crate) fn open_loader(loader: Option<&Path>) -> Result<ash::Entry, Error> {
    let tried = match loader {
        Some(path) => vec![path.to_path_buf()],
        None => default_loaders(std::env::var_os("VULKAN_SDK").as_deref().map(Path::new)),
    };
    for path in &tried {
""", """pub(crate) fn open_loader(loader: Option<&Path>) -> Result<ash::Entry, Error> {
    // Henge's Android port (m2v_icd_library.py): M2V_VULKAN_LIBRARY names the library to open, a
    // loader or a driver (an ICD, through its `vk_icdGetInstanceProcAddr`).
    if let Some(own) = std::env::var_os("M2V_VULKAN_LIBRARY").filter(|v| !v.is_empty()) {
        let path = PathBuf::from(own);
        // SAFETY: as below.
        if let Ok(entry) = unsafe { ash::Entry::load_from(&path) } {
            return Ok(entry);
        }
        // SAFETY: the library is a Vulkan ICD the host chose; its entry point follows the loader-ICD
        // interface, and the library stays loaded for the rest of the process.
        if let Some(entry) = unsafe { open_icd(&path) } {
            return Ok(entry);
        }
        return Err(Error::LoaderNotFound(LoaderNotFound { tried: vec![path] }));
    }
    let tried = match loader {
        Some(path) => vec![path.to_path_buf()],
        None => default_loaders(std::env::var_os("VULKAN_SDK").as_deref().map(Path::new)),
    };
    for path in &tried {
"""),
        ("""    Err(Error::LoaderNotFound(LoaderNotFound { tried }))
}
""", """    Err(Error::LoaderNotFound(LoaderNotFound { tried }))
}

/// Henge's Android port (m2v_icd_library.py): the library at `path` opened as a Vulkan ICD, through
/// its `vk_icdGetInstanceProcAddr`; `None` where it does not open or exports no such symbol.
///
/// # Safety
///
/// `path` is a Vulkan driver whose entry point follows the loader-ICD interface.
#[cfg(unix)]
unsafe fn open_icd(path: &Path) -> Option<ash::Entry> {
    use std::os::unix::ffi::OsStrExt;
    let c_path = std::ffi::CString::new(path.as_os_str().as_bytes()).ok()?;
    // SAFETY: a NUL-terminated path; the handle is kept for the rest of the process.
    let handle = unsafe { libc::dlopen(c_path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
    if handle.is_null() {
        // SAFETY: dlerror answers a static or thread-local string, read at once.
        let why = unsafe { libc::dlerror() };
        let why = if why.is_null() {
            String::new()
        } else {
            // SAFETY: a NUL-terminated string dlerror owns.
            unsafe { std::ffi::CStr::from_ptr(why) }.to_string_lossy().into_owned()
        };
        eprintln!("metal2vulkan: M2V_VULKAN_LIBRARY {}: dlopen failed: {why}", path.display());
        return None;
    }
    // SAFETY: the handle is live; the symbol name is NUL-terminated.
    let symbol = unsafe { libc::dlsym(handle, c"vk_icdGetInstanceProcAddr".as_ptr()) };
    if symbol.is_null() {
        eprintln!(
            "metal2vulkan: M2V_VULKAN_LIBRARY {}: neither vkGetInstanceProcAddr nor vk_icdGetInstanceProcAddr",
            path.display()
        );
        return None;
    }
    eprintln!("metal2vulkan: Vulkan driver {} opened directly as an ICD", path.display());
    // SAFETY: the symbol is the ICD's `vk_icdGetInstanceProcAddr`, of `vkGetInstanceProcAddr`'s type.
    let get_instance_proc_addr: ash::vk::PFN_vkGetInstanceProcAddr = unsafe { std::mem::transmute(symbol) };
    let static_fn = ash::StaticFn { get_instance_proc_addr };
    // SAFETY: the function pointer is the driver's, valid while the library is loaded, which is for
    // the rest of the process (the handle is never closed).
    Some(unsafe { ash::Entry::from_static_fn(static_fn) })
}

#[cfg(not(unix))]
unsafe fn open_icd(_path: &Path) -> Option<ash::Entry> {
    None
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
                sys.exit(f'm2v_icd_library: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'm2v_icd_library: {changed} edits applied')


if __name__ == '__main__':
    main()
