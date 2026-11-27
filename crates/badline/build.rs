//! Compiles the C++ tree (program/, driver/, guikit/, emulation/, deps/) in place and links
//! it into the badline binary.
//!
//! The `cc` crate only finds the compiler (CXX, default c++); flags come from sources.rs and
//! the profile's opt-level. Objects live in OUT_DIR/obj and are rebuilt only when the
//! source, a header from its depfile (-MMD) or the command line changed.
//!
//! Environment:
//!   BADLINE_SDL3_PREFIX  patched SDL3 (default builds/sdl3, see `cargo xtask sdl3`); used
//!                        when it contains lib/pkgconfig/sdl3.pc, else the system SDL3
//!   BADLINE_RPATH        RUNPATH of the binary (default: <SDL3 prefix>/lib when used;
//!                        the release tarball sets $ORIGIN/../lib)
//!   CXXFLAGS             appended to every compile command

#[path = "sources.rs"]
mod sources;

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::SystemTime;

const WATCHED_DIRS: &[&str] = &["program", "driver", "guikit", "emulation", "deps"];
const DEV_SHARE_FOLDERS: &[&str] = &["data", "fonts", "presets", "images", "sounds"];

struct Unit {
    src: PathBuf,
    obj: PathBuf,
    args: Vec<String>,
}

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().parent().unwrap().to_path_buf();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());

    for dir in WATCHED_DIRS {
        println!("cargo:rerun-if-changed={}", root.join(dir).display());
    }
    println!("cargo:rerun-if-changed=sources.rs");
    for var in [
        "CXX",
        "CXXFLAGS",
        "BADLINE_SDL3_PREFIX",
        "BADLINE_RPATH",
        "PKG_CONFIG_PATH",
    ] {
        println!("cargo:rerun-if-env-changed={var}");
    }

    // patched SDL3 (pkg-config path + RUNPATH)
    let sdl_prefix = env::var_os("BADLINE_SDL3_PREFIX")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("builds/sdl3"));
    let sdl_pc = sdl_prefix.join("lib/pkgconfig");
    let mut rpath = env::var("BADLINE_RPATH").ok();
    if sdl_pc.join("sdl3.pc").exists() {
        let old = env::var("PKG_CONFIG_PATH").unwrap_or_default();
        let new = if old.is_empty() {
            sdl_pc.display().to_string()
        } else {
            format!("{}:{old}", sdl_pc.display())
        };
        // SAFETY: build scripts are single threaded at this point
        unsafe { env::set_var("PKG_CONFIG_PATH", new) };
        if rpath.is_none() {
            rpath = Some(sdl_prefix.join("lib").display().to_string());
        }
        println!(
            "cargo:rerun-if-changed={}",
            sdl_pc.join("sdl3.pc").display()
        );
    } else {
        println!(
            "cargo:warning=patched SDL3 not found in {} (run `cargo xtask sdl3`); the system SDL3 drops files from some Wayland drag sources",
            sdl_prefix.display()
        );
    }

    let mut pkgs: HashMap<&str, Option<Pkg>> = HashMap::new();
    pkgs.insert(
        "sdl3",
        Some(Pkg::probe("sdl3").expect("pkg-config: sdl3 not found")),
    );
    pkgs.insert(
        "freetype2",
        Some(Pkg::probe("freetype2").expect("pkg-config: freetype2 not found (overlay UI)")),
    );
    pkgs.insert("openal", Pkg::probe("openal"));
    let curl = Pkg::probe("libcurl").expect("pkg-config: libcurl not found");

    // compiler + flags
    let compiler = cc::Build::new()
        .cpp(true)
        .cargo_metadata(false)
        .get_compiler();
    let cxx = compiler.path().to_path_buf();
    let opt = env::var("OPT_LEVEL").unwrap_or_else(|_| "3".into());
    let mut base: Vec<String> = vec!["-std=c++17".into()];
    match opt.as_str() {
        "0" => base.extend(["-O0".into(), "-g".into()]),
        "1" | "2" => base.extend(["-O2".into(), "-g".into()]),
        "s" | "z" => base.push("-Os".into()), // not used by our profiles
        _ => base.push("-O3".into()),         // release; no NDEBUG on purpose (speed)
    }
    let extra: Vec<String> = env::var("CXXFLAGS")
        .map(|f| f.split_whitespace().map(String::from).collect())
        .unwrap_or_default();
    let version = env::var("CARGO_PKG_VERSION").unwrap();

    let obj_dir = out.join("obj");
    let mut program_units = Vec::new();
    let mut lib_units = Vec::new();
    for group in sources::GROUPS {
        let mut flags: Vec<String> = Vec::new();
        for f in group.flags {
            if let Some(dir) = f.strip_prefix("-I@") {
                flags.push(format!("-I{}", root.join(dir).display()));
            } else if *f == "%version" {
                flags.push(format!("-DVERSION=\"{version}\""));
            } else if *f == "%openal" {
                if let Some(Some(p)) = pkgs.get("openal") {
                    flags.extend(p.cflags.iter().cloned());
                    flags.push("-DDRV_OPENAL".into());
                }
            } else if let Some(name) = f.strip_prefix('%') {
                let pkg = pkgs[name].as_ref().unwrap();
                flags.extend(pkg.cflags.iter().cloned());
            } else {
                flags.push(f.to_string());
            }
        }
        for file in group.files {
            let src = root.join(file);
            let obj = obj_dir.join(format!("{file}.o"));
            let mut args = Vec::new();
            args.extend(flags.iter().cloned());
            args.extend(base.iter().cloned());
            args.extend(extra.iter().cloned());
            let unit = Unit { src, obj, args };
            if group.program {
                program_units.push(unit);
            } else {
                lib_units.push(unit);
            }
        }
    }

    let all: Vec<&Unit> = program_units.iter().chain(lib_units.iter()).collect();
    let jobs: usize = env::var("NUM_JOBS")
        .ok()
        .and_then(|j| j.parse().ok())
        .unwrap_or(4);
    let rebuilt = compile_all(&cxx, &all, jobs);

    let ar = env::var("AR").unwrap_or_else(|_| "ar".into());
    archive(
        &ar,
        &out.join("libbadline_program.a"),
        &program_units,
        rebuilt,
    );
    archive(&ar, &out.join("libbadline_libs.a"), &lib_units, rebuilt);

    // link: program objects whole-archive (as before), then the libraries
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static:+whole-archive=badline_program");
    println!("cargo:rustc-link-lib=static=badline_libs");
    let mut link_pkgs: Vec<&Pkg> = vec![&curl];
    for name in ["freetype2", "sdl3", "openal"] {
        if let Some(Some(p)) = pkgs.get(name) {
            link_pkgs.push(p);
        }
    }
    for p in link_pkgs {
        for dir in &p.lib_dirs {
            println!("cargo:rustc-link-search=native={dir}");
        }
        for lib in &p.libs {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
    }
    for lib in ["GL", "pulse", "pthread", "rt", "dl", "stdc++", "m"] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
    // non-PIE executable as with the former CMake build (-no-pie); a global
    // `relocation-model=static` would also hit proc-macro crates (shared objects)
    println!("cargo:rustc-link-arg-bins=-no-pie");
    if let Some(rpath) = rpath {
        // RUNPATH (new dtags) like the former CMake build
        println!("cargo:rustc-link-arg-bins=-Wl,--enable-new-dtags,-rpath,{rpath}");
    }

    dev_share(&root, &out);
}

