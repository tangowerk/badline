//! Vulkan renderer of Badline (M-VK1): presents the emulator picture, the overlay quads
//! (incl. screen text) into the SDL window. The C++ side (driver/video/vulkan/) passes the raw
//! window handles and per-frame data through the C API below (header driver/video/vulkan/bvk.h).

mod chain;
mod gpu;
pub mod headless;
mod renderer;
pub mod slang;
mod timing;
mod wl;

use std::ffi::{c_char, c_int, c_uint, c_ulong, c_void};
use std::ptr;

pub use renderer::Renderer;

pub const BVK_SURFACE_WAYLAND: c_int = 1;
pub const BVK_SURFACE_XLIB: c_int = 2;

pub const BVK_TEX_ATLAS: c_int = 0;

pub const BVK_FMT_R8: c_int = 0;
pub const BVK_FMT_RGBA8: c_int = 1;

pub const BVK_OK: c_int = 0;
pub const BVK_SKIPPED: c_int = 1;
pub const BVK_NO_SHOT: c_int = 2;
pub const BVK_ERROR: c_int = -1;

/// present mode preference (bvk_set_present_mode)
pub const BVK_PRESENT_MAILBOX: c_int = 0;
pub const BVK_PRESENT_FIFO: c_int = 1;
pub const BVK_PRESENT_IMMEDIATE: c_int = 2;

/// present feedback (BvkPacingCaps::present_api)
pub const BVK_FEEDBACK_NONE: c_int = 0;
pub const BVK_FEEDBACK_WAIT: c_int = 1;
pub const BVK_FEEDBACK_TIMING: c_int = 2;

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
#[derive(Clone, Copy)]
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

    /// rendered screenshot: RGB, top-down, region in window pixels (origin top left)
    pub screenshot: *mut u8,
    pub shot_x: c_int,
    pub shot_y: c_int,
    pub shot_width: c_int,
    pub shot_height: c_int,

    /// emulator frame sequence number: a changed value marks a new picture (presented
    /// `repeat` times), 0 or the same value a redraw
    pub frame_seq: u64,

    /// 1: draw the plain picture even with a preset (hotkey, warp, screenshot without effects)
    pub shader_off: c_int,
    /// picture rotation in 90 degree steps (0..3, part of `mvp`; slang `Rotation`)
    pub rotation: c_int,
    /// emulated frame rate (slang `OriginalFPS`; the pacing grid wins when active)
    pub fps: f32,

    /// emulator picture as C64 colour indices (one byte per pixel, `frame_pitch` bytes per
    /// line), decoded on the GPU with the `bvk_set_picture` settings; used instead of `frame`
    pub indices: *const u8,
}

impl BvkFrame {
    /// a new emulator picture to upload
    pub fn has_picture(&self) -> bool {
        (!self.frame.is_null() || !self.indices.is_null()) && self.frame_width > 0 && self.frame_height > 0
    }

    /// same frame without a new picture (repeats, blend copies)
    pub fn without_picture(&self) -> Self {
        let mut f = *self;
        f.frame = std::ptr::null();
        f.indices = std::ptr::null();
        f
    }
}

/// Colour decoding of index pictures (M-VK3b), see `crates/vk/shaders/decode.frag`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct BvkPicture {
    /// 0xRRGGBB per C64 colour
    pub palette: [u32; 16],
    /// offset on the 0..255 scale (0 = neutral)
    pub brightness: f32,
    /// factors (1 = neutral)
    pub contrast: f32,
    pub saturation: f32,
    /// exponent (1 = neutral)
    pub gamma: f32,
    /// 1: C64 PAL/NTSC encoding pass (phase error, delay line, luma delay, chroma bandwidth)
    pub pal_pass: c_int,
    /// 1: NTSC (YIQ, no delay line)
    pub ntsc: c_int,
    /// parity of the first picture line in the raster (crop top & 1)
    pub first_line_odd: c_int,
    /// degrees
    pub phase_error: f32,
    /// -50..50: PAL delay line weighting of the lines (Hanover bars)
    pub hanover: f32,
    /// luma rise/fall time in pixels (<= 1: instant)
    pub luma_rise: f32,
    pub luma_fall: f32,
    /// chroma low pass taps per side (0 = off)
    pub chroma_taps: c_int,
    /// chroma cutoff in cycles per pixel (U/I and V/Q)
    pub cutoff_u: f32,
    pub cutoff_v: f32,
}

pub use timing::Record as BvkPresent;

