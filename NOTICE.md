# Notices, licenses and credits

Badline is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version (GPL-3.0-or-later). See [LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

## Denise

Badline is a fork of **[Denise](https://github.com/piciji/denise)** by
**PiCiJi**, released under GPL-3.0-or-later. The emulation core
(`emulation/libc64`), the video/shader pipeline, the GUI toolkit and most of the
program layer are Denise's work and keep its copyright. Badline's changes are
released under the same license.

## Third-party components shipped in this repository

| Component | Where | License |
|---|---|---|
| reSID by Dag Lem | `emulation/libc64/sid` | GPL-2.0-or-later |
| libresidfp by Leandro Nini (https://github.com/libsidplayfp/libresidfp) | `deps/libresidfp` | GPL-2.0-or-later |
| VIC-II cycle emulation, based on the VICE implementation (https://vice-emu.sourceforge.io) | `emulation/libc64/vicII` | GPL-2.0-or-later |
| SuperCPU 64 ROM V0.07 by SOCI/Singular (from VICE) | `data/data/scpu64` | GPL-2.0-or-later |
| koko-aio shader presets (https://github.com/kokoko3k/koko-aio-slang) | `data/presets/bezel` | GPL-3.0 |
| Cosine and sinc resamplers from RetroArch (https://github.com/libretro/RetroArch) | `program/audio` | GPL-3.0 |
| Chamberlin filter from Hoxs64 (https://www.hoxs64.net) | `emulation/libc64/sid` | see source |
| P64 by BeRo, fpaq0 by Matt Mahoney | `emulation/libc64/disk` | see source |
| glslang (Khronos) | `deps/glslang` | BSD-3-Clause and others, see `deps/glslang/LICENSE.txt` |
| SPIRV-Cross (Khronos) | `deps/SPIRV-Cross` | Apache-2.0, see `deps/SPIRV-Cross/LICENSE` |
| cpp-httplib by Yuji Hirose | `deps/httplib.h` | MIT |
| cgif by Daniel Löbl | `deps/cgif` | MIT, see `deps/cgif/LICENSE.txt` |
| lhasa (LHA decoder) by Simon Howard | `deps/lha` | ISC |
| stb_image_write / stb_image_resize2 by Sean Barrett | `deps/stb` | MIT or public domain |
| C64 Pro / C64 Pro Mono fonts, C64 TrueType v1.2.1 by Style (https://style64.org/c64-truetype) | `data/fonts` | Style license, not GPL – see `data/fonts/C64_TrueType-LICENSE.txt` |
| Source Code Pro (Adobe) | `data/fonts` | SIL Open Font License 1.1, see `data/fonts/SourceCodePro-LICENSE.md` |
| Floppy sounds by Trackers-World.NET | `data/sounds` | CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/) |
| UI translations by Ben (French), Ulgon (Japanese), Ferenc (Hungarian), thecodeblasters (Spanish), Luigi (Italian), Diego (Esperanto) | `data/translation` | contributed to Denise |

## Libraries linked from the system

SDL3 (zlib), libcurl (curl license), GTK 3 / GLib (LGPL-2.1-or-later,
still used internally until the GTK layer is removed), PulseAudio
(LGPL-2.1-or-later), FreeType (FTL or GPL-2.0), OpenAL Soft (LGPL-2.0-or-later,
optional), LAME (LGPL-2.0-or-later, optional), fontconfig, Xlib/Xrandr (MIT).

## Commodore ROMs

Badline does **not** ship the Commodore ROMs (Kernal, BASIC, character ROM,
1541/1570/1571/1581 DOS). Their copyright belongs to the respective rights
holders; they are not covered by the GPL. On request Badline downloads them
from public archives (zimmers.net, with the VICE source mirror as fallback),
checks each file against a fixed SHA-1 and stores it in the user's ROM folder.
Whether you may use these files is your responsibility.

## Earlier credits from Denise

Application logo and icon of Denise were created by Retrofan; they are not
part of Badline. Thanks to everyone credited in
[docs/DENISE-HISTORY.md](docs/DENISE-HISTORY.md) for their contributions to
Denise.
