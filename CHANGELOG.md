# Changelog

All notable changes to Badline. Badline is a fork of
[Denise](https://github.com/piciji/denise) 2.9 by PiCiJi; Denise's own history
is preserved in [docs/DENISE-HISTORY.md](docs/DENISE-HISTORY.md).

## Unreleased

### Added

- **Frame blending on Vulkan (M-Pacing3).** Without VRR and without an integer
  multiple (e.g. PAL in a window on a 60 Hz display) every vblank now shows a
  phase-weighted mix of the two C64 frames around its display time, mixed in
  linear light, instead of the 5:6 judder. The emulation stays on the grid of
  M-Pacing2 and presents one picture per predicted vblank with a target time;
  the vblank grid is estimated from the present timestamps. Measured on a
  60 Hz panel (PAL, 60 s): one present per vblank, every display within 0.02 ms
  of its predicted vblank, no audio underruns. Costs about one C64 frame plus
  half a refresh of latency (Lowest latency stays the alternative). After a
  window/fullscreen switch, swapchain rebuild or refresh change a 2 s grid probe
  checks for VRR first (then `grid_vrr` as before). A present that slipped by a
  vblank (compositor hiccup) is caught up by leaving out one present.
  MCP `pacing` reports the state `blend` and blend phase statistics;
  `tests/pacing_check.py` checks them. `BADLINE_VK_BLEND=off` disables
  blending, `=sharp` mixes only around the middle of the phase (comparison
  variant); `BADLINE_VK_BLEND_OFFSET_US` overrides the target time offset.
  Screenshots keep showing the unmixed picture.

- **Busy spinner (M-VK2).** A small C64-style spinner (top left, overlay
  scale) shows while a long task runs; GL shader compilation uses it (Vulkan
  shader compilation will in M-VK3). It replaces the rotating progress image
  of Denise. MCP `overlay` action `busy` turns it on/off for tests; the
  overlay state lists the active ids (`busy`).
- MCP `overlay`: the state reports the current screen text (`screen_text`:
  text, warn, timed, visible); action `screen_text` shows a test message.

### Changed

- The Vulkan FIFO swapchain uses one image more (minimum + 2) so a compositor
  hiccup does not block the emulation thread in the acquire.

- **Glyph atlas in Rust (M-VK2).** The overlay fonts (C64 Pro, C64 Pro Mono)
  are rasterized by a new crate (`crates/glyphs`: ttf-parser plus a small
  pixel-exact rasterizer) instead of FreeType; the atlas is byte-identical to
  the FreeType one (checked for every glyph incl. the screen code set).
- **Screen text in C64 Pro (M-VK2).** Messages, FPS and DRC statistics are
  drawn by the overlay painter (both renderers) in the overlay look: C64 Pro,
  panel colours, warnings on red, sized with the overlay scale, in a window
  corner (`screen_text_position`, default bottom right; outside the picture
  area, so rendered screenshots no longer contain it). Messages fade in/out;
  while one shows, FPS/DRC wait underneath and come back afterwards (Denise
  dropped them until the next update). With the menu open, messages go to its
  message line. Removed settings: `screen_text_font`, `screen_text_findex`,
  `screen_text_fontsize`, `screen_text_color`, `screen_text_bgcolor`,
  `screen_warn_color`, `screen_warn_bgcolor`, padding/margin settings.
- FreeType is no longer a dependency (build, release container, package list);
  Source Code Pro and `data/images/progress.png` are gone. Vulkan lost its
  separate screen text texture and pipeline; GL its screen text and progress
  code plus unused shaders. Dead font list code (`MiscHelper` display fonts,
  unused since M9) removed.
- NOTICE: removed stale entries (GTK, FreeType, LAME, fontconfig,
  translations, Source Code Pro), added the Rust crates and libwayland-client.

### Fixed

- The saved FPS display setting (`fps_screen`) was only applied after a
  window/fullscreen switch, not at start (Badline bug since M9: the call went
  with the GTK status bar).
- Vulkan: a normal start stayed in the audio master fallback (old 5:6 judder,
  no DRC) until a display or fullscreen change: the pacing was decided before
  the swapchain reported target time support and never re-evaluated; it now is
  once that support changes (Badline bug, M-Pacing2; fullscreen VRR hid it).
- Vulkan: after a stall (hidden window) the renderer could stay in the audio
  master fallback for good: the recovery polled only the latest present, which
  is usually still queued; it now checks the first present after the stall
  (Badline bug, M-Pacing1).

## 0.2.0 – 2026-11-27

### Added

- **VRR and target-time pacing on Vulkan (M-Pacing2).** When the refresh is
  no integer multiple of the emulated rate (e.g. PAL on 60 Hz), every C64 frame
  waits for its slot on a steady grid at the emulated rate (monotonic clock) and
  is presented with that slot as absolute target time (`VK_EXT_present_timing`,
  Wayland commit-timing through Mesa); audio follows through dynamic rate
  control. With VRR (e.g. fullscreen with Hyprland `misc:vrr = 2`) the display
  follows the grid: PAL on a 40–60 Hz VRR panel scrolls smoothly, measured 60 s
  with every frame interval within 0.6 ms of 20 ms and ~5 ms present-to-display.
  Without VRR the known judder stays (frame blending follows in M-Pacing3);
  without target time support (X11) audio stays the clock. MCP `pacing` reports
  the state `grid` / `grid_vrr` (VRR detected from the display times) and grid
  interval statistics; `tests/pacing_check.py --toggle-fullscreen` checks
  window ↔ fullscreen switches.
- The Wayland window surface is marked as content type "game"
  (`wp_content_type_v1`, via the `wayland-client` crate on SDL's display), so
  compositors can enable VRR for it (Hyprland `misc:vrr = 3`) and keep cursor
  frames from breaking fullscreen VRR. `BADLINE_NO_CONTENT_TYPE` turns it off.
- `BADLINE_VK_DEBUG` also logs pacing state changes and renders blocking for
  more than 30 ms; `BADLINE_VK_LEAD_US` sets how early a grid frame is submitted
  before its target time (default 4000).
- **Frame pacing on Vulkan (M-Pacing1).** When the display refresh is an
  integer multiple of the emulated frame rate (within 0.5 %, e.g. PAL on a
  100 Hz monitor, NTSC on 60/120 Hz), the display clocks the emulation: every
  C64 frame is presented exactly n times, each present waits until the one two
  presents back is on screen, and the audio follows through dynamic rate
  control (non-blocking writes, buffer level averaged and regulated around its
  centre). Smooth scrolling no longer judders there. Other refresh rates keep
  audio as the clock (VRR and frame blending follow in M-Pacing2). New global
  setting `pacing` (0 Auto, 1 Lowest latency: audio clock, IMMEDIATE present,
  tearing allowed); the old sync settings stay for OpenGL only. The pacing
  decision is re-evaluated when the display or its refresh rate changes. When
  nothing is displayed any more (hidden window/workspace) the renderer stops
  waiting (also no blocking acquire) and audio keeps the time until presents
  show again; warp toggles keep the audio buffer setup.
- Present feedback in the Vulkan renderer: `VK_EXT_present_timing` with
  `VK_KHR_present_id2`/`present_wait2` (hand-written bindings, ash has none)
  gives real display timestamps per present; without it `present_id`/
  `present_wait` completion times are used.
- MCP tool `pacing`: current decision (state, repeat, queue depth, refresh,
  clock factor, feedback), `reset` and `stats` (display intervals in refresh
  periods, C64 frames shown longer/shorter than expected, skipped frames,
  hitches with submit times, audio underflows/drops and DRC range);
  `set_mode` switches `pacing`.
- `tests/pacing/softscroll.asm` (+ assembled `softscroll.prg`, acme): 8 sprites
  at 1-4 px/frame, a hardware-scrolled text row and a frame counter at $fb/$fc;
  `tests/pacing_check.py` measures a Vulkan instance with it and checks the
  acceptance (0 repeated/shortened/skipped frames, no audio xruns).

- **Vulkan renderer (preview).** A second video renderer written in Rust
  (`crates/vk`, Vulkan 1.3 via `ash`, Wayland and X11 surfaces) draws the C64
  picture, the overlay menu, drag & drop tiles and screen messages; screenshots
  without shaders are bit-identical to OpenGL. It is opt-in for now:
  `-renderer vulkan` (for one session), the `video_driver` setting
  (`OpenGL`/`Vulkan`), the new hotkey "Toggle Renderer (OpenGL/Vulkan)" (no
  default key) or the new MCP tool `renderer` switch it, also at runtime (the
  window is recreated, the emulation keeps running). If Vulkan cannot start,
  Badline exits with an error instead of falling back silently; a failed
  runtime switch returns to OpenGL. Warp and `video_sync` off use a
  non-blocking present mode. Not yet in Vulkan: shader presets, HDR,
  10 bit (YUV) input and the shader-build progress animation. Validation layers
  are enabled in dev builds when installed (`BADLINE_VK_VALIDATION=1/0`
  overrides).
- `tests/renderer_ab.py` compares rendered screenshots of both renderers;
  `tests/mcp_smoke.py --renderer vulkan` runs the smoke test on Vulkan.

### Removed

- **GTK is gone.** The GTK toolkit layer (guikit widgets, windows, menus,
  status bar, file and message dialogs), the GTK settings windows, the GTK
  debugger windows and the archive viewer were removed; they were unreachable
  since the SDL window (0.1.0). Badline no longer links GTK, fontconfig, Xrandr
  or X11 directly. guikit stays as a small GTK-free tools library (settings,
  files and archive decoders, strings, images, timers, main loop).
- Translation system (`data/translation`): all texts are English literals now.
- Dead features that only existed in the GTK UI: single-instance mode
  (`single_instance`), clipboard copy/paste from the menu, the system font
  fallback for screen texts (the bundled font is used), DMA debugger colouring,
  GTK status bar LED/volume widgets, the embedded GTK icon set. Hotkeys that
  opened GTK windows or drove the GTK debugger do nothing (their ids stay).
- USBSID-Pico stub, the Amiga debugger themes and the Windows/macOS branches
  in `emulation/tools` (unused `uuid`/`DLLoader` helpers deleted). Savestates
  keep their format.
- Dead splash screen code (start image overlay, `splash_screen` setting): its
  image was already removed with the Denise branding in 0.1.0, so nothing was
  shown since then.

### Changed

- Vulkan is now the default renderer (`video_driver` unset); OpenGL stays
  selectable (`-renderer gl`, `video_driver` = `OpenGL`) and is still needed
  for shader presets until M-VK3.
- PulseAudio: latency and frequency changes now apply at runtime (Denise only
  re-initialised the stream when an init handle was set, which is always 0);
  requests are 2 ms small so the writable size stays current; under video
  master the latency is the client buffer (no `ADJUST_LATENCY`, at least two
  frames + 10 ms); buffer underflows and dropped bytes are counted.
- Vulkan with a hidden window no longer slows the emulation to ~10 fps (every
  FIFO acquire ran into its 100 ms timeout).
- `.gitignore`: Python `__pycache__/` (pacing_check imports mcp_smoke).
- `tests/renderer_ab.py`: the screen text shot waits until the time based
  fade-in has settled (the renderers present with different latency).

- **Only current rolling distributions are supported.** The release tarball is
  now built on current Arch Linux (CI container `archlinux:base-devel`) instead
  of Ubuntu 24.04; `cargo xtask dist --deps` installs the build dependencies via
  pacman. The patched SDL3 stays bundled. README and INSTALL.txt list the Arch
  packages instead of Ubuntu/Debian ones.
- Headless mode (`-headless`) needs no display server anymore (no X/Wayland
  connection is opened), errors of the autoloader are printed to stderr and
  captured by MCP calls there too.
- The main loop is a plain loop with polled timers instead of the GLib main
  loop; the display refresh rate (adaptive sync, sync status hotkey) comes
  from SDL.
- The VICE binary monitor keeps its checkpoints itself instead of hidden GTK
  debugger windows; new smoke test `tests/binmon_smoke.py`.
- **Cargo builds Badline now; CMake is gone.** The repository is a Cargo
  workspace: `crates/badline` (the binary; a small Rust `main` hands over to
  the C++ program, its `build.rs` compiles the C++ sources with the same flags
  as before and only recompiles changed files) and `xtask` for project tasks.
  `cargo build --release` builds `target/release/badline`; the dev profile
  compiles C++ with `-O2 -g` (full speed), `--profile gdb` without
  optimisation. `cargo xtask sdl3`, `cargo xtask install [--prefix] [--mime]`
  and `cargo xtask dist` replace `tools/build_sdl3.sh`, `cmake --install` and
  `tools/build_release.sh`; the CI workflow uses them. The Rust toolchain is
  pinned in `rust-toolchain.toml` (1.99). Installed files and the release
  tarball contents are unchanged; warp speed is the same as the CMake build.
- The version is set once in `Cargo.toml` (was `program/program.h`).
- The binary is linked with `-no-pie` instead of building all Rust code with
  `relocation-model=static` (that broke proc-macro dependencies).
- Removed the untested Debian package script (`data/build_deb.sh`,
  `data/control`), the CMake `uninstall` target and the Denise-era entries in
  `.gitignore`.

### Fixed

- Pausing the emulation thread (settings changes, media, MCP calls) could
  leave the OpenGL context bound to it when both requests arrived at once
  (Denise bug); harmless before, a renderer switch now relies on it.

- Changed disks and flash cartridges are written back into the image file
  again when the question is turned off (`question_media_write = false`);
  since 0.1.0 they were never written back because the GTK question dialog
  was gone. With the question on (default) changes still stay unsaved until
  the overlay gets a dialog. (Badline bug since 0.1.0.)
- `-headless -debugcart` crashed on the first audio sample (no audio manager,
  warp mode without NoAudioOut). (Badline bug since 0.1.0.)
- Binary monitor: a failing checkpoint set left the emulation locked, and a
  condition was always applied to the C64 CPU checkpoint list even for drive
  checkpoints. (Denise bugs.)

## 0.1.3 – 2026-11-27

### Changed

- **Drag & drop tiles** rebuilt with the overlay look: while a file is dragged
  over the window, one tile per connected drive shows the drive number, the
  inserted image and two halves, INSERT (no reset) and START (reset + run from
  that drive, also drive 9–11). Disks go to the drive of the tile; tapes are
  inserted and cartridges/PRGs started from any tile; dropping outside the
  tiles autostarts as before. A hint line says what the hovered half does. The
  tiles also appear over the open menu (START closes it, INSERT keeps it open).
  Replaces Denise's four unlabelled picture slots (`mediaSlot*.png` and the GL
  drag & drop layer are gone).
- MCP `overlay` tool: actions `drag`, `drag_move`, `drop` and `drag_leave`
  simulate a file drag; the state lists the tiles under `drag`.

### Fixed

- Inserting more disks than drives are connected (drop of several disks
  without autostart) crashed the emulator: the extra drive was switched on
  without its ROM and without a power-on. Drive ROMs are now loaded into all
  drives, and a newly connected drive starts from power-on while the C64 keeps
  running. (Denise bug.)
- A tape or cartridge dropped on the second drive's slot was silently ignored
  (the slot index was applied to the tape/cartridge port), and inserting a
  cartridge or PRG without autostart tried to open the removed GTK settings
  window; they are now inserted or started like from the Media page. (Denise
  bug, partly Badline since 0.1.0.)
- `driver/video/splashScreen.h` includes `<atomic>` itself.

## 0.1.2 – 2026-11-27

### Fixed

- The window no longer freezes on the last picture after a reset while the
  overlay is open (e.g. cartridge eject from the Media page): a power cycle
  drops the last emulated frame, and while paused nothing was presented, so the
  overlay kept reacting to input but was never redrawn. A black picture is shown
  until the emulation runs again. (Badline bug since 0.1.0, overlay menu.)
- Ejecting a cartridge (overlay Media page, MCP `eject`) now unplugs the
  expansion as well. Before, an empty EasyFlash (or other cartridge type) stayed
  attached, the C64 crashed on the following hard reset and the setting was
  saved, so every later start crashed too. (Badline bug since 0.1.0.)
- Directory listings no longer leak file handles (`opendir` without `closedir`
  in the guikit file helpers); inserting media from a folder left up to ~25
  open handles per insert. (Denise bug.)

## 0.1.1 – 2026-11-27

### Removed

- **SuperCPU** (65816 accelerator) and **Final ChessCard** (65C02) support,
  including the SCPU debugger views, the SuperCPU turbo hotkey, the 65816
  registers in the binary monitor and the bundled free SuperCPU ROM. Configs
  that still select one of them start without an expansion; savestates made
  with one of them attached can no longer be loaded. Saved hotkey assignments
  keep their slots.
- **Audio DSP effects**: bass boost, echo, reverb and balance/stereo
  separation.
- **Audio recording** (MP3 via LAME, WAV), its menu entries, status bar
  indicator and hotkey; LAME is no longer a build or runtime dependency.
  Saved hotkey assignments keep their slots.
- **GIF screenshots** (single and animated); PNG, JPG, BMP and TGA remain.
  The bundled cgif library is gone.
- The **koko-aio bezel presets** (6 MB): they only worked with the separate
  koko-aio shader package. The internal presets remain.
- Amiga leftovers: Amiga floppy sound samples (they were still installed),
  the unused Amiga CIA (8520) source, an Amiga hint in a shader parameter.
- Denise build leftovers: Travis CI configuration and the Flatpak manifest.
- Unused parts of vendored libraries: SPIRV-Cross (tests, reference shaders,
  HLSL/MSL backends; 4340 → 15 files), glslang (tests, HLSL frontend, build
  files; 234 → 87 files), the Windows file of lha and two unused unity-build
  files.
- Four audio resamplers that were never compiled (cubic, hermite, linear,
  precise cosine).
- Dead hard-disk branches in file loading and autostart ordering (the C64 has
  no hard-disk media).

### Changed

- SDL3 is built by the new `tools/build_sdl3.sh` (version + patches, rebuilt
  when they change). Dev builds use `builds/sdl3` automatically when it exists;
  without it CMake warns and falls back to the system SDL3.
- **Project goals** updated in the README: Badline now develops independently
  of Denise (no more upstream cherry-picks); priorities are compatibility with
  C64 software first, host performance second, lean code third, with parts
  rewritten in Rust where that helps.
- The `data/data` folder is no longer installed while it is empty; installed
  builds still look there for ROMs from distribution packages.
- README, NOTICE and the release packaging list no longer mention the removed
  parts (LAME, cgif, koko-aio, SuperCPU ROM).

### Fixed

- Overlay menu in narrow windows (e.g. a tiled window): the scale factor now
  also keeps at least 480 logical pixels of width, so the Media page tabs no
  longer run off the right edge.
- Drag and drop of files from Wayland sources that also offer plain text
  (e.g. the Guppy file manager): SDL 3.4.18 requested the text variant and
  silently dropped plain paths. Badline now builds against an SDL3 with a small
  patch that keeps the file list (`tools/patches/`); the release tarball ships
  this SDL3.

## 0.1.0 – 2026-11-27

First preview release. The C64 emulation is Denise's; this release reshapes
everything around it.

Download: `badline-0.1.0-linux-x86_64.tar.gz` (built on Ubuntu 24.04, needs
glibc 2.39+, SDL3 included; see `INSTALL.txt` inside) or build from source.

### Identity

- Renamed to **Badline**: binary `badline`, window title, Wayland app id,
  desktop file, AppStream metadata, MIME type `application/x-badline-save`,
  MCP server name `badline`.
- Own version numbering starting at 0.1.0.
- Settings move to `~/.config/badline` (fresh start, no migration from Denise).
- Denise's logo, icons and splash screen are removed; Badline has a simple
  placeholder icon (`tools/make_icon.py`).

### Scope: C64 only, Linux only

- **Removed the Amiga emulation** (libami, Amiga debuggers for Agnus, Blitter,
  Copper and Paula, Amiga UI and command line options, Kickstart handling,
  Topaz fonts, Amiga presets and MIME types).
- **Removed Windows and macOS support** (Cocoa/WinAPI GUI toolkits, Direct3D and
  Metal drivers, MSVC/Xcode projects, AppVeyor CI) and resolved platform
  `#ifdef`s to Linux.
- **Removed USBSID-Pico** hardware SID support from the program layer (the
  libc64 stub stays for upstream compatibility).

### New window and input (SDL3)

- The GTK main window is replaced by an **SDL3 window**, native on Wayland (EGL),
  with X11 fallback via SDL and HiDPI support.
- New **OpenGL 3.2 core driver on SDL3**, ported from Denise's GLX driver: slang
  shaders, shader build thread with shared context, optional render thread,
  VSync, splash/progress/screen text, drag & drop overlay. GLX is gone.
- New **SDL3 input driver** replacing Xlib, XCB and udev: keyboard by scancode
  (letters follow the active layout), relative mouse mode for mouse capture,
  gamepads via the SDL gamepad API with raw joystick fallback and hotplug.
- **Gamepads are assigned automatically**: the first host pad drives emulated
  joystick #1, the second joystick #2 (D-pad and left stick, A = fire,
  B = button 2), next to the keypad mapping.
