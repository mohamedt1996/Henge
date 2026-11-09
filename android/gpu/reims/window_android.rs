//! The window on Android (Henge's port): none. The app draws the display's frames itself, through a
//! frame target (`rvg_frame_target_*`), into its own Vulkan surface, so a display never asks for a
//! window here; this is the seam the shared window code needs, answering that there is none.

use std::sync::Arc;

use reims_vgpu_core::platform::os::{Clock as _, MonoTime};
use reims_vgpu_vulkan::os::OsClock;

use crate::handle::Window;
use crate::present::{Presenter, VulkanTarget};
use crate::screens::HostScreen;

/// What draws into a window's swapchain: as on Linux.
pub trait FrameSink: Send + Sync {
    /// A presenter over `target`'s window; never asked for here, where no window opens.
    fn presenter(&self, target: &dyn VulkanTarget) -> Option<Box<dyn Presenter>>;

    /// The guest's cursor position, as on Linux.
    fn guest_cursor_position(&self) -> Option<(u16, u16)> {
        None
    }
}

/// A window's surface: nothing to draw into.
#[derive(Debug, Default)]
pub(crate) struct Surface;

impl Surface {
    pub(crate) fn new() -> Surface {
        Surface
    }

    /// The guest presented a frame: there is no window to draw it in.
    pub(crate) fn request_draw(&self) {}

    /// The window's end.
    pub(crate) fn close(&self) {}
}

/// A display created with a window config gets none on Android: it is logged, and the display
/// works as one without a window.
pub(crate) fn open(window: &Arc<Window>) {
    let _ = window;
    log::warn!("reims-vgpu-window: no windows on Android; the host draws through a frame target");
}

pub(crate) fn guest_mode_changed(_id: u64) {}

pub(crate) fn cursor_changed(_id: u64) {}

pub(crate) fn close(_id: u64) {}

/// Now, on the monotonic clock the library's platform reads (`OsClock`).
pub(crate) fn now() -> MonoTime {
    OsClock::new().now_mono()
}

/// No host screens to match.
pub(crate) fn host_screens() -> Vec<HostScreen> {
    Vec::new()
}
