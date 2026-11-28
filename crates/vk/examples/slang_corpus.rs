//! Compiles every .slangp below a directory (parse, glslang, reflection, LUT decoding; no GPU).
//! `cargo run --release -p badline-vk --example slang_corpus -- <dir> [filter]`

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

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = PathBuf::from(args.get(1).expect("corpus directory"));
    let filter = args.get(2).cloned().unwrap_or_default();
    let mut presets = Vec::new();
    walk(&dir, &mut presets);
    presets.sort();
    presets.retain(|p| p.to_string_lossy().contains(&filter));
    let t0 = std::time::Instant::now();
    let (mut ok, mut fail, mut warn) = (0, 0, 0);
    for p in &presets {
        match badline_vk::slang::compile(p, &[]) {
            Ok(c) => {
                ok += 1;
                if !c.warnings.is_empty() {
                    warn += 1;
                    println!("WARN {}: {}", p.display(), c.warnings.join("; "));
                }
            }
            Err(e) => {
                fail += 1;
                println!("FAIL {}: {}", p.display(), e.lines().next().unwrap_or(""));
            }
        }
    }
    println!("{ok}/{} ok, {fail} failed, {warn} with warnings, {:.1} s", presets.len(), t0.elapsed().as_secs_f64());
}
