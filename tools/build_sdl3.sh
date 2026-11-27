#!/usr/bin/env bash
# Builds SDL3 from source with the patches in tools/patches/sdl3-*.patch and installs it
# into a prefix (default builds/sdl3). Used by tools/build_release.sh; CMake picks up
# builds/sdl3 automatically for dev builds, so the patched SDL is used there too.
#
#   tools/build_sdl3.sh [--prefix DIR]
#
# The prefix is reused as long as SDL version and patches are unchanged (stamp file);
# otherwise SDL is rebuilt.

set -euo pipefail

SDL_VERSION=3.4.18
root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$root/builds/sdl3"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) prefix="$(realpath -m "$2")"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

patches=("$root"/tools/patches/sdl3-*.patch)
stamp="$SDL_VERSION $(cat "${patches[@]}" | sha1sum | cut -d' ' -f1)"

if [ -f "$prefix/lib/pkgconfig/sdl3.pc" ] && [ "$(cat "$prefix/.badline-stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "SDL3 $SDL_VERSION (patched) already in $prefix"
    exit 0
fi

src="$prefix-src"
rm -rf "$src" "$prefix"
git clone --depth 1 --branch "release-$SDL_VERSION" https://github.com/libsdl-org/SDL.git "$src"
for p in "${patches[@]}"; do
    echo "applying $(basename "$p")"
    git -C "$src" apply "$p"
done
cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF
cmake --build "$src/build"
cmake --install "$src/build"
echo "$stamp" > "$prefix/.badline-stamp"