#[repr(C)]
pub struct BvkPacingCaps {
    /// BVK_FEEDBACK_*
    pub feedback: c_int,
    /// Vulkan time domain of the display times (1 = CLOCK_MONOTONIC); 0 with BVK_FEEDBACK_WAIT
    pub time_domain: c_int,
    /// refresh duration reported by present timing (ns, 0 = unknown)
    pub refresh_ns: u64,
    /// 1: a present wait timed out and nothing was displayed since (hidden window)
    pub stalled: c_int,
    /// 1: presents can carry absolute target times (grid pacing possible)
    pub absolute_time: c_int,
    /// 1: grid presents are displayed at the grid cadence (VRR active)
    pub cadence: c_int,
    /// 1: grid without VRR, every vblank shows a phase-weighted mix (M-Pacing3)
    pub blend: c_int,
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

/// Name of the physical device (for messages), valid until bvk_destroy.
///
/// # Safety
/// `r` must come from `bvk_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_device_name(r: *const Renderer) -> *const c_char {
    unsafe { (*r).device_name().as_ptr() }
}

/// Present mode preference BVK_PRESENT_* (FIFO = vsync), applied with the next frame.
///
/// # Safety
/// `r` must come from `bvk_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_present_mode(r: *mut Renderer, mode: c_int) {
    unsafe { (*r).set_present_mode(mode) };
}

/// Pacing: every new picture is presented `repeat` times (1..8); `queue_depth` > 0 blocks present N
/// (FIFO only) until present N - queue_depth is on screen (video master); 0 = no wait.
/// `period_ns` > 0 (FIFO with absolute target times, see BvkPacingCaps::absolute_time): every new
/// picture waits for the next slot of a grid with this period and is presented with the slot as
/// target time (the grid clocks the emulation); 0 = off. `refresh_ns`: nominal display refresh
/// period (0 = unknown); with the grid and a refresh above the emulated rate the renderer blends
/// when no VRR is detected (M-Pacing3).
///
/// # Safety
/// `r` must come from `bvk_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_pacing(r: *mut Renderer, repeat: c_uint, queue_depth: c_uint, period_ns: u64, refresh_ns: u64) {
    unsafe { (*r).set_pacing(repeat, queue_depth, period_ns, refresh_ns) };
}

/// # Safety
/// `r` must come from `bvk_create`, `caps` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_pacing_caps(r: *const Renderer, caps: *mut BvkPacingCaps) {
    let r = unsafe { &*r };
    let caps = unsafe { &mut *caps };
    caps.feedback = match r.present_api() {
        renderer::PresentApi::None => BVK_FEEDBACK_NONE,
        renderer::PresentApi::V1 => BVK_FEEDBACK_WAIT,
        renderer::PresentApi::V2 if r.timing_active() => BVK_FEEDBACK_TIMING,
        renderer::PresentApi::V2 => BVK_FEEDBACK_WAIT,
    };
    caps.time_domain = if r.timing_active() { r.timing_domain() } else { 0 };
    caps.refresh_ns = r.refresh_ns();
    caps.stalled = r.stalled() as c_int;
    caps.absolute_time = r.absolute_time() as c_int;
    caps.cadence = r.grid_cadence() as c_int;
    caps.blend = r.blending() as c_int;
}

/// Moves up to `max` finished present records (oldest first) into `out`, returns the count.
///
/// # Safety
/// `r` must come from `bvk_create`, `out` must hold `max` records.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_take_presents(r: *mut Renderer, out: *mut BvkPresent, max: c_uint) -> c_uint {
    let r = unsafe { &mut *r };
    if out.is_null() || max == 0 {
        return 0;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, max as usize) };
    r.take_presents(out) as c_uint
}

// ------------------------------------------------------------ slang presets (M-VK3a)

/// A parsed and compiled preset (CPU work only), handed to `bvk_set_preset`.
pub struct BvkPreset {
    compiled: slang::Compiled,
    summary: std::ffi::CString,
}

fn json_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn json_params(params: &[slang::Param]) -> String {
    let items: Vec<String> = params
        .iter()
        .map(|p| {
            format!(
                "{{\"id\":{},\"description\":{},\"value\":{},\"default\":{},\"min\":{},\"max\":{},\"step\":{}}}",
                json_str(&p.id),
                json_str(p.description.trim()),
                p.value,
                p.initial,
                p.minimum,
                p.maximum,
                p.step
            )
        })
        .collect();
    format!("[{}]", items.join(","))
}

