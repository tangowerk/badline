#pragma once

// Main emulator window on SDL3 (replaces the GTK main window).
// All SDL window/event calls run on the UI (main) thread.

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include "../../guikit/api.h"

struct SDL_Window;
union SDL_Event;

struct SdlWindow {
    auto create() -> bool;
    auto destroy() -> void;
    // UI thread: destroy and create the window again for another video API (size/fullscreen kept)
    auto recreate(bool vulkan) -> bool;
    // reason of the last failed window creation
    auto lastError() const -> const std::string& { return error; }
    auto handle() const -> uintptr_t { return (uintptr_t)window; }

    // UI thread: pump SDL events, apply pending state (mouse capture, cursor)
    auto pumpEvents() -> void;

    auto fullScreen() const -> bool { return fullscreen; }
    auto setFullScreen(bool state) -> void;
    auto focused() const -> bool { return hasFocus; }
    auto setTitle(const std::string& title) -> void;

    // refresh rate (Hz) of the display showing the window, 0 if unknown; any thread
    auto refreshRate() const -> float { return currentRefreshRate; }
    // set when the refresh rate changed; the emu loop re-evaluates the frame pacing
    std::atomic<bool> refreshChanged{false};

    // mouse position in drawable pixels (lightpen/gun); thread-safe snapshot
    auto mousePosition() -> GUIKIT::Position;

    // pixel size of the drawable
    auto drawableSize(int& w, int& h) -> void;

    // persist window size/fullscreen in the global settings
    auto saveState() -> void;

    // missing Commodore ROMs: Return downloads them (background thread), Esc skips
    auto askRomDownload(const std::vector<std::string>& missing) -> void;

private:
    auto createWindow(bool vulkan) -> bool;
    auto showWindow() -> void;
    auto handleEvent(SDL_Event& ev) -> void;
    auto updateRefreshRate() -> void;
    auto handleDrop(SDL_Event& ev) -> void;
    auto updateMouseCapture() -> void;
    auto requestQuit() -> void;
    auto handleRomPrompt(SDL_Event& ev) -> bool;
    auto romDownloadFinished() -> void;
    auto showText(const std::string& text, bool warn, unsigned seconds) -> void;

    enum class RomPrompt { None, Asking, Downloading } romPrompt = RomPrompt::None;
    std::atomic<bool> romDownloadDone{false};
    bool romDownloadOk = false;
    std::string romDownloadError;

    SDL_Window* window = nullptr;
    std::string error;
    std::atomic<float> currentRefreshRate{0.0f};
    bool fullscreen = false;
    bool hasFocus = false;
    bool captured = false;
    bool cursorHidden = false;
    bool textInput = false;
    uint64_t lastMouseMove = 0;
    std::atomic<int> mouseX{0};
    std::atomic<int> mouseY{0};
    int windowedWidth = 0;
    int windowedHeight = 0;

    // drag & drop: SDL3 delivers one DROP_FILE per file between BEGIN and COMPLETE
    std::vector<std::string> dropFiles;
    bool dragging = false;

};

extern SdlWindow* sdlWindow;
