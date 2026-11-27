//! Wayland surface hints next to the Vulkan WSI (M-Pacing2): marks the SDL window surface as
//! content type "game" (wp_content_type_v1). Neither Mesa nor SDL set it; Hyprland uses it for
//! `misc:vrr = 3` and to keep cursor frames from breaking fullscreen VRR.
//!
//! Runs on its own event queue on SDL's wl_display (libwayland-client through wayland-backend's
//! `client_system`). The tearing-control hint is left to Mesa (one object per surface).

use std::ffi::c_void;

use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{wl_registry, wl_surface::WlSurface};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::content_type::v1::client::wp_content_type_manager_v1::WpContentTypeManagerV1;
use wayland_protocols::wp::content_type::v1::client::wp_content_type_v1::{Type, WpContentTypeV1};

struct State;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(_: &mut Self, _: &wl_registry::WlRegistry, _: wl_registry::Event, _: &GlobalListContents, _: &Connection, _: &QueueHandle<Self>) {}
}

impl Dispatch<WpContentTypeManagerV1, ()> for State {
    fn event(_: &mut Self, _: &WpContentTypeManagerV1, _: <WpContentTypeManagerV1 as Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
}

impl Dispatch<WpContentTypeV1, ()> for State {
    fn event(_: &mut Self, _: &WpContentTypeV1, _: <WpContentTypeV1 as Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
}

/// content type object of the window surface; destroyed before the surface (renderer drop)
pub struct ContentType {
    conn: Connection,
    object: WpContentTypeV1,
}

impl ContentType {
    /// None when the compositor lacks the protocol or the handles are unusable
    ///
    /// # Safety
    /// `display` and `surface` must be the live wl_display / wl_surface of the window.
    pub unsafe fn game(display: *mut c_void, surface: *mut c_void) -> Option<Self> {
        if display.is_null() || surface.is_null() {
            return None;
        }
        let backend = unsafe { Backend::from_foreign_display(display.cast()) };
        let conn = Connection::from_backend(backend);
        let (globals, mut queue) = registry_queue_init::<State>(&conn).ok()?;
        let qh = queue.handle();
        let manager: WpContentTypeManagerV1 = globals.bind(&qh, 1..=1, ()).ok()?;
        let id = unsafe { ObjectId::from_ptr(WlSurface::interface(), surface.cast()) }.ok()?;
        let wl_surface = WlSurface::from_id(&conn, id).ok()?;
        let object = manager.get_surface_content_type(&wl_surface, &qh, ());
        // double-buffered: applied with the next commit (the next present)
        object.set_content_type(Type::Game);
        manager.destroy();
        let _ = queue.roundtrip(&mut State);
        Some(ContentType { conn, object })
    }
}

impl Drop for ContentType {
    fn drop(&mut self) {
        self.object.destroy();
        let _ = self.conn.flush();
    }
}