/// Parses and compiles a preset (`.slangp`) without a renderer (any thread). `overrides`:
/// saved parameter values "id=value;id=value" (null or empty = none). Null on error.
///
/// # Safety
/// `path` must be a NUL terminated string, `overrides` too or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_preset_load(
    path: *const c_char,
    overrides: *const c_char,
    error: *mut c_char,
    error_len: usize,
) -> *mut BvkPreset {
    let path = unsafe { std::ffi::CStr::from_ptr(path) }.to_string_lossy().into_owned();
    let overrides: Vec<(String, f32)> = if overrides.is_null() {
        Vec::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(overrides) }
            .to_string_lossy()
            .split(';')
            .filter_map(|kv| {
                let (k, v) = kv.split_once('=')?;
                Some((k.trim().to_string(), v.trim().parse().ok()?))
            })
            .collect()
    };
    let t0 = std::time::Instant::now();
    match slang::compile(std::path::Path::new(&path), &overrides) {
        Ok(compiled) => {
            let millis = t0.elapsed().as_millis();
            let warnings: Vec<String> = compiled.warnings.iter().map(|w| json_str(w)).collect();
            let summary = format!(
                "{{\"path\":{},\"passes\":{},\"luts\":{},\"history\":{},\"feedback_passes\":{},\"compiled_stages\":{},\
                 \"cached_stages\":{},\"load_ms\":{},\"warnings\":[{}],\"parameters\":{}}}",
                json_str(&path),
                compiled.passes.len(),
                compiled.luts.len(),
                compiled.history,
                compiled.feedback.iter().filter(|f| **f).count(),
                compiled.compiled,
                compiled.cached,
                millis,
                warnings.join(","),
                json_params(&compiled.params)
            );
            Box::into_raw(Box::new(BvkPreset {
                compiled,
                summary: std::ffi::CString::new(summary).unwrap_or_default(),
            }))
        }
        Err(e) => {
            write_error(&e, error, error_len);
            ptr::null_mut()
        }
    }
}

/// JSON summary of a loaded preset (passes, parameters, compile statistics), valid until the
/// preset is freed or handed to `bvk_set_preset`.
///
/// # Safety
/// `p` must come from `bvk_preset_load`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_preset_summary(p: *const BvkPreset) -> *const c_char {
    unsafe { (*p).summary.as_ptr() }
}

/// # Safety
/// `p` must come from `bvk_preset_load` (or be null) and not have been handed to `bvk_set_preset`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_preset_free(p: *mut BvkPreset) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}

/// Activates a preset (takes ownership, also on error); null removes it. Builds pipelines and
/// LUT textures (waits for the device).
///
/// # Safety
/// `r` must come from `bvk_create`, `p` from `bvk_preset_load` or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_preset(r: *mut Renderer, p: *mut BvkPreset, error: *mut c_char, error_len: usize) -> c_int {
    let r = unsafe { &mut *r };
    let compiled = if p.is_null() {
        None
    } else {
        Some(unsafe { Box::from_raw(p) }.compiled)
    };
    match r.set_preset(compiled) {
        Ok(()) => BVK_OK,
        Err(e) => {
            write_error(&e, error, error_len);
            BVK_ERROR
        }
    }
}

/// Colour decoding settings for index pictures (`BvkFrame::indices`), applied from the next
/// uploaded picture on.
///
/// # Safety
/// `r` must come from `bvk_create`, `picture` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_picture(r: *mut Renderer, picture: *const BvkPicture) {
    unsafe { (*r).set_picture(*picture) };
}

/// Sets a parameter of the active preset; BVK_ERROR when there is no such parameter.
///
/// # Safety
/// `r` must come from `bvk_create`, `id` must be a NUL terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_set_param(r: *mut Renderer, id: *const c_char, value: f32) -> c_int {
    let r = unsafe { &mut *r };
    let id = unsafe { std::ffi::CStr::from_ptr(id) }.to_string_lossy();
    if r.set_param(&id, value) { BVK_OK } else { BVK_ERROR }
}

/// JSON of the active preset (`null` without one): path, passes, parameters with values.
/// Writes at most `len` bytes (NUL terminated) and returns the full length.
///
/// # Safety
/// `r` must come from `bvk_create`, `buf` must hold `len` bytes (or be null with len 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bvk_preset_state(r: *const Renderer, buf: *mut c_char, len: usize) -> usize {
    let r = unsafe { &*r };
    let text = match r.chain() {
        None => "null".to_string(),
        Some(c) => format!(
            "{{\"path\":{},\"passes\":{},\"parameters\":{}}}",
            json_str(&c.path.display().to_string()),
            c.pass_count(),
            json_params(&c.params)
        ),
    };
    write_error(&text, buf, len);
    text.len()
}
