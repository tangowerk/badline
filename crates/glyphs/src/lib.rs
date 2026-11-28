//! Glyph atlas of the overlay (M-VK2): C64 Pro and C64 Pro Mono rasterized once at their
//! native 8 px grid (1 bit, pixel exact) into an R8 atlas that the overlay painter scales by
//! integer factors. The C++ side (program/overlay/font.cpp) calls the C API below
//! (header program/overlay/glyphs.h, kept in sync by hand).

mod raster;

use std::ffi::{CStr, c_char, c_int};
use std::path::Path;

pub const FACE_FILES: [&str; 2] = ["C64_Pro-STYLE121.ttf", "C64_Pro_Mono-STYLE121.ttf"];
/// logical pixels per em / text line
pub const SIZE: u32 = 8;
/// 8 px glyph + padding against bleeding
const CELL: u32 = 10;
const ATLAS_WIDTH: u32 = 512;
/// C64 Pro Mono also gets the screen code glyphs (U+EE00 + code, upper case/graphics set)
const SCREEN_CODES: u32 = 256;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BglGlyph {
    pub face: u32,
    pub codepoint: u32,
    pub advance: i32,
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
    /// top left texel in the atlas
    pub x: u32,
    pub y: u32,
}

#[repr(C)]
pub struct BglAtlas {
    pub pixels: *mut u8,
    pub width: u32,
    pub height: u32,
    pub glyphs: *mut BglGlyph,
    pub glyph_count: u32,
    /// a fully set 4x4 block at (1,1) for solid rectangles
    pub white_x: u32,
    pub white_y: u32,
}

pub struct Atlas {
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub glyphs: Vec<BglGlyph>,
}

/// ASCII, Latin-1 and a few symbols used by the UI (arrows, blocks)
fn charset() -> Vec<u32> {
    let mut out: Vec<u32> = (32..127).chain(160..256).collect();
    out.extend_from_slice(&[
        0x2190, 0x2191, 0x2192, 0x2193, 0x2022, 0x2026, 0x2500, 0x2502, 0x2588, 0x2591, 0x2592,
        0x25cf, 0x25cb, 0x2713,
    ]);
    out
}

pub fn build(folder: &Path) -> Result<Atlas, String> {
    let codes = charset();
    let per_row = ATLAS_WIDTH / CELL;
    // cell 0 is the white block
    let cells = 1 + 2 * codes.len() as u32 + SCREEN_CODES;
    let width = ATLAS_WIDTH;
    let mut height = 16;
    while height < cells.div_ceil(per_row) * CELL {
        height <<= 1;
    }
    let mut pixels = vec![0u8; (width * height) as usize];
    for y in 0..4 {
        for x in 0..4 {
            pixels[((1 + y) * width + 1 + x) as usize] = 255;
        }
    }

    let mut glyphs = Vec::new();
    let mut cell = 1;
    for (f, file) in FACE_FILES.iter().enumerate() {
        let path = folder.join(file);
        let data =
            std::fs::read(&path).map_err(|_| format!("cannot load font {}", path.display()))?;
        let face = ttf_parser::Face::parse(&data, 0)
            .map_err(|e| format!("cannot load font {}: {e}", path.display()))?;
        let scale = SIZE as f64 / face.units_per_em() as f64;

        let mut face_codes = codes.clone();
        if f == 1 {
            face_codes.extend((0..SCREEN_CODES).map(|sc| 0xee00 + sc));
        }
        for cp in face_codes {
            let Some(id) = char::from_u32(cp).and_then(|c| face.glyph_index(c)) else {
                if cp != ' ' as u32 {
                    continue;
                }
                // a missing space still advances (glyph 0 metrics)
                glyphs.push(place(
                    &mut pixels,
                    width,
                    per_row,
                    &mut cell,
                    f,
                    cp,
                    &face,
                    ttf_parser::GlyphId(0),
                    scale,
                    None,
                ));
                continue;
            };
            let mut outline = raster::Outline::new(scale);
            face.outline_glyph(id, &mut outline);
            // like FreeType: an empty outline still yields a blank 1x1 bitmap (collapsed box)
            let bitmap = if cp == ' ' as u32 {
                None
            } else {
                Some(outline.render().unwrap_or(raster::Bitmap {
                    left: 0,
                    top: 1,
                    width: 1,
                    height: 1,
                    pixels: vec![false],
                }))
            };
            glyphs.push(place(
                &mut pixels,
                width,
                per_row,
                &mut cell,
                f,
                cp,
                &face,
                id,
                scale,
                bitmap,
            ));
        }
    }
    Ok(Atlas {
        pixels,
        width,
        height,
        glyphs,
    })
}

