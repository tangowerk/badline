//! RetroArch slang shader presets (M-VK3a): preset + shader parsing, compilation through the system
//! glslang with a disk cache, reflection and semantic resolution. Everything here is CPU work
//! without a Vulkan device, so a preset can be prepared off the render thread; `chain.rs` builds
//! the Vulkan objects from the result.

pub mod glslang;
pub mod preset;
pub mod reflect;
pub mod source;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use ash::vk;

pub use glslang::Stage;
use preset::{PassConfig, PresetConfig, Wrap};
use reflect::{Base, Reflection};
use source::ShaderSource;

/// texture semantics (RetroArch slang spec)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tex {
    Original,
    Source,
    /// OriginalHistoryN, N >= 1 (0 is Original)
    History(u32),
    PassOutput(u32),
    PassFeedback(u32),
    Lut(u32),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Uni {
    Mvp,
    OutputSize,
    FinalViewportSize,
    FrameCount,
    FrameDirection,
    FrameTimeDelta,
    OriginalFps,
    Rotation,
    OriginalAspect,
    OriginalAspectRotated,
    TotalSubFrames,
    CurrentSubFrame,
    /// HDR semantics of newer RetroArch versions: Badline outputs SDR (HDR off, nits defaults)
    Hdr(f32),
    Size(Tex),
    Param(u32),
    /// not a known semantic: stays 0 (RetroArch refuses the shader)
    Unknown,
}

#[derive(Clone, Debug)]
pub struct Slot {
    pub uni: Uni,
    pub offset: u32,
    pub base: Base,
    pub components: u32,
}

#[derive(Clone, Debug)]
pub struct Binding {
    pub binding: u32,
    pub tex: Tex,
}

#[derive(Clone, Debug)]
pub struct Param {
    pub id: String,
    pub description: String,
    pub value: f32,
    pub initial: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub step: f32,
}

#[derive(Clone, Debug)]
pub struct CompiledPass {
    pub config: PassConfig,
    pub vertex: Vec<u32>,
    pub fragment: Vec<u32>,
    pub ubo_binding: Option<u32>,
    pub ubo_size: u32,
    pub ubo: Vec<Slot>,
    pub push_size: u32,
    pub push: Vec<Slot>,
    pub textures: Vec<Binding>,
    /// render target format (ignored for the last pass, which renders to the output)
    pub format: vk::Format,
}

pub struct Lut {
    pub name: String,
    pub linear: bool,
    pub wrap: Wrap,
    pub mipmap: bool,
    pub width: u32,
    pub height: u32,
    /// RGBA8, top row first
    pub rgba: Vec<u8>,
}

pub struct Compiled {
    pub path: PathBuf,
    pub passes: Vec<CompiledPass>,
    pub luts: Vec<Lut>,
    pub params: Vec<Param>,
    /// OriginalHistory depth (highest N used)
    pub history: u32,
    /// passes whose previous output is read (PassFeedbackN)
    pub feedback: Vec<bool>,
    /// stages compiled / taken from the disk cache, unknown uniform names (warnings)
    pub compiled: usize,
    pub cached: usize,
    pub warnings: Vec<String>,
}

// ------------------------------------------------------------ disk cache

/// FNV-1a 64 with two offsets (128 bit key); stable across Rust versions
fn hash(parts: &[&[u8]]) -> String {
    let mut a: u64 = 0xcbf2_9ce4_8422_2325;
    let mut b: u64 = 0x6c62_272e_07bb_0142;
    for p in parts {
        for &x in p.iter().chain(&[0xffu8]) {
            a = (a ^ x as u64).wrapping_mul(0x0000_0100_0000_01b3);
            b = (b ^ x as u64).wrapping_mul(0x0000_0100_0000_01b3).rotate_left(5);
        }
    }
    format!("{a:016x}{b:016x}")
}

/// `$XDG_CACHE_HOME/badline/shaders` (else `~/.cache/badline/shaders`)
pub fn cache_dir() -> Option<PathBuf> {
    let base = std::env::var_os("XDG_CACHE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache")))?;
    Some(base.join("badline/shaders"))
}