- **Fullscreen** on F11 or Alt+Enter; window size and fullscreen state are
  remembered.
- **Drag & drop** onto the window with Denise's slot overlay (drive/autostart).
- Messages that used to open GTK dialogs now go to the screen and stderr;
  questions take the safe default.
- Key mappings from the X11 era are reset once to new defaults.
- The settings dialogs, debugger windows and file dialogs of the GTK GUI are
  no longer reachable; they will be replaced by the in-window overlay.

### In-window overlay menu

- **F12 or the gamepad Guide button** opens a menu drawn inside the emulator
  window in a C64 look (C64 Pro fonts, Pepto palette, dimmed last frame); the
  emulation pauses while it is open. Keyboard, gamepad and mouse work
  throughout.
- **Media page** (fully working): tabs Autostart, Disk 8, Disk 9, Cartridge and
  Tape with the inserted media, eject and write protection; bookmarks and
  recent files; a file browser that enters ZIP, GZ, TAR and LHA archives, filters while
  you type and previews disk and tape directories in the C64 font – start a
  single program straight from the listing.
- Resume, soft/hard reset and Quit (with confirmation) work; the Savestates,
  Video, Audio, Input and System pages are **previews** without effect yet.
- MCP `overlay` tool to drive the menu; the overlay state appears in `status`.

