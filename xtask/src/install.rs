//! `cargo xtask install`: release build and installation below a prefix. All destinations
//! are prefix-relative (also used to stage the release tarball and package tests).

use crate::{Args, Result, absolute, exec, root};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

pub fn run(args: &[String]) -> Result {
    let mut a = Args::new(args);
    let prefix = a
        .value("--prefix")?
        .map(|p| absolute(&p))
        .unwrap_or_else(|| PathBuf::from("/usr/local"));
    let mime = a.flag("--mime");
    let no_build = a.flag("--no-build");
    a.finish()?;
    let binary = if no_build {
        root().join("target/release/badline")
    } else {
        build_release(None, &[])?
    };
    install(&binary, &prefix, mime)
}

/// `cargo build --release -p badline`, optionally into another target dir and with extra
/// environment (e.g. BADLINE_RPATH); returns the binary.
pub fn build_release(target_dir: Option<&Path>, env: &[(&str, String)]) -> Result<PathBuf> {
    let cargo = std::env::var("CARGO").unwrap_or_else(|_| "cargo".into());
    let mut cmd = Command::new(cargo);
    cmd.current_dir(root())
        .args(["build", "--release", "--locked", "-p", "badline"]);
    if let Some(t) = target_dir {
        cmd.arg("--target-dir").arg(t);
    }
    for (k, v) in env {
        cmd.env(k, v);
    }
    exec(&mut cmd)?;
    Ok(target_dir
        .map(Path::to_path_buf)
        .unwrap_or_else(|| root().join("target"))
        .join("release/badline"))
}

pub fn install(binary: &Path, prefix: &Path, mime: bool) -> Result {
    let root = root();
    let data = root.join("data");
    let share = prefix.join("share");
    let res = share.join("badline");

    copy_file(binary, &prefix.join("bin/badline"), 0o755)?;

    if data.join("data").exists() {
        // currently empty (nothing shipped), still searched for ROMs
        copy_tree(&data.join("data"), &res.join("data"), &|_| true)?;
    }
    copy_tree(&data.join("fonts"), &res.join("fonts"), &|name| {
        name.ends_with(".ttf") || name.contains("LICENSE")
    })?;
    for folder in ["sounds"] {
        copy_tree(&data.join(folder), &res.join(folder), &|_| true)?;
    }
    for doc in ["LICENSE", "NOTICE.md", "README.md", "CHANGELOG.md"] {
        copy_file(&root.join(doc), &share.join("doc/badline").join(doc), 0o644)?;
    }
    copy_file(
        &data.join("badline.appdata.xml"),
        &share.join("appdata/badline.appdata.xml"),
        0o644,
    )?;
    let img = data.join("img");
    let icons = share.join("icons");
    copy_file(
        &img.join("badline.png"),
        &icons.join("hicolor/256x256/apps/badline.png"),
        0o644,
    )?;
    copy_file(
        &img.join("badline_48.png"),
        &icons.join("hicolor/48x48/apps/badline.png"),
        0o644,
    )?;
    copy_file(
        &img.join("badline_48.png"),
        &icons.join("badline.png"),
        0o644,
    )?;
    copy_file(
        &img.join("badline_32.png"),
        &icons.join("hicolor/32x32/apps/badline.png"),
        0o644,
    )?;
    copy_file(
        &data.join("badline.desktop"),
        &share.join("applications/badline.desktop"),
        0o755,
    )?;
    // best effort, as with the former CMake install (tool is missing on many systems)
    let _ = Command::new("update-icon-caches")
        .arg(icons.join("hicolor"))
        .status();

    if mime {
        copy_file(
            &data.join("application-x-badline.xml"),
            &share.join("mime/packages/application-x-badline.xml"),
            0o644,
        )?;
        exec(Command::new("update-mime-database").arg(share.join("mime")))?;
        exec(Command::new("update-desktop-database").arg(share.join("applications")))?;
    }
    println!("installed to {}", prefix.display());
    Ok(())
}

fn copy_file(src: &Path, dst: &Path, mode: u32) -> Result {
    let err = |e: std::io::Error| format!("{} -> {}: {e}", src.display(), dst.display());
    fs::create_dir_all(dst.parent().unwrap()).map_err(err)?;
    let _ = fs::remove_file(dst); // replace a running binary instead of writing into it
    fs::copy(src, dst).map_err(err)?;
    fs::set_permissions(dst, fs::Permissions::from_mode(mode)).map_err(err)
}

/// Copies a directory recursively; `keep` filters files by name (directories are kept).
fn copy_tree(src: &Path, dst: &Path, keep: &dyn Fn(&str) -> bool) -> Result {
    let entries = fs::read_dir(src).map_err(|e| format!("{}: {e}", src.display()))?;
    for e in entries {
        let e = e.map_err(|e| e.to_string())?;
        let path = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if path.is_dir() {
            copy_tree(&path, &dst.join(&name), keep)?;
        } else if keep(&name) {
            copy_file(&path, &dst.join(&name), 0o644)?;
        }
    }
    Ok(())
}
