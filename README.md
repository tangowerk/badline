<p align="center"><img src="data/img/badline_1024.png" width="128" alt="Badline icon"></p>

# Badline

**A cycle-accurate Commodore 64 emulator for Linux – built to be played, scripted and debugged.**

Badline is a fork of [**Denise**](https://github.com/piciji/denise), the
excellent C64 and Amiga emulator by **PiCiJi**. Denise did the hard part: a
cycle-exact C64 core, precise drive emulation, a slang shader pipeline and years
of careful accuracy work. Badline would not exist without it. Thank you, PiCiJi,
and thanks to everyone who contributed to Denise over the years.

Badline takes that core in a different direction: **C64 only, Linux only**, a
modern SDL3/Wayland front end, and first-class support for **automation and
development** – an embedded [MCP](https://modelcontextprotocol.io) server,
a headless mode for test pipelines and the VICE-compatible binary monitor.

> **Status: 0.1.0 – early preview.** The emulation is mature (it is Denise's),
> the front end is not yet. The in-window menu (F12) can load media, the other
> settings pages are still previews – see
> [Current state](#current-state) before you try it.

*Why the name?* A **badline** is the raster line on which the VIC-II steals the
bus from the CPU to fetch character data – the moment every C64 coder learns to
count cycles around. It is where timing gets exact, and that is what this
project is about.

---

## Scope and goals

**What Badline is**

- A **pure C64 emulator**: C64/C64C (PAL/NTSC), 1541/1541-II/1541-C/1570/1571/1581
  drives, tape, cartridges and expansions – all from Denise's accurate core.
- **For players**: a lean window, fullscreen, gamepads, drag & drop, CRT shaders.
- **For developers and agents**: debug the C64 and its drives through MCP or the
  binary monitor, run it headless and unthrottled in CI, drive it from scripts
  or AI agents.

**Goals** (in order of priority)

1. **Maximum compatibility** with C64 software. Real hardware is the
   reference; where no hardware results exist, VICE x64sc is. Accuracy is never
   traded for speed – there is one accurate core and no "fast mode".
2. **Maximum performance** of the host side: low CPU load at real speed, low
   input latency, high warp throughput, fast start-up and a small memory
   footprint.
3. **Lean, resource-friendly code**: Badline now develops independently of
   Denise; inherited code is questioned, fixed and optimized along these
   priorities, and parts are rewritten in **Rust** where that serves them.

Along the way: a fast **in-window overlay** instead of native dialogs, the
debuggers as an overlay **split view** with a VICE-style **text monitor**, and
MCP and the binary monitor for external tools. Linux-native: Wayland first,
SDL3, no Windows/macOS code paths.

**Non-goals**: Amiga emulation (removed), other Commodore machines, Windows/macOS.

---

## Current state

| Area | State in 0.1.0 |
|---|---|
| C64 emulation | Denise 2.9 core: cycle-exact VIC-II, reSID / libresidfp, CIA, drives with true drive emulation, tape, savestates, run-ahead |
| Media | D64, G64, P64, D71, G71, D81, TAP, T64, PRG, CRT, REU images; archives |
| Cartridges | EasyFlash, Retro Replay, Action Replay-type freezers, GMod, Trilogic Expert, REU, GeoRAM and more (from Denise) |
| Window | SDL3, native Wayland (EGL) with X11 fallback, HiDPI, fullscreen (F11 / Alt+Enter), size remembered |
| Video | Vulkan renderer (Rust, default): C64 colour decoding and PAL pass on the GPU, RetroArch slang presets (libretro collection), frame pacing (the display clocks the emulation on integer-multiple refresh rates, VRR, frame blending); OpenGL 3.2 renderer (being removed) |
| Input | Keyboard (positional), mouse capture (middle button), gamepads with hotplug – the first pad drives joystick #1 |
| Drag & drop | Drop images onto the window: slot overlay for drive/autostart |
| Automation | Embedded MCP server, 32 tools; headless mode (no display server needed); VICE binary monitor (API v2) |
| Overlay menu | F12 / gamepad Guide: Media page (drives, cartridge, tape, file browser with archives and directory preview), reset, quit. Other pages are previews. |
| Settings | Settings without an overlay page yet live in `~/.config/badline/*.ini` and can be changed via MCP. |

**Known limitations**

- Only the Media page of the overlay works; video, audio, input and system
  settings have no UI yet (the old GTK dialogs were removed).
- Keyboard layouts: mapping is positional; some symbols are wrong on non-US
  layouts (e.g. `"` on German keyboards). A dedicated milestone will fix this.
- No status bar (drive LEDs, track) until the overlay HUD arrives.

---

## ROMs

Badline does **not** include the Commodore ROMs – they are copyrighted by
their rights holders and not free software.

On first start Badline detects the missing ROMs and asks:

```
C64 ROMs missing. Return: download from zimmers.net   Esc: skip
```

Press **Return** and Badline downloads the original Kernal, BASIC, character
and drive ROMs from [zimmers.net](http://www.zimmers.net/anonftp/pub/cbm/firmware/)
(falling back to the VICE source mirror), verifies each file against a fixed
SHA-1 checksum and stores it in `~/.local/share/badline/roms/`. Nothing is
downloaded without your confirmation.

Alternatives:

- `badline -fetch-roms` – download non-interactively (scripts, headless).
- Copy your own ROMs into `~/.local/share/badline/roms/` (names `Kernal`,
  `Basic`, `Char`, `VC1541-II`, `VC1541`, `VC1541-C`, `VC1570`, `VC1571`,
  `VC1581`, or the usual zimmers/VICE file names such as `kernal.901227-03.bin`).
- `badline -rom-dir <path>` – use an additional ROM folder.

Using the ROMs is your responsibility.

---

## Shaders

With the Vulkan renderer Badline runs RetroArch slang presets (`.slangp`): CRT
masks, scanlines, bezels (Mega Bezel, koko-aio), NTSC/PAL signal emulation and
more. Pick one in the menu (F12 → Video → Preset); the browser shows

- **System**: the libretro collection of your distribution
  (`sudo pacman -S libretro-shaders-slang`),
- **Downloaded**: the same collection fetched by Badline (menu entry
  "Download" or `badline -fetch-shaders`; a pinned libretro/slang-shaders
  commit from github.com, 52 MB, SHA-1 checked, stored in
  `~/.local/share/badline/shaders/`),
- **Folder**: any folder with your own presets.

The chosen preset is saved; `badline -shader <file.slangp|none>` overrides it
for one session. Badline's own C64 PAL pass (Video → PAL pass) runs before the
preset.

---

## Download

Releases on GitHub ship `badline-<version>-linux-x86_64.tar.gz`, built on current
Arch Linux with the patched SDL3 included. Only current rolling distributions
(Arch and derivatives) are supported. Unpack anywhere and run `bin/badline`; the
other libraries come from the system:

```
sudo pacman -S --needed libpulse curl openal libglvnd vulkan-icd-loader glslang
```

`cargo xtask dist` builds the same tarball (used by the GitHub Actions
workflow).

---

## Building

Requirements (Arch Linux package names; only current rolling distributions are supported):

```
rust (or rustup) gcc pkgconf git curl libpulse vulkan-icd-loader glslang
for the patched SDL3: cmake ninja
optional: openal
```

Cargo drives the whole build; `crates/badline/build.rs` compiles the C++ sources
(only changed files are recompiled).

```sh
cargo xtask sdl3             # once: patched SDL3 in builds/sdl3 (picked up automatically)
cargo build --release
./target/release/badline
```

`cargo build` (dev profile) compiles the C++ code with `-O2 -g`, so it still runs at
full speed; `cargo build --profile gdb` builds everything unoptimised for debugging.
The build links the `data/` resources into `target/share/badline`, so the binary runs
straight from the build tree. Without `builds/sdl3` the system SDL3 is used (package
`sdl3`); file drops from some Wayland sources then fail (see `tools/patches/`).
`cargo xtask install` installs to `/usr/local` (binary, data, desktop file, icons;
`--prefix DIR` for another location, `--mime` adds the MIME types; for a root-owned
prefix build first and run `sudo cargo xtask install --no-build`).

---

## Usage

```sh
badline game.d64                 # autostart an image
badline -fullscreen demo.prg
badline -mcp                     # with MCP server on 127.0.0.1:6464
badline -headless -fetch-roms    # no window/audio, MCP on, ROM download
badline -headless -unthrottled   # as fast as possible (CI)
badline -binarymonitor           # VICE binary monitor for C64 Studio, VS64, ...
badline -renderer gl             # OpenGL renderer for this session (default: Vulkan)
badline -shader crt-royale.slangp  # slang preset for this session
badline -help                    # all options
```

**Default keys**: F11 or Alt+Enter = fullscreen, middle mouse button = capture
mouse, numeric keypad = joystick #1 (8/2/4/6, 0 = fire). The C64 keyboard is
mapped by key position.

**Smooth scrolling** (Vulkan): on a display whose refresh is a multiple of the
C64 rate (PAL on 100 Hz, NTSC on 60/120 Hz) the display clocks the emulation.
Otherwise frames are presented at exact target times, which a VRR display
follows (on Hyprland VRR is used in fullscreen with `misc:vrr = 2`, or for
every window with `1`). Without VRR each display refresh shows a mix of the
two C64 frames around it, weighted by time (frame blending: even motion with
slight ghosting, about one frame more latency; `BADLINE_VK_BLEND=off` turns it
off).

**Testing**: `tests/mcp_smoke.py` runs every MCP tool against a fresh instance
(`--headless` for the headless variant, `--renderer vulkan` for the Vulkan
renderer); `tests/renderer_ab.py` compares OpenGL and Vulkan screenshots;
`tests/pacing_check.py` measures frame pacing with `tests/pacing/softscroll.prg`.

---

## MCP: let agents drive the C64

Start Badline with `-mcp [port]` (or `-headless`) and connect any MCP client to
`http://127.0.0.1:6464/mcp` (Streamable HTTP, localhost only). For Claude Code:

```sh
claude mcp add --transport http badline http://127.0.0.1:6464/mcp
```

Tools include:

- **Control & media**: `status`, `reset`, `pause`/`resume`, `warp`, `load_media`
  (with autostart), `eject`, `savestate_save`/`savestate_load`, `renderer`
  (OpenGL/Vulkan), `pacing` (frame pacing state and statistics), `quit`
- **Input & observation**: `type_text`, `key`, `joystick`, `run_frames`,
  `wait_for_text`, `read_screen` (screen RAM as text), `screenshot` (raw or as
  rendered with shaders)
- **Debugging** (C64 and drives 8–11): `breakpoint_set/list/delete` (exec,
  read/write watchpoints, conditions, hit counts), `continue`, `break`, `step`
  (into/over/out), `registers`, `memory_read/write`, `load_memory`,
  `disassemble`, `cpu_history`, `chip_state` (VIC, SID, CIAs, VIAs)

---

## Roadmap

- **Overlay UI** (next): framework, then pages for media, savestates/reset,
  video/audio and system/input; status HUD.
- **Keyboard layouts**: correct symbolic/positional mapping for DE/US/FR.
- **Debugger in the overlay**: split view (picture + disassembler, registers,
  memory …) and a text monitor with VICE-like commands; label files.
- **SDL3 audio** evaluation (PipeWire) against the current PulseAudio driver.

See [CHANGELOG.md](CHANGELOG.md) for what changed since Denise.

---

## License and credits

Badline is licensed under the **GNU General Public License v3.0 or later**,
like Denise – see [LICENSE](LICENSE). Copyright of the original code remains
with PiCiJi and the Denise contributors; Badline's changes are © the Badline
contributors.

Third-party components (reSID, libresidfp, VICE-derived code, glslang,
SPIRV-Cross, cpp-httplib, fonts, sounds, shaders …) and their licenses are
listed in [NOTICE.md](NOTICE.md). The C64 Pro fonts by Style are included
unmodified under their own license.

Denise's original README and complete changelog are preserved in
[docs/DENISE-HISTORY.md](docs/DENISE-HISTORY.md). If you want the full
C64 **and** Amiga experience on Windows, macOS or Linux, use
[Denise](https://github.com/piciji/denise) – and consider supporting its author.
