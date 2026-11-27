#!/usr/bin/env bash
# Builds the relocatable Linux release tarball dist/badline-<version>-linux-x86_64.tar.gz.
#
# Meant for Ubuntu 24.04 (GitHub Actions runner or container): the binary then runs on
# glibc 2.39+ systems. SDL3 is not packaged there, so it is built from source and shipped
# in lib/ (RPATH $ORIGIN/../lib); everything else comes from the target system.
#
#   tools/build_release.sh [--deps] [--sdl-prefix DIR]
#     --deps          install the apt build dependencies first (needs sudo)
#     --sdl-prefix    where SDL3 is built/installed (default: builds/sdl3, reused if present)

set -euo pipefail

SDL_VERSION=3.4.18
root="$(cd "$(dirname "$0")/.." && pwd)"
sdlPrefix="$root/builds/sdl3"
installDeps=0

while [ $# -gt 0 ]; do
    case "$1" in
        --deps) installDeps=1 ;;
        --sdl-prefix) sdlPrefix="$(realpath -m "$2")"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

version="$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$root/program/program.h")"
[ -n "$version" ] || { echo "version not found in program/program.h" >&2; exit 1; }
name="badline-$version-linux-x86_64"

if [ "$installDeps" = 1 ]; then
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        libgtk-3-dev libpulse-dev libcurl4-openssl-dev libfreetype-dev libfontconfig-dev \
        libx11-dev libxrandr-dev libxfixes-dev libxext-dev libopenal-dev \
        libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev \
        libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev \
        libxcursor-dev libxi-dev libxss-dev libxtst-dev libxinerama-dev \
        libpipewire-0.3-dev libasound2-dev libudev-dev libdbus-1-dev libibus-1.0-dev
fi

# SDL3 from source
if [ ! -f "$sdlPrefix/lib/pkgconfig/sdl3.pc" ]; then
    src="$sdlPrefix-src"
    rm -rf "$src"
    git clone --depth 1 --branch "release-$SDL_VERSION" https://github.com/libsdl-org/SDL.git "$src"
    cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$sdlPrefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF
    cmake --build "$src/build"
    cmake --install "$src/build"
fi

# Badline
build="$root/builds/dist"
stage="$root/dist/$name"
rm -rf "$build" "$stage"
PKG_CONFIG_PATH="$sdlPrefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'
cmake --build "$build"
cmake --install "$build" --prefix "$stage"

# ship SDL3 (soname file + symlink)
mkdir -p "$stage/lib"
cp -P "$sdlPrefix"/lib/libSDL3.so.0* "$stage/lib/"

# sanity: RPATH set, SDL3 resolves to the bundled copy, nothing missing
readelf -d "$stage/bin/badline" | grep -q 'RUNPATH.*\$ORIGIN/../lib' \
    || { echo "RUNPATH \$ORIGIN/../lib missing" >&2; exit 1; }
ldd "$stage/bin/badline" | grep -q "libSDL3.so.0 => $stage/bin/../lib/libSDL3.so.0" \
    || { echo "libSDL3 does not resolve to the bundled copy" >&2; ldd "$stage/bin/badline" >&2; exit 1; }
if ldd "$stage/bin/badline" | grep -q 'not found'; then
    ldd "$stage/bin/badline" | grep 'not found' >&2; exit 1
fi

cat > "$stage/INSTALL.txt" <<EOF
Badline $version for Linux x86_64 (built on Ubuntu 24.04, needs glibc 2.39 or newer)

Run in place:   ./bin/badline
Install:        copy the folder anywhere (e.g. ~/.local/opt/badline) and start bin/badline;
                share/applications/badline.desktop can be adapted for a launcher entry.

SDL3 is included (lib/). The other libraries come from your system; on Ubuntu/Debian:
  sudo apt install libgtk-3-0t64 libpulse0 libcurl4t64 libopenal1 \\
                   libfreetype6 libfontconfig1 libgl1 libegl1
(Debian 13 / Ubuntu 24.04 names; most desktops have them already.)

On first start Badline offers to download the C64 ROMs (see share/doc/badline/README.md).
EOF

tar -C "$root/dist" -czf "$root/dist/$name.tar.gz" "$name"
echo "dist/$name.tar.gz"
