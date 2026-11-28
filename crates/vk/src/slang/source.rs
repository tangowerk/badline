//! Slang shader files (RetroArch conventions): `#include "file"` and `#pragma include_optional
//! "file"` (relative to the including file), one file for both stages split by `#pragma stage`,
//! metadata pragmas `name`, `format` and `parameter`.

use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq)]
pub struct Parameter {
    pub id: String,
    pub description: String,
    pub initial: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub step: f32,
}

#[derive(Clone, Debug)]
pub struct ShaderSource {
    pub vertex: String,
    pub fragment: String,
    /// `#pragma name` (alias of the pass output, like `aliasN` in the preset)
    pub name: Option<String>,
    /// `#pragma format` (render target format of the pass)
    pub format: Option<String>,
    pub parameters: Vec<Parameter>,
}

const MAX_INCLUDE_DEPTH: usize = 32;

fn read(path: &Path) -> Result<String, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(String::from_utf8_lossy(&bytes).replace("\r\n", "\n").replace('\r', "\n"))
}

/// `#<directive> "path"` with optional blanks after the '#'
fn directive<'a>(line: &'a str, words: &[&str]) -> Option<&'a str> {
    let mut rest = line.trim_start().strip_prefix('#')?;
    for w in words {
        rest = rest.trim_start().strip_prefix(w)?;
        if !rest.starts_with(char::is_whitespace) && !rest.starts_with('"') {
            return None;
        }
    }
    let rest = rest.trim_start().strip_prefix('"')?;
    Some(&rest[..rest.find('"')?])
}

fn expand(path: &Path, depth: usize, out: &mut Vec<String>) -> Result<(), String> {
    if depth > MAX_INCLUDE_DEPTH {
        return Err(format!("{}: includes nested too deeply", path.display()));
    }
    let text = read(path)?;
    for line in text.split('\n') {
        if let Some(inc) = directive(line, &["include"]) {
            let p = relative(path, inc);
            expand(&p, depth + 1, out).map_err(|e| format!("{e}\n  included from {}", path.display()))?;
        } else if let Some(inc) = directive(line, &["pragma", "include_optional"]) {
            let p = relative(path, inc);
            if p.exists() {
                expand(&p, depth + 1, out)?;
            }
        } else {
            out.push(line.to_string());
        }
    }
    Ok(())
}

/// `#pragma <word> rest` -> rest
fn pragma<'a>(line: &'a str, word: &str) -> Option<&'a str> {
    let rest = line.trim_start().strip_prefix('#')?.trim_start().strip_prefix("pragma")?;
    let rest = rest.strip_prefix(char::is_whitespace)?.trim_start().strip_prefix(word)?;
    if rest.is_empty() {
        return Some(rest);
    }
    rest.strip_prefix(char::is_whitespace).map(str::trim)
}

/// longest float prefix after blanks (C `%f`): "0.0.001" reads as 0.0, then ".001"
fn scan_float(s: &str) -> Option<(f32, &str)> {
    let t = s.trim_start();
    let b = t.as_bytes();
    let mut i = 0;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let mut digits = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
        digits += 1;
    }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
            digits += 1;
        }
    }
    if digits == 0 {
        return None;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mut j = i + 1;
        if j < b.len() && (b[j] == b'+' || b[j] == b'-') {
            j += 1;
        }
        if j < b.len() && b[j].is_ascii_digit() {
            while j < b.len() && b[j].is_ascii_digit() {
                j += 1;
            }
            i = j;
        }
    }
    Some((t[..i].parse().ok()?, &t[i..]))
}

/// `NAME "Description" initial min max [step]`, read like RetroArch's sscanf (step defaults to
/// 1/10 of the range)
fn parse_parameter(rest: &str) -> Option<Parameter> {
    let rest = rest.trim();
    let (id, rest) = rest.split_once(char::is_whitespace)?;
    let rest = rest.trim_start();
    let (description, mut numbers) = if let Some(r) = rest.strip_prefix('"') {
        let end = r.find('"')?;
        (r[..end].to_string(), &r[end + 1..])
    } else {
        let (d, n) = rest.split_once(char::is_whitespace)?;
        (d.to_string(), n)
    };
    let mut values = Vec::new();
    while values.len() < 4
        && let Some((v, rest)) = scan_float(numbers)
    {
        values.push(v);
        numbers = rest;
    }
    if values.len() < 3 {
        return None;
    }
    let step = values.get(3).copied().unwrap_or(0.1 * (values[2] - values[1]));
    Some(Parameter {
        id: id.to_string(),
        description,
        initial: values[0],
        minimum: values[1],
        maximum: values[2],
        step,
    })
}

