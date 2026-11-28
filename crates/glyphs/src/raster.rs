//! Monochrome rasterizer for unhinted TrueType outlines: a pixel is set when its centre lies
//! inside the outline (non-zero winding). C64 Pro is drawn on a 256-unit grid (8 px per em),
//! so at 8 px nearly every edge falls between pixel centres and the result is exact.

/// one rendered glyph: bitmap rows top down, origin at the pen position
pub struct Bitmap {
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
    pub pixels: Vec<bool>,
}

#[derive(Default)]
pub struct Outline {
    /// line segments in pixel units (y up)
    segments: Vec<[f64; 4]>,
    /// control box incl. off-curve points (pixel units)
    min: [f64; 2],
    max: [f64; 2],
    start: [f64; 2],
    pen: [f64; 2],
    scale: f64,
    empty: bool,
}

impl Outline {
    pub fn new(scale: f64) -> Self {
        Outline {
            min: [f64::MAX; 2],
            max: [f64::MIN; 2],
            scale,
            empty: true,
            ..Default::default()
        }
    }

    fn point(&mut self, x: f32, y: f32) -> [f64; 2] {
        let p = [x as f64 * self.scale, y as f64 * self.scale];
        for (i, v) in p.iter().enumerate() {
            self.min[i] = self.min[i].min(*v);
            self.max[i] = self.max[i].max(*v);
        }
        self.empty = false;
        p
    }

    fn line(&mut self, to: [f64; 2]) {
        if to != self.pen {
            self.segments.push([self.pen[0], self.pen[1], to[0], to[1]]);
        }
        self.pen = to;
    }

    /// renders the outline; `None` for an empty outline (space)
    pub fn render(&self) -> Option<Bitmap> {
        if self.empty {
            return None;
        }
        let left = self.min[0].floor() as i32;
        let right = self.max[0].ceil() as i32;
        let bottom = self.min[1].floor() as i32;
        let top = self.max[1].ceil() as i32;
        let (width, height) = (right - left, top - bottom);
        let mut pixels = vec![false; (width * height) as usize];
        for row in 0..height {
            let cy = (top - row) as f64 - 0.5;
            for col in 0..width {
                let cx = (left + col) as f64 + 0.5;
                pixels[(row * width + col) as usize] = self.winding(cx, cy) != 0;
            }
        }
        Some(Bitmap {
            left,
            top,
            width,
            height,
            pixels,
        })
    }

    fn winding(&self, x: f64, y: f64) -> i32 {
        let mut w = 0;
        for s in &self.segments {
            let (x0, y0, x1, y1) = (s[0], s[1], s[2], s[3]);
            if (y0 <= y) != (y1 <= y) {
                let t = (y - y0) / (y1 - y0);
                if x0 + t * (x1 - x0) > x {
                    w += if y1 > y0 { 1 } else { -1 };
                }
            }
        }
        w
    }
}

impl ttf_parser::OutlineBuilder for Outline {
    fn move_to(&mut self, x: f32, y: f32) {
        let p = self.point(x, y);
        self.start = p;
        self.pen = p;
    }

    fn line_to(&mut self, x: f32, y: f32) {
        let p = self.point(x, y);
        self.line(p);
    }

    fn quad_to(&mut self, x1: f32, y1: f32, x: f32, y: f32) {
        let c = self.point(x1, y1);
        let p = self.point(x, y);
        let s = self.pen;
        const STEPS: usize = 16;
        for i in 1..=STEPS {
            let t = i as f64 / STEPS as f64;
            let u = 1.0 - t;
            let q = [
                u * u * s[0] + 2.0 * u * t * c[0] + t * t * p[0],
                u * u * s[1] + 2.0 * u * t * c[1] + t * t * p[1],
            ];
            self.line(q);
        }
    }

    fn curve_to(&mut self, x1: f32, y1: f32, x2: f32, y2: f32, x: f32, y: f32) {
        let c1 = self.point(x1, y1);
        let c2 = self.point(x2, y2);
        let p = self.point(x, y);
        let s = self.pen;
        const STEPS: usize = 24;
        for i in 1..=STEPS {
            let t = i as f64 / STEPS as f64;
            let u = 1.0 - t;
            let f = |a: f64, b: f64, c: f64, d: f64| {
                u * u * u * a + 3.0 * u * u * t * b + 3.0 * u * t * t * c + t * t * t * d
            };
            let q = [f(s[0], c1[0], c2[0], p[0]), f(s[1], c1[1], c2[1], p[1])];
            self.line(q);
        }
    }

    fn close(&mut self) {
        let s = self.start;
        self.line(s);
    }
}