#[allow(clippy::too_many_arguments)]
fn place(
    pixels: &mut [u8],
    width: u32,
    per_row: u32,
    cell: &mut u32,
    face_index: usize,
    cp: u32,
    face: &ttf_parser::Face,
    id: ttf_parser::GlyphId,
    scale: f64,
    bitmap: Option<raster::Bitmap>,
) -> BglGlyph {
    let advance = (face.glyph_hor_advance(id).unwrap_or(0) as f64 * scale).round() as i32;
    let (cx, cy) = ((*cell % per_row) * CELL, (*cell / per_row) * CELL);
    *cell += 1;
    let mut g = BglGlyph {
        face: face_index as u32,
        codepoint: cp,
        advance,
        x: cx,
        y: cy,
        ..Default::default()
    };
    if let Some(b) = bitmap {
        g.left = b.left;
        g.top = b.top;
        g.width = b.width.min(CELL as i32 - 1);
        g.height = b.height.min(CELL as i32 - 1);
        for y in 0..g.height {
            for x in 0..g.width {
                if b.pixels[(y * b.width + x) as usize] {
                    pixels[((cy + y as u32) * width + cx + x as u32) as usize] = 255;
                }
            }
        }
    }
    g
}

/// Builds the atlas from the two C64 Pro fonts in `folder` (path ends with '/').
/// Returns 0 on success; on failure writes a NUL-terminated message into `error`.
///
/// # Safety
/// `folder` must be a valid C string, `out` writable, `error` writable for `error_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bgl_atlas_build(
    folder: *const c_char,
    out: *mut BglAtlas,
    error: *mut c_char,
    error_len: usize,
) -> c_int {
    let folder = unsafe { CStr::from_ptr(folder) }
        .to_string_lossy()
        .into_owned();
    match build(Path::new(&folder)) {
        Ok(atlas) => {
            let mut pixels = atlas.pixels.into_boxed_slice();
            let mut glyphs = atlas.glyphs.into_boxed_slice();
            let o = unsafe { &mut *out };
            o.width = atlas.width;
            o.height = atlas.height;
            o.glyph_count = glyphs.len() as u32;
            o.pixels = pixels.as_mut_ptr();
            o.glyphs = glyphs.as_mut_ptr();
            o.white_x = 1;
            o.white_y = 1;
            std::mem::forget(pixels);
            std::mem::forget(glyphs);
            0
        }
        Err(msg) => {
            if !error.is_null() && error_len > 0 {
                let n = msg.len().min(error_len - 1);
                unsafe {
                    std::ptr::copy_nonoverlapping(msg.as_ptr(), error as *mut u8, n);
                    *error.add(n) = 0;
                }
            }
            -1
        }
    }
}

/// Frees what `bgl_atlas_build` allocated.
///
/// # Safety
/// `atlas` must come from a successful `bgl_atlas_build` and not be freed twice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bgl_atlas_free(atlas: *mut BglAtlas) {
    let a = unsafe { &mut *atlas };
    if !a.pixels.is_null() {
        let len = (a.width * a.height) as usize;
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(a.pixels, len)) });
        a.pixels = std::ptr::null_mut();
    }
    if !a.glyphs.is_null() {
        drop(unsafe {
            Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                a.glyphs,
                a.glyph_count as usize,
            ))
        });
        a.glyphs = std::ptr::null_mut();
    }
}
