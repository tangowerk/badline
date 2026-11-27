# Changelog

All notable changes to Badline. Badline is a fork of
[Denise](https://github.com/piciji/denise) 2.9 by PiCiJi; Denise's own history
is preserved in [docs/DENISE-HISTORY.md](docs/DENISE-HISTORY.md).

## Unreleased

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

- Headless mode (`-headless`) needs no display server anymore (no X/Wayland
  connection is opened), errors of the autoloader are printed to stderr and
  captured by MCP calls there too.
- The main loop is a plain loop with polled timers instead of the GLib main
  loop; the display refresh rate (adaptive sync, sync status hotkey) comes
  from SDL.
- The VICE binary monitor keeps its checkpoints itself instead of hidden GTK
  debugger windows; new smoke test `tests/binmon_smoke.py`.

### Fixed

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
