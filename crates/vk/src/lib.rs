//! Vulkan renderer of Badline (M-VK1): presents the emulator picture, the overlay quads and
//! the screen text into the SDL window. The C++ side (driver/video/vulkan/) passes the raw
//! window handles and per-frame data through the C API below (header driver/video/vulkan/bvk.h).

mod renderer;

use std::ffi::{c_char, c_int, c_uint, c_ulong, c_void};
use std::ptr;

pub use renderer::Renderer;

pub const BVK_SURFACE_WAYLAND: c_int = 1;
pub const BVK_SURFACE_XLIB: c_int = 2;

pub const BVK_TEX_ATLAS: c_int = 0;
pub const BVK_TEX_TEXT: c_int = 1;

pub const BVK_FMT_R8: c_int = 0;
pub const BVK_FMT_RGBA8: c_int = 1;

pub const BVK_OK: c_int = 0;
pub const BVK_SKIPPED: c_int = 1;
pub const BVK_NO_SHOT: c_int = 2;
pub const BVK_ERROR: c_int = -1;

#[repr(C)]
pub struct BvkCreateInfo {
    pub kind: c_int,
    pub display: *mut c_void,
    /// wl_surface* (Wayland)
    pub surface: *mut c_void,
    /// X11 Window (xlib)
    pub window: c_ulong,
}

#[repr(C)]
pub struct BvkFrame {
    /// pixel size of the window surface
    pub window_width: c_uint,
    pub window_height: c_uint,

    /// emulator picture, BGRA in memory (u32 0xAARRGGBB); null = keep the last uploaded one
    pub frame: *const u32,
    pub frame_width: c_uint,
    pub frame_height: c_uint,
    /// line pitch in pixels
    pub frame_pitch: c_uint,
    /// 0: window cleared only (no picture)
    pub show_frame: c_int,
    /// picture area in window pixels, origin top left
    pub vp_x: c_int,
    pub vp_y: c_int,
    pub vp_width: c_int,
    pub vp_height: c_int,
    /// GL style MVP for the unit quad (column major)
    pub mvp: [f32; 16],
    pub linear: c_int,

    /// overlay: 8 floats per vertex (x, y, u, v, r, g, b, a), window pixels of overlay_width/height
    pub overlay_vertices: *const f32,
    pub overlay_vertex_count: c_uint,
    pub overlay_width: f32,
    pub overlay_height: f32,

    /// screen text (texture BVK_TEX_TEXT), quad corners in GL NDC of the picture area
    pub text_visible: c_int,
    pub text_quad: [f32; 16],
    pub text_color: [f32; 4],
    pub text_bg_color: [f32; 4],

    /// rendered screenshot: RGB, top-down, region in window pixels (origin top left)
    pub screenshot: *mut u8,
    pub shot_x: c_int,
    pub shot_y: c_int,
    pub shot_width: c_int,
    pub shot_height: c_int,
}

fn write_error(msg: &str, buf: *mut c_char, len: usize) {
    if buf.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(len - 1);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
}

/// Creates instance, surface, device and pipelines. Returns null on error (message in `error`).
///
/// # Safety
/// `info` must point to valid handles of a live SDL window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_create(info: *const BvkCreateInfo, error: *mut c_char, error_len: usize) -> *mut Renderer {
    let info = unsafe { &*info };
    match Renderer::new(info) {
        Ok(r) => Box::into_raw(Box::new(r)),
        Err(e) => {
            write_error(&e, error, error_len);
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `r` must come from `bvk_create` (or be null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_destroy(r: *mut Renderer) {
    if !r.is_null() {
        drop(unsafe { Box::from_raw(r) });
    }
}

/// Queues a texture slot upload (tightly packed rows, copied), applied with the next frame.
/// Width/height 0 removes it.
///
/// # Safety
/// `data` must hold width * height * bytes-per-pixel bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_texture(
    r: *mut Renderer,
    slot: c_int,
    format: c_int,
    width: c_uint,
    height: c_uint,
    data: *const c_void,
    error: *mut c_char,
    error_len: usize,
) -> c_int {
    let r = unsafe { &mut *r };
    let bpp = if format == BVK_FMT_R8 { 1 } else { 4 };
    let data = if data.is_null() || width == 0 || height == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(data as *const u8, width as usize * height as usize * bpp as usize) }
    };
    match r.set_texture(slot, format, width, height, data) {
        Ok(()) => BVK_OK,
        Err(e) => {
            write_error(&e, error, error_len);
            BVK_ERROR
        }
    }
}

/// Renders and presents one frame. BVK_SKIPPED: nothing presented (zero size window,
/// swapchain out of date, acquire timeout); BVK_NO_SHOT: presented, the requested screenshot
/// region was outside the swapchain (buffer untouched); BVK_ERROR: device error (message in `error`).
///
/// # Safety
/// All pointers in `frame` must be valid for the given sizes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_render(r: *mut Renderer, frame: *const BvkFrame, error: *mut c_char, error_len: usize) -> c_int {
    let r = unsafe { &mut *r };
    match unsafe { r.render(&*frame) } {
        Ok(renderer::Rendered::Presented | renderer::Rendered::WithScreenshot) => BVK_OK,
        Ok(renderer::Rendered::NoScreenshot) => BVK_NO_SHOT,
        Ok(renderer::Rendered::Skipped) => BVK_SKIPPED,
        Err(e) => {
            write_error(&e, error, error_len);
            BVK_ERROR
        }
    }
}

/// VSync on: FIFO; off: MAILBOX or IMMEDIATE (warp, `video_sync` off). Applied with the next frame.
///
/// # Safety
/// `r` must come from `bvk_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_vsync(r: *mut Renderer, on: c_int) {
    unsafe { (*r).set_vsync(on != 0) };
}

/// Name of the physical device (for messages), valid until bvk_destroy.
///
/// # Safety
/// `r` must come from `bvk_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_device_name(r: *const Renderer) -> *const c_char {
    unsafe { (*r).device_name().as_ptr() }
}
