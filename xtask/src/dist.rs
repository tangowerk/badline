//! `cargo xtask dist`: relocatable Linux release tarball dist/badline-<version>-linux-x86_64.tar.gz.
//!
//! Built on current Arch Linux (CI container `archlinux:base-devel`); only current rolling
//! distributions are supported. The patched SDL3 is built from source and shipped in lib/
//! (RUNPATH $ORIGIN/../lib); everything else comes from the target system.
//! The build goes to target/dist, so the dev binary in target/release keeps its RUNPATH.

use crate::{Args, Result, VERSION, absolute, exec, install, output, root, sdl3};
use std::fs;
use std::path::Path;
use std::process::Command;

/// Arch Linux build dependencies (CI container `archlinux:base-devel`): Badline itself plus
/// the SDL3 backends (SDL loads most of them at runtime, the headers decide what is built).
const PACMAN_DEPS: &[&str] = &[
    "base-devel",
    "git",
    "cmake",
    "ninja",
    "pkgconf",
    "rustup",
    "libpulse",
    "curl",
    "openal",
    "libx11",
    "libxrandr",
    "libxfixes",
    "libxext",
    "libxcursor",
    "libxi",
    "libxss",
    "libxtst",
    "libxinerama",
    "wayland",
    "wayland-protocols",
    "libxkbcommon",
    "libdecor",
    "libglvnd",
    "glslang",
    "mesa",
    "libdrm",
    "pipewire",
    "alsa-lib",
    "systemd-libs",
    "dbus",
    "ibus",
    "vulkan-headers",
    "vulkan-icd-loader",
];

pub fn run(args: &[String]) -> Result {
    let mut a = Args::new(args);
    let deps = a.flag("--deps");
    let sdl_prefix = a
        .value("--sdl-prefix")?
        .map(|p| absolute(&p))
        .unwrap_or_else(|| root().join("builds/sdl3"));
    a.finish()?;

    let root = root();
    let name = format!("badline-{VERSION}-linux-x86_64");

    if deps {
        // root in the CI container, sudo elsewhere
        let root_user = output(Command::new("id").arg("-u"))?.trim() == "0";
        let mut cmd = if root_user {
            Command::new("pacman")
        } else {
            let mut c = Command::new("sudo");
            c.arg("pacman");
            c
        };
        exec(cmd.args(["-Syu", "--needed", "--noconfirm"]).args(PACMAN_DEPS))?;
    }

    sdl3::build(&sdl_prefix)?;

    let binary = install::build_release(
        Some(&root.join("target/dist")),
        &[
            ("BADLINE_RPATH", "$ORIGIN/../lib".into()),
            ("BADLINE_SDL3_PREFIX", sdl_prefix.display().to_string()),
        ],
    )?;

    let dist = root.join("dist");
    let stage = dist.join(&name);
    let _ = fs::remove_dir_all(&stage);
    install::install(&binary, &stage, false)?;

    // ship SDL3 (soname file + symlinks)
    let lib = stage.join("lib");
    fs::create_dir_all(&lib).map_err(|e| e.to_string())?;
    for e in fs::read_dir(sdl_prefix.join("lib")).map_err(|e| e.to_string())? {
        let e = e.map_err(|e| e.to_string())?;
        let fname = e.file_name();
        if !fname.to_string_lossy().starts_with("libSDL3.so.0") {
            continue;
        }
        let dst = lib.join(&fname);
        let meta = fs::symlink_metadata(e.path()).map_err(|e| e.to_string())?;
        if meta.file_type().is_symlink() {
            let target = fs::read_link(e.path()).map_err(|e| e.to_string())?;
            std::os::unix::fs::symlink(target, &dst).map_err(|e| e.to_string())?;
        } else {
            fs::copy(e.path(), &dst).map_err(|e| e.to_string())?;
        }
    }

    check(&stage)?;

    fs::write(stage.join("INSTALL.txt"), install_txt()).map_err(|e| e.to_string())?;
    let tarball = dist.join(format!("{name}.tar.gz"));
    exec(
        Command::new("tar")
            .arg("-C")
            .arg(&dist)
            .arg("-czf")
            .arg(&tarball)
            .arg(&name),
    )?;
    println!("dist/{name}.tar.gz");
    Ok(())
}

/// RUNPATH set, SDL3 resolves to the bundled copy, nothing missing.
fn check(stage: &Path) -> Result {
    let bin = stage.join("bin/badline");
    let dynamic = output(Command::new("readelf").arg("-d").arg(&bin))?;
    if !dynamic
        .lines()
        .any(|l| l.contains("RUNPATH") && l.contains("$ORIGIN/../lib"))
    {
        return Err("RUNPATH $ORIGIN/../lib missing".into());
    }
    let ldd = output(Command::new("ldd").arg(&bin))?;
    let bundled = format!(
        "libSDL3.so.0 => {}/bin/../lib/libSDL3.so.0",
        stage.display()
    );
    if !ldd.contains(&bundled) {
        return Err(format!(
            "libSDL3 does not resolve to the bundled copy:\n{ldd}"
        ));
    }
    let missing: Vec<&str> = ldd.lines().filter(|l| l.contains("not found")).collect();
    if !missing.is_empty() {
        return Err(format!("missing libraries:\n{}", missing.join("\n")));
    }
    Ok(())
}

fn install_txt() -> String {
    format!(
        "Badline {VERSION} for Linux x86_64 (built on current Arch Linux; only current
rolling distributions are supported)

Run in place:   ./bin/badline
Install:        copy the folder anywhere (e.g. ~/.local/opt/badline) and start bin/badline;
                share/applications/badline.desktop can be adapted for a launcher entry.

SDL3 is included (lib/, patched). The other libraries come from your system; on Arch:
  sudo pacman -S --needed libpulse curl openal libglvnd vulkan-icd-loader
(most desktops have them already).

On first start Badline offers to download the C64 ROMs (see share/doc/badline/README.md).
"
    )
}
