//! Project tasks: `cargo xtask <command>` (alias in .cargo/config.toml).
//!
//!   sdl3    [--prefix DIR]                 patched SDL3 from source (default builds/sdl3)
//!   install [--prefix DIR] [--mime] [--no-build]
//!                                          release build + install (default /usr/local);
//!                                          --no-build installs target/release/badline as is
//!   dist    [--deps] [--sdl-prefix DIR]    relocatable release tarball in dist/

mod dist;
mod install;
mod sdl3;

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

pub type Result<T = ()> = std::result::Result<T, String>;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");

fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let cmd = args.next().unwrap_or_default();
    let rest: Vec<String> = args.collect();
    let result = match cmd.as_str() {
        "sdl3" => sdl3::run(&rest),
        "install" => install::run(&rest),
        "dist" => dist::run(&rest),
        _ => {
            eprintln!(
                "usage: cargo xtask <command>\n\
                 \x20 sdl3    [--prefix DIR]               patched SDL3 from source (default builds/sdl3)\n\
                 \x20 install [--prefix DIR] [--mime] [--no-build]  release build + install (default /usr/local)\n\
                 \x20 dist    [--deps] [--sdl-prefix DIR]  release tarball in dist/ (Arch Linux)"
            );
            return ExitCode::from(2);
        }
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("xtask {cmd}: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Repository root.
pub fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .to_path_buf()
}

/// Absolute path relative to the current directory (also for paths that do not exist yet).
pub fn absolute(p: &str) -> PathBuf {
    std::path::absolute(p).unwrap_or_else(|_| PathBuf::from(p))
}

/// Runs a command, fails on a non-zero exit status.
pub fn exec(cmd: &mut Command) -> Result {
    let status = cmd
        .status()
        .map_err(|e| format!("cannot run {cmd:?}: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{cmd:?} failed ({status})"))
    }
}

/// Runs a command and returns its stdout.
pub fn output(cmd: &mut Command) -> Result<String> {
    let out = cmd
        .output()
        .map_err(|e| format!("cannot run {cmd:?}: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "{cmd:?} failed: {}",
            String::from_utf8_lossy(&out.stderr)
        ));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Simple `--flag` / `--option value` parser.
pub struct Args {
    items: Vec<String>,
}

impl Args {
    pub fn new(items: &[String]) -> Args {
        Args {
            items: items.to_vec(),
        }
    }

    pub fn flag(&mut self, name: &str) -> bool {
        let before = self.items.len();
        self.items.retain(|a| a != name);
        self.items.len() != before
    }

    pub fn value(&mut self, name: &str) -> Result<Option<String>> {
        let Some(i) = self.items.iter().position(|a| a == name) else {
            return Ok(None);
        };
        if i + 1 >= self.items.len() {
            return Err(format!("{name} needs a value"));
        }
        let v = self.items.remove(i + 1);
        self.items.remove(i);
        Ok(Some(v))
    }

    pub fn finish(self) -> Result {
        match self.items.first() {
            Some(a) => Err(format!("unknown argument: {a}")),
            None => Ok(()),
        }
    }
}