impl ShaderSource {
    pub fn load(path: &Path) -> Result<Self, String> {
        let mut lines = Vec::new();
        expand(path, 0, &mut lines)?;
        Self::from_lines(&lines, &path.display().to_string())
    }

    /// built-in shaders (single string, no includes)
    pub fn from_str(text: &str, what: &str) -> Result<Self, String> {
        let lines: Vec<String> = text.split('\n').map(str::to_string).collect();
        Self::from_lines(&lines, what)
    }

    fn from_lines(lines: &[String], what: &str) -> Result<Self, String> {
        let mut vertex = String::new();
        let mut fragment = String::new();
        let mut name = None;
        let mut format = None;
        let mut parameters: Vec<Parameter> = Vec::new();
        // None: both stages (shared code before the first stage pragma)
        let mut stage: Option<bool> = None;
        let mut has_vertex = false;
        let mut has_fragment = false;
        for line in lines {
            // removed lines stay empty so glslang's line numbers match the expanded file
            let mut keep = true;
            if let Some(s) = pragma(line, "stage") {
                match s.split_whitespace().next() {
                    Some("vertex") => {
                        stage = Some(true);
                        has_vertex = true;
                    }
                    Some("fragment") => {
                        stage = Some(false);
                        has_fragment = true;
                    }
                    other => return Err(format!("{what}: unknown stage {other:?}")),
                }
                keep = false;
            } else if let Some(n) = pragma(line, "name") {
                name = Some(n.trim().to_string());
                keep = false;
            } else if let Some(f) = pragma(line, "format") {
                format = Some(f.trim().to_string());
                keep = false;
            } else if let Some(p) = pragma(line, "parameter") {
                let p = parse_parameter(p).ok_or_else(|| format!("{what}: bad parameter line: {}", line.trim()))?;
                // the same parameter may come from several includes: first definition wins
                if !parameters.iter().any(|q| q.id == p.id) {
                    parameters.push(p);
                }
                keep = false;
            }
            let text = if keep { line.as_str() } else { "" };
            if stage != Some(false) {
                vertex.push_str(text);
                vertex.push('\n');
            }
            if stage != Some(true) {
                fragment.push_str(text);
                fragment.push('\n');
            }
        }
        if !has_vertex || !has_fragment {
            return Err(format!("{what}: needs #pragma stage vertex and fragment"));
        }
        Ok(ShaderSource {
            vertex,
            fragment,
            name,
            format,
            parameters,
        })
    }
}

/// path of an include/reference relative to the file that names it; Windows separators are
/// converted like RetroArch does (`pathname_conform_slashes_to_os`; many presets use `..\\`)
pub fn relative(base: &Path, name: &str) -> PathBuf {
    let name = name.replace('\\', "/");
    let p = Path::new(&name);
    if p.is_absolute() {
        p.to_path_buf()
    } else {
        base.parent().unwrap_or(Path::new(".")).join(p)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split_and_pragmas() {
        let src = "#version 450\n#pragma name Foo\n#pragma parameter A \"Alpha\" 0.5 0.0 1.0 0.1\nshared\n\
                   #pragma stage vertex\nvert\n#pragma stage fragment\n#pragma format R16G16B16A16_SFLOAT\nfrag\n";
        let s = ShaderSource::from_str(src, "t").unwrap();
        assert_eq!(s.name.as_deref(), Some("Foo"));
        assert_eq!(s.format.as_deref(), Some("R16G16B16A16_SFLOAT"));
        assert_eq!(s.parameters.len(), 1);
        assert_eq!(s.parameters[0].step, 0.1);
        assert_eq!(s.vertex, "#version 450\n\n\nshared\n\nvert\n");
        assert_eq!(s.fragment, "#version 450\n\n\nshared\n\n\nfrag\n\n");
    }

    #[test]
    fn windows_separators() {
        let base = Path::new("/s/presets/a/x.slangp");
        assert_eq!(relative(base, "..\\..\\shaders\\m.slang"), Path::new("/s/presets/a/../../shaders/m.slang"));
    }

    #[test]
    fn parameter_without_step() {
        let p = parse_parameter("B \"Beta\" 1.0 0.0 2.0").unwrap();
        assert!((p.step - 0.2).abs() < 1e-6);
        assert_eq!(p.description, "Beta");
        let p = parse_parameter("T \"t\"  0  0 0.0.001 0.001").unwrap();
        assert_eq!((p.initial, p.minimum, p.maximum, p.step), (0.0, 0.0, 0.0, 0.001));
    }
}
