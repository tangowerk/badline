//! Slang corpus test (M-VK3c): compiles every .slangp below a directory (parse, glslang,
//! reflection, LUT decoding) and with `--render` builds its chain on a window-less Vulkan device
//! and renders a few frames of a test picture.
//!
//! `cargo run --release -p badline-vk --example slang_corpus -- <dir> [options]`
//!   --filter S      only presets whose path contains S
//!   --list FILE     only the presets listed in FILE (paths relative to <dir>, # comments)
//!   --known FILE    presets expected to fail (broken in the collection itself): reported, no error
//!   --render        render after compiling (device: first Vulkan 1.3 one, VK_DRIVER_FILES selects)
//!   --validation    with the validation layer; errors are reported per preset (no failure;
//!                   unwritten fragment inputs are skipped, they are WARN lines already)
//!   --frames N      frames per preset (default 3), --original WxH (384x272), --viewport WxH (960x720)
//!   --png DIR       save the last output of every rendered preset as PNG
//!   --shard K/N     only every N-th preset starting with the K-th (0-based; CI matrix)
//!   --times         print the time per preset
//! Exit code 1 when a preset outside --known fails or a --known one passes (stale entry).

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            if p.file_name().is_some_and(|n| n != ".git") {
                walk(&p, out);
            }
        } else if p.extension().is_some_and(|x| x == "slangp") {
            out.push(p);
        }
    }
}

fn read_list(path: &str) -> BTreeSet<String> {
    let text = std::fs::read_to_string(path).unwrap_or_else(|e| panic!("{path}: {e}"));
    text.lines()
        .map(|l| l.split('#').next().unwrap().trim())
        .filter(|l| !l.is_empty())
        .map(str::to_owned)
        .collect()
}

fn size(s: &str) -> (u32, u32) {
    let (w, h) = s.split_once('x').expect("WxH");
    (w.parse().expect("width"), h.parse().expect("height"))
}

fn save_png(path: &Path, w: u32, h: u32, bgra: &[u8]) {
    let mut rgba = bgra.to_vec();
    for px in rgba.chunks_exact_mut(4) {
        px.swap(0, 2);
        px[3] = 255;
    }
    let file = std::fs::File::create(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().and_then(|mut w| w.write_image_data(&rgba)).expect("png");
}

fn main() {
    let mut args = std::env::args().skip(1);
    let dir = PathBuf::from(args.next().expect("corpus directory"));
    let (mut filter, mut list, mut known) = (String::new(), None, BTreeSet::new());
    let (mut render, mut validation, mut frames) = (false, false, 3u32);
    let (mut original, mut viewport, mut png_dir) = ((384, 272), (960, 720), None::<PathBuf>);
    let (mut shard, mut times) = ((0usize, 1usize), false);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| panic!("{a} needs a value"));
        match a.as_str() {
            "--filter" => filter = val(),
            "--list" => list = Some(read_list(&val())),
            "--known" => known = read_list(&val()),
            "--render" => render = true,
            "--validation" => validation = true,
            "--frames" => frames = val().parse().expect("frames"),
            "--original" => original = size(&val()),
            "--viewport" => viewport = size(&val()),
            "--png" => png_dir = Some(PathBuf::from(val())),
            "--shard" => {
                let v = val();
                let (k, n) = v.split_once('/').expect("K/N");
                shard = (k.parse().expect("K"), n.parse().expect("N"));
                assert!(shard.1 > 0 && shard.0 < shard.1, "--shard K/N with K < N");
            }
            "--times" => times = true,
            _ if filter.is_empty() && !a.starts_with("--") => filter = a, // old positional filter
            _ => panic!("unknown option {a}"),
        }
    }
    let mut presets = Vec::new();
    walk(&dir, &mut presets);
    presets.sort();
    let rel = |p: &Path| p.strip_prefix(&dir).unwrap_or(p).to_string_lossy().into_owned();
    presets.retain(|p| rel(p).contains(&filter) && list.as_ref().is_none_or(|l| l.contains(&rel(p))));
    presets = presets.into_iter().skip(shard.0).step_by(shard.1).collect();
    if let Some(l) = &list {
        let found: BTreeSet<String> = presets.iter().map(|p| rel(p)).collect();
        for missing in l.difference(&found) {
            println!("MISSING {missing}");
        }
    }
    if let Some(d) = &png_dir {
        std::fs::create_dir_all(d).expect("png dir");
    }
    let headless = render.then(|| match badline_vk::headless::Headless::new(validation) {
        Ok(h) => {
            println!("device: {}", h.device_name);
            h
        }
        Err(e) => {
            eprintln!("Vulkan: {e}");
            std::process::exit(2);
        }
    });

    let t0 = std::time::Instant::now();
    let (mut ok, mut fail, mut warn, mut invalid, mut flat) = (0, 0, 0, 0, 0);
    let (mut unexpected, mut stale) = (Vec::new(), Vec::new());
    for p in &presets {
        let name = rel(p);
        let is_known = known.contains(&name);
        let started = std::time::Instant::now();
        let result = badline_vk::slang::compile(p, &[]).and_then(|c| {
            if !c.warnings.is_empty() {
                warn += 1;
                println!("WARN {name}: {}", c.warnings.join("; "));
            }
            match &headless {
                Some(h) => h.render(c, original, viewport, frames).map(Some),
                None => Ok(None),
            }
        });
        if times {
            println!("TIME {name}: {:.2} s", started.elapsed().as_secs_f64());
        }
        match result {
            Ok(r) => {
                ok += 1;
                if is_known {
                    stale.push(name.clone());
                }
                if let Some(r) = r {
                    // fragment inputs the vertex stage never writes: corpus bug, already a WARN
                    let other: Vec<&String> =
                        r.validation.iter().filter(|m| !m.contains("has no Output declared there")).collect();
                    if !other.is_empty() {
                        invalid += 1;
                        println!("VALIDATION {name}: {} errors, first: {}", other.len(), other[0]);
                    }
                    if r.distinct() <= 1 {
                        flat += 1;
                        println!("FLAT {name}: {} passes, single colour output", r.passes);
                    }
                    if let Some(d) = &png_dir {
                        save_png(&d.join(name.replace('/', "__") + ".png"), r.width, r.height, &r.pixels);
                    }
                }
            }
            Err(e) => {
                fail += 1;
                let tag = if is_known { "KNOWN" } else { "FAIL" };
                println!("{tag} {name}: {}", e.lines().next().unwrap_or(""));
                if !is_known {
                    unexpected.push(name);
                }
            }
        }
    }
    println!(
        "{ok}/{} ok, {fail} failed ({} unexpected), {warn} with warnings{}, {:.1} s",
        presets.len(),
        unexpected.len(),
        if render { format!(", {invalid} with validation errors, {flat} flat") } else { String::new() },
        t0.elapsed().as_secs_f64()
    );
    for s in &stale {
        println!("STALE known entry passes: {s}");
    }
    if !unexpected.is_empty() || !stale.is_empty() {
        std::process::exit(1);
    }
}
