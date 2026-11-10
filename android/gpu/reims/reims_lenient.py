#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""reims-vgpu on a GPU the library never ran on (Qualcomm's Adreno): what the device lacks is an
error the guest sees, not the death of the app. Run by android_port.py on the reims-vgpu checkout.

reims-vgpu answers every refusal of metal2vulkan's (a feature the Vulkan device lacks, a path the
runtime has not built) with a death of the process, by design: on the hosts it was measured on,
Metal would have gone on, so an answer other than Metal's is never given. On the tablet every such
refusal has ended the app, one per build: the host-memory import, the framebuffer fetch, the raster
order group. Metal itself answers nil and an NSError (code 3, "the compiler refused the function")
from its pipeline creators for a function it cannot build on the device, and nil from a texture
creator for a texture it cannot make, and the guest's drivers and WindowServer handle both (a
WindowServer may abort and be restarted by launchd; the Mac and the app go on). On Android:

  - a pipeline creator (render, tile, compute) answers an `Unsupported` or `NotImplemented` refusal
    as Metal's code 3 NSError, logged with the runtime's full text so henge.log names the gap;
  - a creator with a nil answer (`answer::create`: textures, buffers, heaps, samplers...) answers
    such a refusal as nil, logged.

Each edit must match exactly once, so a changed upstream stops the build.

usage: reims_lenient.py REIMS_VGPU_DIR"""
import pathlib
import sys

EDITS = {
    'crates/reims-vgpu-vulkan/src/gpu/states.rs': [
        ("""        rt::Error::MetalError(m) => m.code,
        _ => return None,
    };
    Some(NsErrorText {
        code,
        ..error_text(e)
    })
}
""", """        rt::Error::MetalError(m) => m.code,
        // Henge's Android port (reims_lenient.py): what this device lacks is Metal's "the compiler
        // refused the function", an NSError the guest handles, not the app's death.
        rt::Error::Unsupported(_) | rt::Error::NotImplemented(_) if cfg!(target_os = "android") => {
            log::warn!("pipeline refused on this device, answered as Metal's NSError code {METAL_REFUSES}: {e}");
            METAL_REFUSES
        }
        _ => return None,
    };
    Some(NsErrorText {
        code,
        ..error_text(e)
    })
}
"""),
    ],
    'crates/reims-vgpu-vulkan/src/answer.rs': [
        ("""    stop_if_task_ended();
    match inside(Some(sel), call) {
        Ok(v) => Ok(Some(v)),
        Err(Error::Nil(_)) => Ok(None),
        Err(e) if nil.covers(&e) => Ok(None),
        Err(e) => decide(sel, policy, e),
    }
}
""", """    stop_if_task_ended();
    match inside(Some(sel), call) {
        Ok(v) => Ok(Some(v)),
        Err(Error::Nil(_)) => Ok(None),
        Err(e) if nil.covers(&e) => Ok(None),
        // Henge's Android port (reims_lenient.py): what this device lacks answers nil, as Metal
        // answers for what it cannot make, not the app's death.
        Err(e @ (Error::Unsupported(_) | Error::NotImplemented(_))) if cfg!(target_os = "android") => {
            log::warn!("{sel}: refused on this device, answered nil: {e}");
            Ok(None)
        }
        Err(e) => decide(sel, policy, e),
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
                sys.exit(f'reims_lenient: {path}: {old.splitlines()[0].strip()!r} not found once; upstream changed')
            text = text.replace(old, new)
            changed += 1
        path.write_text(text)
    print(f'reims_lenient: {changed} edits applied')


if __name__ == '__main__':
    main()