static VERSION: Mutex<Option<String>> = Mutex::new(None);

fn compiler_version() -> String {
    VERSION.lock().unwrap().get_or_insert_with(glslang::version).clone()
}

/// SPIR-V of one stage, from the disk cache when possible; the bool is true for a cache hit
pub fn spirv(source: &str, stage: Stage) -> Result<(Vec<u32>, bool), String> {
    let dir = cache_dir();
    let key = hash(&[compiler_version().as_bytes(), stage.name().as_bytes(), source.as_bytes()]);
    let file = dir.as_ref().map(|d| d.join(format!("{key}.spv")));
    if let Some(f) = &file
        && let Ok(bytes) = std::fs::read(f)
        && bytes.len() >= 20
        && bytes.len() % 4 == 0
    {
        let words: Vec<u32> = bytes.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        if words[0] == 0x0723_0203 {
            return Ok((words, true));
        }
    }
    let words = glslang::compile(source, stage)?;
    if let (Some(d), Some(f)) = (&dir, &file) {
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        // write + rename: parallel instances never see half a file
        let tmp = f.with_extension(format!("tmp{}", std::process::id()));
        if std::fs::create_dir_all(d).is_ok() && std::fs::write(&tmp, &bytes).is_ok() && std::fs::rename(&tmp, f).is_err() {
            let _ = std::fs::remove_file(&tmp);
        }
    }
    Ok((words, false))
}

// ------------------------------------------------------------ semantics

fn number(s: &str) -> Option<u32> {
    (!s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())).then(|| s.parse().ok()).flatten()
}

struct Names<'a> {
    aliases: &'a [Option<String>],
    luts: &'a [String],
    params: &'a HashMap<String, u32>,
}

impl Names<'_> {
    fn texture(&self, name: &str) -> Option<Tex> {
        match name {
            "Original" => return Some(Tex::Original),
            "Source" => return Some(Tex::Source),
            _ => {}
        }
        let indexed = [
            ("OriginalHistory", Tex::History as fn(u32) -> Tex),
            ("PassOutput", Tex::PassOutput),
            ("PassFeedback", Tex::PassFeedback),
            ("User", Tex::Lut),
        ];
        for (prefix, make) in indexed {
            if let Some(n) = name.strip_prefix(prefix).and_then(number) {
                return Some(if prefix == "OriginalHistory" && n == 0 { Tex::Original } else { make(n) });
            }
        }
        if let Some(i) = self.aliases.iter().position(|a| a.as_deref() == Some(name)) {
            return Some(Tex::PassOutput(i as u32));
        }
        if let Some(base) = name.strip_suffix("Feedback")
            && let Some(i) = self.aliases.iter().position(|a| a.as_deref() == Some(base))
        {
            return Some(Tex::PassFeedback(i as u32));
        }
        self.luts.iter().position(|l| l == name).map(|i| Tex::Lut(i as u32))
    }

    fn uniform(&self, name: &str) -> Uni {
        let fixed = match name {
            "MVP" => Some(Uni::Mvp),
            "OutputSize" => Some(Uni::OutputSize),
            "FinalViewportSize" => Some(Uni::FinalViewportSize),
            "FrameCount" => Some(Uni::FrameCount),
            "FrameDirection" => Some(Uni::FrameDirection),
            "FrameTimeDelta" => Some(Uni::FrameTimeDelta),
            "OriginalFPS" => Some(Uni::OriginalFps),
            "Rotation" => Some(Uni::Rotation),
            "OriginalAspect" => Some(Uni::OriginalAspect),
            "OriginalAspectRotated" => Some(Uni::OriginalAspectRotated),
            "TotalSubFrames" => Some(Uni::TotalSubFrames),
            "CurrentSubFrame" => Some(Uni::CurrentSubFrame),
            _ => None,
        };
        if let Some(u) = fixed {
            return u;
        }
        // parameters win over texture sizes (a parameter may end in "Size") and over the HDR
        // values (several HDR shaders declare them as parameters, verify against RetroArch)
        if let Some(&i) = self.params.get(name) {
            return Uni::Param(i);
        }
        let hdr = match name {
            "EnableHDR" | "HDRMode" | "ExpandGamut" | "InverseTonemap" | "HDR10" => Some(0.0),
            "PaperWhiteNits" | "BrightnessNits" => Some(200.0),
            "MaxNits" => Some(1000.0),
            _ => None,
        };
        if let Some(v) = hdr {
            return Uni::Hdr(v);
        }
        if let Some(base) = name.strip_suffix("Size")
            && let Some(t) = self.texture(base)
        {
            return Uni::Size(t);
        }
        // indexed sizes: OriginalHistorySizeN, PassOutputSizeN, PassFeedbackSizeN, UserSizeN
        for prefix in ["OriginalHistory", "PassOutput", "PassFeedback", "User"] {
            if let Some(n) = name.strip_prefix(prefix).and_then(|r| r.strip_prefix("Size")).and_then(number)
                && let Some(t) = self.texture(&format!("{prefix}{n}"))
            {
                return Uni::Size(t);
            }
        }
        Uni::Unknown
    }
}

