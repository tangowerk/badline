#pragma once

// RetroArch slang presets of the Vulkan renderer (M-VK3c): preset sources (system collection,
// downloaded collection), the saved preset (C64 setting `video_slang_preset`, `-shader`
// overrides it for the session) and loading on a worker thread (compile + pipelines can take
// seconds for big presets; the busy spinner runs meanwhile).

#include <functional>
#include <string>

namespace SlangPresets {

// Arch `libretro-shaders-slang`: <XDG_DATA_DIRS>/libretro/shaders/shaders_slang, "" if missing
auto systemDir() -> std::string;
// collection fetched by -fetch-shaders / the overlay: <user data>/badline/shaders/slang-shaders,
// "" if not downloaded
auto downloadedDir() -> std::string;
auto downloadTarget() -> std::string;
// pinned libretro/slang-shaders commit (same as the Arch package at the time of the release)
auto downloadInfo() -> std::string;
// blocking; progress gets single lines. Downloads and unpacks the pinned archive (SHA-1 checked)
auto download(const std::function<void(const std::string&)>& progress, std::string& error) -> bool;

// Vulkan renderer with a window
auto supported() -> bool;

struct State {
    std::string path;   // active preset, "" = none
    std::string status; // "12 passes, 0.4 s" or the error of the last load
    bool error = false;
    bool loading = false;
    std::string requested; // path being loaded
    unsigned passes = 0;
};
auto state() -> State;

// preset of this session: -shader, else the saved setting
auto wanted() -> std::string;
// UI thread. Loads on the worker thread ("" = none); on success the path is saved (save=true).
auto load(const std::string& path, bool save = true) -> void;
// any thread, blocking (MCP): result = JSON summary or the error
auto loadNow(const std::string& path, bool save, std::string& result) -> bool;
// after the video driver was created: load the wanted preset (async)
auto applyWanted() -> void;
// before the video driver is deleted: wait for a running load
auto wait() -> void;
// UI thread: saves finished loads into the settings
auto poll() -> void;

}
