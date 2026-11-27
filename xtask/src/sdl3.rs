//! Builds SDL3 from source with the patches in tools/patches/sdl3-*.patch and installs it
//! into a prefix (default builds/sdl3). The crate build uses builds/sdl3 automatically, so
//! dev builds get the patched SDL too. The prefix is reused as long as SDL version and
//! patches are unchanged (stamp file), otherwise SDL is rebuilt.

use crate::{Args, Result, absolute, exec, root};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const SDL_VERSION: &str = "3.4.18";

pub fn run(args: &[String]) -> Result {
    let mut a = Args::new(args);
    let prefix = a
        .value("--prefix")?
        .map(|p| absolute(&p))
        .unwrap_or_else(|| root().join("builds/sdl3"));
    a.finish()?;
    build(&prefix)
}

pub fn build(prefix: &Path) -> Result {
    let patches = patches()?;
    let stamp = format!("{SDL_VERSION} {}", sha1_of(&patches)?);
    let stamp_file = prefix.join(".badline-stamp");
    if prefix.join("lib/pkgconfig/sdl3.pc").exists()
        && fs::read_to_string(&stamp_file)
            .map(|s| s.trim() == stamp)
            .unwrap_or(false)
    {
        println!(
            "SDL3 {SDL_VERSION} (patched) already in {}",
            prefix.display()
        );
        return Ok(());
    }

    let src = PathBuf::from(format!("{}-src", prefix.display()));
    let _ = fs::remove_dir_all(&src);
    let _ = fs::remove_dir_all(prefix);
    exec(
        Command::new("git")
            .args([
                "clone",
                "--depth",
                "1",
                "--branch",
                &format!("release-{SDL_VERSION}"),
            ])
            .arg("https://github.com/libsdl-org/SDL.git")
            .arg(&src),
    )?;
    for p in &patches {
        println!("applying {}", p.file_name().unwrap().to_string_lossy());
        exec(Command::new("git").arg("-C").arg(&src).arg("apply").arg(p))?;
    }
    let build = src.join("build");
    exec(
        Command::new("cmake")
            .arg("-S")
            .arg(&src)
            .arg("-B")
            .arg(&build)
            .args(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
            .arg(format!("-DCMAKE_INSTALL_PREFIX={}", prefix.display()))
            .args([
                "-DCMAKE_INSTALL_LIBDIR=lib",
                "-DSDL_TESTS=OFF",
                "-DSDL_EXAMPLES=OFF",
                "-DSDL_STATIC=OFF",
            ]),
    )?;
    exec(Command::new("cmake").arg("--build").arg(&build))?;
    exec(Command::new("cmake").arg("--install").arg(&build))?;
    fs::write(&stamp_file, format!("{stamp}\n")).map_err(|e| e.to_string())
}

/// tools/patches/sdl3-*.patch, sorted like the shell glob did.
fn patches() -> Result<Vec<PathBuf>> {
    let dir = root().join("tools/patches");
    let mut v: Vec<PathBuf> = fs::read_dir(&dir)
        .map_err(|e| format!("{}: {e}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| {
            let n = p.file_name().unwrap().to_string_lossy();
            n.starts_with("sdl3-") && n.ends_with(".patch")
        })
        .collect();
    v.sort();
    Ok(v)
}

/// SHA-1 of the concatenated patches (same stamp as the former tools/build_sdl3.sh).
fn sha1_of(files: &[PathBuf]) -> Result<String> {
    let mut data = Vec::new();
    for f in files {
        data.extend(fs::read(f).map_err(|e| format!("{}: {e}", f.display()))?);
    }
    let mut child = Command::new("sha1sum")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot run sha1sum: {e}"))?;
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&data)
        .map_err(|e| e.to_string())?;
    let out = child.wait_with_output().map_err(|e| e.to_string())?;
    let s = String::from_utf8_lossy(&out.stdout);
    s.split_whitespace()
        .next()
        .map(String::from)
        .ok_or_else(|| "sha1sum failed".into())
}