### Automation and debugging

- **Embedded MCP server** (`-mcp [port]`, Streamable HTTP on 127.0.0.1, default
  port 6464) with 31 tools:
  - control and media: status, reset, pause/resume, warp, load_media (incl.
    autostart, file path or base64), eject, savestate save/load, quit
  - input and observation: type_text, key (with hold time), joystick,
    run_frames, wait_for_text, read_screen (screen RAM decoded as text),
    screenshot (raw frame buffer with palette, or rendered output with shaders)
  - debugging for the C64 CPU and drives 8–11: breakpoints and read/write
    watchpoints with conditions and hit counts, blocking continue, break,
    step into/over/out, registers, memory read/write with bank selection,
    load_memory, disassembler, instruction history, chip state of VIC-II, SID,
    CIA 1/2 and drive VIAs
- **Headless mode** (`-headless`, alias `-no-gui`): no window and no audio, MCP
  server on, real-time speed by default or `-unthrottled`; never writes the
  user settings. Suited for test pipelines and agents.
- MCP calls never block on dialogs: messages during calls and at startup are
  captured and returned.
- `tests/mcp_smoke.py`: smoke test for all MCP tools in window and headless mode.

### Build and release

- `tools/build_release.sh` builds a relocatable Linux tarball on Ubuntu 24.04
  with SDL3 built from source and bundled.