pub fn format_by_name(name: &str) -> Option<vk::Format> {
    use vk::Format as F;
    Some(match name {
        "R8_UNORM" => F::R8_UNORM,
        "R8_UINT" => F::R8_UINT,
        "R8_SINT" => F::R8_SINT,
        "R8G8_UNORM" => F::R8G8_UNORM,
        "R8G8_UINT" => F::R8G8_UINT,
        "R8G8_SINT" => F::R8G8_SINT,
        "R8G8B8A8_UNORM" => F::R8G8B8A8_UNORM,
        "R8G8B8A8_UINT" => F::R8G8B8A8_UINT,
        "R8G8B8A8_SINT" => F::R8G8B8A8_SINT,
        "R8G8B8A8_SRGB" => F::R8G8B8A8_SRGB,
        "A2B10G10R10_UNORM_PACK32" => F::A2B10G10R10_UNORM_PACK32,
        "A2B10G10R10_UINT_PACK32" => F::A2B10G10R10_UINT_PACK32,
        "R16_UINT" => F::R16_UINT,
        "R16_SINT" => F::R16_SINT,
        "R16_SFLOAT" => F::R16_SFLOAT,
        "R16G16_UINT" => F::R16G16_UINT,
        "R16G16_SINT" => F::R16G16_SINT,
        "R16G16_SFLOAT" => F::R16G16_SFLOAT,
        "R16G16B16A16_UINT" => F::R16G16B16A16_UINT,
        "R16G16B16A16_SINT" => F::R16G16B16A16_SINT,
        "R16G16B16A16_SFLOAT" => F::R16G16B16A16_SFLOAT,
        "R32_UINT" => F::R32_UINT,
        "R32_SINT" => F::R32_SINT,
        "R32_SFLOAT" => F::R32_SFLOAT,
        "R32G32_UINT" => F::R32G32_UINT,
        "R32G32_SINT" => F::R32G32_SINT,
        "R32G32_SFLOAT" => F::R32G32_SFLOAT,
        "R32G32B32A32_UINT" => F::R32G32B32A32_UINT,
        "R32G32B32A32_SINT" => F::R32G32B32A32_SINT,
        "R32G32B32A32_SFLOAT" => F::R32G32B32A32_SFLOAT,
        _ => return None,
    })
}

// ------------------------------------------------------------ LUT images

fn decode_png(bytes: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(bytes));
    decoder.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = decoder.read_info().map_err(|e| e.to_string())?;
    let mut buf = vec![0; reader.output_buffer_size().ok_or("image too large")?];
    let info = reader.next_frame(&mut buf).map_err(|e| e.to_string())?;
    let (w, h) = (info.width, info.height);
    let px = (w * h) as usize;
    let rgba = match info.color_type {
        png::ColorType::Rgba => buf[..px * 4].to_vec(),
        png::ColorType::Rgb => buf[..px * 3].chunks_exact(3).flat_map(|c| [c[0], c[1], c[2], 255]).collect(),
        png::ColorType::GrayscaleAlpha => buf[..px * 2].chunks_exact(2).flat_map(|c| [c[0], c[0], c[0], c[1]]).collect(),
        png::ColorType::Grayscale => buf[..px].iter().flat_map(|&g| [g, g, g, 255]).collect(),
        png::ColorType::Indexed => return Err("palette not expanded".into()),
    };
    Ok((w, h, rgba))
}