/// pkg-config result for one package.
struct Pkg {
    cflags: Vec<String>,
    lib_dirs: Vec<String>,
    libs: Vec<String>,
}

impl Pkg {
    fn probe(name: &str) -> Option<Pkg> {
        let run = |arg: &str| -> Option<Vec<String>> {
            let o = Command::new("pkg-config").args([arg, name]).output().ok()?;
            o.status.success().then(|| {
                String::from_utf8_lossy(&o.stdout)
                    .split_whitespace()
                    .map(String::from)
                    .collect()
            })
        };
        let cflags = run("--cflags")?;
        let lib_dirs = run("--libs-only-L")?
            .into_iter()
            .filter_map(|s| s.strip_prefix("-L").map(String::from))
            .collect();
        let libs = run("--libs-only-l")?
            .into_iter()
            .filter_map(|s| s.strip_prefix("-l").map(String::from))
            .collect();
        Some(Pkg {
            cflags,
            lib_dirs,
            libs,
        })
    }
}

fn mtime(p: &Path) -> Option<SystemTime> {
    fs::metadata(p).and_then(|m| m.modified()).ok()
}

/// Command line as stored next to the object (`.cmd`), to rebuild on flag changes.
fn command_line(cxx: &Path, u: &Unit) -> String {
    let mut s = cxx.display().to_string();
    for a in &u.args {
        s.push('\n');
        s.push_str(a);
    }
    s.push('\n');
    s.push_str(&u.src.display().to_string());
    s
}

fn up_to_date(cxx: &Path, u: &Unit) -> bool {
    let Some(obj_time) = mtime(&u.obj) else {
        return false;
    };
    let cmd_file = u.obj.with_extension("o.cmd");
    if fs::read_to_string(&cmd_file).ok().as_deref() != Some(command_line(cxx, u).as_str()) {
        return false;
    }
    let Ok(dep) = fs::read_to_string(u.obj.with_extension("o.d")) else {
        return false;
    };
    let deps = parse_depfile(&dep);
    if deps.is_empty() {
        return false;
    }
    deps.iter()
        .all(|d| mtime(Path::new(d)).is_some_and(|t| t <= obj_time))
}