- The GitHub Actions workflow replaces Denise's Flatpak/macOS/Windows
  pipeline: build check on every push, release with these notes on `v*` tags.

### ROMs

- **Commodore ROMs are no longer shipped.** On first start Badline offers to
  download them (Return = yes, Esc = skip) from zimmers.net with the VICE source
  mirror as fallback; every file is checked against a fixed SHA-1 and stored in
  `~/.local/share/badline/roms`.
- New options `-fetch-roms` (non-interactive download) and `-rom-dir <path>`;
  ROMs are also found under common zimmers/VICE file names.
- MCP `status` reports missing ROMs.
- The Amiga AROS extension ROM is removed; the free SuperCPU ROM by
  SOCI/Singular stays.

### Fixes

- Drive breakpoints and steps now report their drive instead of the C64 CPU
  (`M6502New::controlBreaks`), so drive debugger windows and MCP see their own
  stops. (Denise bug.)
- Screenshots of the rendered output work on EGL (read before the buffer swap).
- Very short key taps are no longer lost between two input polls.
- Development builds find their resources (translations, images, fonts) when
  run from the build directory.

### Documentation and licensing

- New README, this changelog, full GPL-3.0 text in `LICENSE`, third-party
  licenses and credits in `NOTICE.md`, license texts for the bundled fonts.
- Denise's README and changelog archived in `docs/DENISE-HISTORY.md`.

### Known limitations (preview)

- Only the Media page of the overlay works; other settings via
  `~/.config/badline/*.ini` or MCP.
- Not every overlay input path is tested yet (pad triggers, mouse double/right
  click, Ctrl shortcuts under Wayland text input, two drives, tape preview).
- Keyboard mapping is positional; some symbols are wrong on non-US layouts
  (e.g. `"` on German keyboards).
- No status bar (drive LEDs, track display) until the overlay HUD.
- The koko-aio bezel presets need the full koko-aio shader package.
- GTK is still initialized internally, so an X display (XWayland) is required.
