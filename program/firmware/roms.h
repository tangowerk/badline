#pragma once

// Commodore ROMs are not shipped with Badline. They are looked up in
// -rom-dir, the user ROM folder (~/.local/share/badline/roms) and the install
// data folder, and can be downloaded on request from public archives
// (zimmers.net, VICE mirror); only files with a known SHA-1 are stored.

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace Roms {

struct Rom {
    std::string name;                  // Badline firmware name (file name in the ROM folder)
    std::string sha1;
    unsigned size;
    std::vector<std::string> aliases;  // common file names (zimmers.net, VICE)
    std::vector<std::string> urls;     // first one wins, the others are fallbacks
};

auto list() -> const std::vector<Rom>&;
auto userDir() -> std::string;
auto searchDirs() -> std::vector<std::string>;

// existing file for a firmware name (or an alias), empty if none
auto find(const std::string& name) -> std::string;
// path to load: existing file or the expected location in the user folder
auto path(const std::string& name) -> std::string;
// Commodore ROMs that are not available
auto missing() -> std::vector<std::string>;
auto sourceHost() -> std::string;

// blocking; progress gets one line per file. Returns false if a ROM could not be fetched.
auto download(const std::function<void(const std::string&)>& progress, std::string& error) -> bool;

auto sha1(const uint8_t* data, size_t size) -> std::string;

}