/// Prerequisites of a make depfile written by -MMD (`obj: src hdr \` lines, `\ ` = space).
fn parse_depfile(text: &str) -> Vec<String> {
    let joined = text.replace("\\\n", " ");
    let Some((_, rest)) = joined.split_once(": ") else {
        return Vec::new();
    };
    let mut deps = Vec::new();
    let mut cur = String::new();
    let mut chars = rest.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\\' if chars.peek() == Some(&' ') => {
                cur.push(' ');
                chars.next();
            }
            c if c.is_whitespace() => {
                if !cur.is_empty() {
                    deps.push(std::mem::take(&mut cur));
                }
            }
            c => cur.push(c),
        }
    }
    if !cur.is_empty() {
        deps.push(cur);
    }
    deps
}

/// Compiles all stale units in parallel; returns the number of rebuilt objects.
fn compile_all(cxx: &Path, units: &[&Unit], jobs: usize) -> usize {
    let stale: Vec<&Unit> = units
        .iter()
        .copied()
        .filter(|u| !up_to_date(cxx, u))
        .collect();
    if stale.is_empty() {
        return 0;
    }
    let next = AtomicUsize::new(0);
    let errors: Mutex<Vec<String>> = Mutex::new(Vec::new());
    let warnings: Mutex<Vec<String>> = Mutex::new(Vec::new());
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1).min(stale.len()) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(u) = stale.get(i) else { break };
                    if let Err(e) = compile(cxx, u, &warnings) {
                        errors.lock().unwrap().push(e);
                    }
                }
            });
        }
    });
    for w in warnings.into_inner().unwrap() {
        for line in w.lines() {
            println!("cargo:warning={line}");
        }
    }
    let errors = errors.into_inner().unwrap();
    if !errors.is_empty() {
        for e in &errors {
            eprintln!("{e}");
        }
        panic!("{} C++ translation unit(s) failed to compile", errors.len());
    }
    stale.len()
}

fn compile(cxx: &Path, u: &Unit, warnings: &Mutex<Vec<String>>) -> Result<(), String> {
    fs::create_dir_all(u.obj.parent().unwrap()).map_err(|e| e.to_string())?;
    let dep = u.obj.with_extension("o.d");
    let cmd_file = u.obj.with_extension("o.cmd");
    let _ = fs::remove_file(&cmd_file);
    let out = Command::new(cxx)
        .args(&u.args)
        .arg("-MMD")
        .arg("-MF")
        .arg(&dep)
        .arg("-c")
        .arg(&u.src)
        .arg("-o")
        .arg(&u.obj)
        .output()
        .map_err(|e| format!("cannot run {}: {e}", cxx.display()))?;
    let stderr = String::from_utf8_lossy(&out.stderr).into_owned();
    if !out.status.success() {
        let _ = fs::remove_file(&u.obj);
        return Err(format!("error compiling {}:\n{stderr}", u.src.display()));
    }
    if !stderr.trim().is_empty() {
        warnings.lock().unwrap().push(stderr);
    }
    fs::write(&cmd_file, command_line(cxx, u)).map_err(|e| e.to_string())
}

/// (Re)creates a static archive when objects were rebuilt or its member list changed.
fn archive(ar: &str, lib: &Path, units: &[Unit], rebuilt: usize) {
    let list: String = units
        .iter()
        .map(|u| format!("{}\n", u.obj.display()))
        .collect();
    let list_file = lib.with_extension("a.list");
    let lib_time = mtime(lib);
    let fresh = rebuilt == 0
        && lib_time.is_some()
        && fs::read_to_string(&list_file).ok().as_deref() == Some(list.as_str())
        && units.iter().all(|u| mtime(&u.obj) <= lib_time);
    if fresh {
        return;
    }
    let _ = fs::remove_file(lib);
    let status = Command::new(ar)
        .arg("qcs") // append: members with equal basenames (images.cpp, socket.cpp) must not replace each other
        .arg(lib)
        .args(units.iter().map(|u| &u.obj))
        .status()
        .unwrap_or_else(|e| panic!("cannot run {ar}: {e}"));
    assert!(status.success(), "{ar} failed for {}", lib.display());
    fs::write(&list_file, list).unwrap();
}

/// Dev builds look for resources in <exe dir>/../share/badline (System::getResourceFolder):
/// target/<profile>/badline -> target/share/badline/* symlinks to data/.
fn dev_share(root: &Path, out: &Path) {
    // OUT_DIR = <target>/<profile>/build/<pkg>-<hash>/out
    let Some(profile_dir) = out.ancestors().nth(3) else {
        return;
    };
    let Some(target_dir) = profile_dir.parent() else {
        return;
    };
    let share = target_dir.join("share/badline");
    let _ = fs::create_dir_all(&share);
    for folder in DEV_SHARE_FOLDERS {
        let src = root.join("data").join(folder);
        let link = share.join(folder);
        if src.exists() && fs::symlink_metadata(&link).is_err() {
            let _ = std::os::unix::fs::symlink(&src, &link);
        }
    }
}