fn decode_jpeg(bytes: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    use zune_jpeg::zune_core::colorspace::ColorSpace;
    use zune_jpeg::zune_core::options::DecoderOptions;
    let options = DecoderOptions::default().jpeg_set_out_colorspace(ColorSpace::RGBA);
    let mut d = zune_jpeg::JpegDecoder::new_with_options(std::io::Cursor::new(bytes), options);
    let rgba = d.decode().map_err(|e| format!("{e:?}"))?;
    let info = d.info().ok_or("no image info")?;
    Ok((info.width as u32, info.height as u32, rgba))
}

fn load_image(path: &Path) -> Result<(u32, u32, Vec<u8>), String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let res = if bytes.starts_with(b"\x89PNG") {
        decode_png(&bytes)
    } else if bytes.starts_with(&[0xff, 0xd8]) {
        decode_jpeg(&bytes)
    } else {
        Err("unsupported image format (PNG or JPEG)".into())
    };
    let (w, h, rgba) = res.map_err(|e| format!("{}: {e}", path.display()))?;
    if w == 0 || h == 0 || rgba.len() != (w * h * 4) as usize {
        return Err(format!("{}: bad image size", path.display()));
    }
    Ok((w, h, rgba))
}

// ------------------------------------------------------------ preset compilation

/// final blit when the last pass of a preset has its own scale (RetroArch appends the same)
const STOCK: &str = r#"#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() { FragColor = texture(Source, vTexCoord); }
"#;

/// runs `f` over `items` on all cores
fn parallel<T: Sync, R: Send>(items: &[T], f: impl Fn(&T) -> R + Sync) -> Vec<R> {
    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<Option<R>>> = Mutex::new((0..items.len()).map(|_| None).collect());
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(items.len()).max(1);
    std::thread::scope(|s| {
        for _ in 0..workers {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= items.len() {
                        break;
                    }
                    let r = f(&items[i]);
                    results.lock().unwrap()[i] = Some(r);
                }
            });
        }
    });
    results.into_inner().unwrap().into_iter().map(|r| r.unwrap()).collect()
}

struct StageResult {
    spirv: Vec<u32>,
    cached: bool,
}

