//! `.slangp` presets (RetroArch config format): `key = value` lines, `#reference "path"` chains
//! (the referenced preset is the base, later lines override it), paths relative to the file that
//! defines them. Every key that matches a shader parameter id sets its value.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use super::source::relative;

const MAX_REFERENCE_DEPTH: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScaleType {
    Source,
    Viewport,
    Absolute,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Wrap {
    Border,
    Edge,
    Repeat,
    Mirror,
}

impl Wrap {
    fn parse(v: &str) -> Wrap {
        match v {
            "clamp_to_edge" => Wrap::Edge,
            "repeat" => Wrap::Repeat,
            "mirrored_repeat" => Wrap::Mirror,
            _ => Wrap::Border,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Scale {
    pub x: (ScaleType, f32),
    pub y: (ScaleType, f32),
}

#[derive(Clone, Debug)]
pub struct PassConfig {
    pub shader: PathBuf,
    /// None: the global filter setting (smooth/nearest of the picture)
    pub filter_linear: Option<bool>,
    pub wrap: Wrap,
    pub mipmap_input: bool,
    pub alias: Option<String>,
    pub float_framebuffer: bool,
    pub srgb_framebuffer: bool,
    /// None: source x1, the last pass viewport x1
    pub scale: Option<Scale>,
    pub frame_count_mod: u32,
}

#[derive(Clone, Debug)]
pub struct LutConfig {
    pub name: String,
    pub path: PathBuf,
    pub linear: bool,
    pub wrap: Wrap,
    pub mipmap: bool,
}

#[derive(Clone, Debug)]
pub struct PresetConfig {
    pub path: PathBuf,
    pub passes: Vec<PassConfig>,
    pub luts: Vec<LutConfig>,
    /// all keys (parameter values are looked up by id)
    values: HashMap<String, Value>,
}

#[derive(Clone, Debug)]
struct Value {
    text: String,
    /// file that set the value (base of relative paths)
    file: PathBuf,
}

/// one `key = value` line: quoted values end at the closing quote (a stray second quote, as in
/// some corpus presets, is ignored), plain values at the first blank
fn parse_line(line: &str) -> Option<(String, String)> {
    let line = line.trim();
    if line.is_empty() || line.starts_with('#') {
        return None;
    }
    let (key, rest) = line.split_once('=')?;
    let key = key.trim();
    if key.is_empty() || key.contains(char::is_whitespace) {
        return None;
    }
    let rest = rest.trim_start();
    let value = if let Some(r) = rest.strip_prefix('"') {
        &r[..r.find('"').unwrap_or(r.len())]
    } else {
        rest.split_whitespace().next().unwrap_or("")
    };
    Some((key.to_string(), value.to_string()))
}

fn reference(line: &str) -> Option<&str> {
    let rest = line.trim_start().strip_prefix("#reference")?;
    let rest = rest.trim_start().strip_prefix('"')?;
    Some(&rest[..rest.find('"')?])
}

fn collect(path: &Path, depth: usize, values: &mut HashMap<String, Value>) -> Result<(), String> {
    if depth > MAX_REFERENCE_DEPTH {
        return Err(format!("{}: #reference nested too deeply", path.display()));
    }
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let text = String::from_utf8_lossy(&bytes);
    for line in text.lines() {
        if let Some(r) = reference(line) {
            let p = relative(path, r);
            collect(&p, depth + 1, values).map_err(|e| format!("{e}\n  referenced from {}", path.display()))?;
        } else if let Some((k, v)) = parse_line(line) {
            values.insert(k, Value {
                text: v,
                file: path.to_path_buf(),
            });
        }
    }
    Ok(())
}

fn parse_bool(v: &str) -> Option<bool> {
    match v.trim().to_ascii_lowercase().as_str() {
        "true" | "1" => Some(true),
        "false" | "0" => Some(false),
        _ => None,
    }
}

fn scale_type(v: &str) -> Option<ScaleType> {
    match v {
        "source" => Some(ScaleType::Source),
        "viewport" => Some(ScaleType::Viewport),
        "absolute" => Some(ScaleType::Absolute),
        _ => None,
    }
}

impl PresetConfig {
    pub fn load(path: &Path) -> Result<Self, String> {
        let mut values = HashMap::new();
        collect(path, 0, &mut values)?;
        let mut preset = PresetConfig {
            path: path.to_path_buf(),
            passes: Vec::new(),
            luts: Vec::new(),
            values,
        };
        let count: usize = preset
            .get("shaders")
            .ok_or_else(|| format!("{}: no 'shaders' count", path.display()))?
            .trim()
            .parse()
            .map_err(|_| format!("{}: bad 'shaders' count", path.display()))?;
        for i in 0..count {
            let pass = preset.pass(i)?;
            preset.passes.push(pass);
        }
        if let Some(list) = preset.get("textures").map(str::to_string) {
            for name in list.split(';').map(str::trim).filter(|n| !n.is_empty()) {
                let path = preset
                    .path_of(name)
                    .ok_or_else(|| format!("{}: texture '{name}' has no path", path.display()))?;
                preset.luts.push(LutConfig {
                    name: name.to_string(),
                    path,
                    linear: preset.bool_of(&format!("{name}_linear")).unwrap_or(false),
                    wrap: preset.get(&format!("{name}_wrap_mode")).map(Wrap::parse).unwrap_or(Wrap::Border),
                    mipmap: preset.bool_of(&format!("{name}_mipmap")).unwrap_or(false),
                });
            }
        }
        Ok(preset)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(|v| v.text.as_str())
    }

    fn bool_of(&self, key: &str) -> Option<bool> {
        self.get(key).and_then(parse_bool)
    }

    fn path_of(&self, key: &str) -> Option<PathBuf> {
        let v = self.values.get(key)?;
        let t = v.text.trim();
        (!t.is_empty()).then(|| relative(&v.file, t))
    }

    /// value of a shader parameter set in the preset (or a referencing preset)
    pub fn parameter(&self, id: &str) -> Option<f32> {
        self.get(id).and_then(|v| v.trim().trim_end_matches('f').parse().ok())
    }

    fn pass(&self, i: usize) -> Result<PassConfig, String> {
        let shader = self
            .path_of(&format!("shader{i}"))
            .ok_or_else(|| format!("{}: shader{i} missing", self.path.display()))?;
        let st = self.get(&format!("scale_type{i}")).and_then(scale_type);
        let stx = self.get(&format!("scale_type_x{i}")).and_then(scale_type).or(st);
        let sty = self.get(&format!("scale_type_y{i}")).and_then(scale_type).or(st);
        let scale = if stx.is_some() || sty.is_some() {
            let num = |k: &str| self.get(k).and_then(|v| v.trim().parse::<f32>().ok());
            let s = num(&format!("scale{i}"));
            let sx = num(&format!("scale_x{i}")).or(s).unwrap_or(1.0);
            let sy = num(&format!("scale_y{i}")).or(s).unwrap_or(1.0);
            Some(Scale {
                x: (stx.unwrap_or(ScaleType::Source), sx),
                y: (sty.unwrap_or(ScaleType::Source), sy),
            })
        } else {
            None
        };
        Ok(PassConfig {
            shader,
            filter_linear: self.bool_of(&format!("filter_linear{i}")),
            wrap: self.get(&format!("wrap_mode{i}")).map(Wrap::parse).unwrap_or(Wrap::Border),
            mipmap_input: self.bool_of(&format!("mipmap_input{i}")).unwrap_or(false),
            alias: self
                .get(&format!("alias{i}"))
                .map(|a| a.trim().to_string())
                .filter(|a| !a.is_empty()),
            float_framebuffer: self.bool_of(&format!("float_framebuffer{i}")).unwrap_or(false),
            srgb_framebuffer: self.bool_of(&format!("srgb_framebuffer{i}")).unwrap_or(false),
            scale,
            frame_count_mod: self
                .get(&format!("frame_count_mod{i}"))
                .and_then(|v| v.trim().parse().ok())
                .unwrap_or(0),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines() {
        assert_eq!(parse_line("shader0 = \"a b.slang\"\"").unwrap(), ("shader0".into(), "a b.slang".into()));
        assert_eq!(parse_line("scale0 = 2.0 # two").unwrap(), ("scale0".into(), "2.0".into()));
        assert!(parse_line("# comment").is_none());
        assert!(parse_line("//#reference \"x\"").is_none_or(|(k, _)| k.starts_with("//")));
        assert_eq!(reference("#reference \"../x.slangp\"").unwrap(), "../x.slangp");
    }

    #[test]
    fn reference_chain() {
        let dir = std::env::temp_dir().join(format!("badline-preset-test-{}", std::process::id()));
        std::fs::create_dir_all(dir.join("sub")).unwrap();
        std::fs::write(
            dir.join("sub/base.slangp"),
            "shaders = 2\nshader0 = a.slang\nshader1 = b.slang\nscale_type1 = viewport\n\
             textures = \"LUT\"\nLUT = lut.png\nLUT_linear = true\nP = 1.0\n",
        )
        .unwrap();
        std::fs::write(dir.join("top.slangp"), "#reference \"sub/base.slangp\"\nP = 0.25\nLUT = other.png\n").unwrap();
        let p = PresetConfig::load(&dir.join("top.slangp")).unwrap();
        assert_eq!(p.passes.len(), 2);
        assert_eq!(p.passes[0].shader, dir.join("sub/a.slang"));
        assert_eq!(p.passes[1].scale.unwrap().x, (ScaleType::Viewport, 1.0));
        assert!(p.passes[0].scale.is_none());
        assert_eq!(p.luts[0].path, dir.join("other.png"));
        assert!(p.luts[0].linear);
        assert_eq!(p.parameter("P"), Some(0.25));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