/// Loads and compiles a preset. `overrides` are parameter values set by the user (they win over
/// the preset's own values).
pub fn compile(path: &Path, overrides: &[(String, f32)]) -> Result<Compiled, String> {
    let config = PresetConfig::load(path)?;
    let mut passes = config.passes.clone();
    if passes.is_empty() {
        return Err(format!("{}: preset has no passes", path.display()));
    }
    let stock = passes.last().unwrap().scale.is_some();

    // sources (shared files are loaded once)
    let mut sources: Vec<ShaderSource> = Vec::new();
    let mut index_of: HashMap<PathBuf, usize> = HashMap::new();
    let mut source_of = Vec::new();
    for p in &passes {
        let i = match index_of.get(&p.shader) {
            Some(&i) => i,
            None => {
                sources.push(ShaderSource::load(&p.shader)?);
                index_of.insert(p.shader.clone(), sources.len() - 1);
                sources.len() - 1
            }
        };
        source_of.push(i);
    }
    if stock {
        sources.push(ShaderSource::from_str(STOCK, "stock")?);
        source_of.push(sources.len() - 1);
        passes.push(PassConfig {
            shader: PathBuf::from("stock"),
            filter_linear: None,
            wrap: Wrap::Border,
            mipmap_input: false,
            alias: None,
            float_framebuffer: false,
            srgb_framebuffer: false,
            scale: None,
            frame_count_mod: 0,
        });
    }

    let jobs: Vec<(usize, Stage)> =
        (0..sources.len()).flat_map(|i| [(i, Stage::Vertex), (i, Stage::Fragment)]).collect();
    let results = parallel(&jobs, |&(i, stage)| {
        let src = if stage == Stage::Vertex { &sources[i].vertex } else { &sources[i].fragment };
        spirv(src, stage).map(|(spirv, cached)| StageResult { spirv, cached })
    });
    let mut stages: Vec<[Option<StageResult>; 2]> = (0..sources.len()).map(|_| [None, None]).collect();
    let (mut compiled, mut cached) = (0, 0);
    for ((i, stage), r) in jobs.iter().zip(results) {
        let file = passes[source_of.iter().position(|&s| s == *i).unwrap()].shader.display().to_string();
        let r = r.map_err(|e| format!("{file}: {e}"))?;
        if r.cached {
            cached += 1;
        } else {
            compiled += 1;
        }
        stages[*i][(*stage == Stage::Fragment) as usize] = Some(r);
    }

    // parameters: first definition wins, preset values, then user overrides
    let mut params: Vec<Param> = Vec::new();
    let mut param_index: HashMap<String, u32> = HashMap::new();
    for &s in &source_of {
        for p in &sources[s].parameters {
            if param_index.contains_key(&p.id) {
                continue;
            }
            let value = overrides
                .iter()
                .find(|(id, _)| *id == p.id)
                .map(|o| o.1)
                .or_else(|| config.parameter(&p.id))
                .unwrap_or(p.initial);
            param_index.insert(p.id.clone(), params.len() as u32);
            params.push(Param {
                id: p.id.clone(),
                description: p.description.clone(),
                value,
                initial: p.initial,
                minimum: p.minimum,
                maximum: p.maximum,
                step: p.step,
            });
        }
    }

    // aliases: preset aliasN, else `#pragma name`
    let aliases: Vec<Option<String>> = passes
        .iter()
        .zip(&source_of)
        .map(|(p, &s)| p.alias.clone().or_else(|| sources[s].name.clone()))
        .collect();
    let lut_names: Vec<String> = config.luts.iter().map(|l| l.name.clone()).collect();
    let names = Names {
        aliases: &aliases,
        luts: &lut_names,
        params: &param_index,
    };

    let mut out = Vec::new();
    let mut history = 0;
    let mut feedback = vec![false; passes.len()];
    let mut warnings = Vec::new();
    let pass_count = passes.len();
    for (pi, (mut p, &s)) in passes.into_iter().zip(&source_of).enumerate() {
        let [v, f] = &stages[s];
        let (v, f) = (v.as_ref().unwrap(), f.as_ref().unwrap());
        let what = p.shader.display().to_string();
        let mut r = Reflection::default();
        reflect::reflect_stage(&v.spirv, false, &mut r).map_err(|e| format!("{what}: {e}"))?;
        reflect::reflect_stage(&f.spirv, true, &mut r).map_err(|e| format!("{what}: {e}"))?;
        // undefined values on the GPU, a validation error (RetroArch passes such shaders on too)
        for loc in r.inputs.iter().filter(|l| !r.outputs.contains(l)) {
            warnings.push(format!("{what}: fragment input location {loc} is not written by the vertex stage"));
        }

        let mut textures = Vec::new();
        for smp in &r.samplers {
            let tex = names
                .texture(&smp.name)
                .ok_or_else(|| format!("{what}: unknown texture '{}'", smp.name))?;
            match tex {
                Tex::PassOutput(n) if n as usize >= pi => {
                    return Err(format!("{what}: '{}' is the output of a later pass", smp.name));
                }
                Tex::PassFeedback(n) | Tex::PassOutput(n) if n as usize >= pass_count => {
                    return Err(format!("{what}: '{}' names pass {n}, the preset has {pass_count}", smp.name));
                }
                Tex::Lut(n) if n as usize >= lut_names.len() => {
                    return Err(format!("{what}: '{}': no such texture", smp.name));
                }
                Tex::History(n) => history = history.max(n),
                Tex::PassFeedback(n) => feedback[n as usize] = true,
                _ => {}
            }
            textures.push(Binding {
                binding: smp.binding,
                tex,
            });
        }
        let mut slots = |block: &Option<reflect::Block>| -> Vec<Slot> {
            let Some(b) = block else { return Vec::new() };
            b.members
                .iter()
                .map(|m| {
                    let uni = names.uniform(&m.name);
                    match uni {
                        Uni::Unknown => warnings.push(format!("{what}: unknown uniform '{}'", m.name)),
                        Uni::Size(Tex::History(n)) => history = history.max(n),
                        Uni::Size(Tex::PassFeedback(n)) if (n as usize) < pass_count => feedback[n as usize] = true,
                        _ => {}
                    }
                    Slot {
                        uni,
                        offset: m.offset,
                        base: m.base,
                        components: m.components,
                    }
                })
                .collect()
        };
        let ubo = slots(&r.ubo);
        let push = slots(&r.push);
        if r.push.as_ref().is_some_and(|b| b.size > 128) {
            return Err(format!("{what}: push constant block larger than 128 bytes"));
        }

        // render target: preset flags override `#pragma format`
        let mut format = sources[s]
            .format
            .as_deref()
            .map(|n| format_by_name(n).ok_or_else(|| format!("{what}: unknown format {n}")))
            .transpose()?
            .unwrap_or(vk::Format::R8G8B8A8_UNORM);
        if p.srgb_framebuffer {
            format = vk::Format::R8G8B8A8_SRGB;
        }
        if p.float_framebuffer {
            format = vk::Format::R16G16B16A16_SFLOAT;
        }
        if p.alias.is_none() {
            p.alias = aliases[pi].clone();
        }
        out.push(CompiledPass {
            config: p,
            vertex: v.spirv.clone(),
            fragment: f.spirv.clone(),
            ubo_binding: r.ubo.as_ref().map(|b| b.binding),
            ubo_size: r.ubo.as_ref().map_or(0, |b| b.size),
            ubo,
            push_size: r.push.as_ref().map_or(0, |b| b.size),
            push,
            textures,
            format,
        });
    }

    let mut luts = Vec::new();
    let loaded = parallel(&config.luts, |l| load_image(&l.path));
    for (l, img) in config.luts.iter().zip(loaded) {
        let (width, height, rgba) = img?;
        luts.push(Lut {
            name: l.name.clone(),
            linear: l.linear,
            wrap: l.wrap,
            mipmap: l.mipmap,
            width,
            height,
            rgba,
        });
    }

    warnings.dedup();
    Ok(Compiled {
        path: path.to_path_buf(),
        passes: out,
        luts,
        params,
        history,
        feedback,
        compiled,
        cached,
        warnings,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        let aliases = vec![Some("First".to_string()), None];
        let luts = vec!["Mask".to_string()];
        let mut params = HashMap::new();
        params.insert("GammaSize".to_string(), 0);
        let n = Names {
            aliases: &aliases,
            luts: &luts,
            params: &params,
        };
        assert_eq!(n.texture("OriginalHistory0"), Some(Tex::Original));
        assert_eq!(n.texture("OriginalHistory3"), Some(Tex::History(3)));
        assert_eq!(n.texture("First"), Some(Tex::PassOutput(0)));
        assert_eq!(n.texture("FirstFeedback"), Some(Tex::PassFeedback(0)));
        assert_eq!(n.texture("Mask"), Some(Tex::Lut(0)));
        assert_eq!(n.uniform("FirstSize"), Uni::Size(Tex::PassOutput(0)));
        assert_eq!(n.uniform("PassFeedbackSize1"), Uni::Size(Tex::PassFeedback(1)));
        assert_eq!(n.uniform("MaskSize"), Uni::Size(Tex::Lut(0)));
        assert_eq!(n.uniform("GammaSize"), Uni::Param(0));
        assert_eq!(n.uniform("Nope"), Uni::Unknown);
    }
}
