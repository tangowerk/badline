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
    auto handle() const -> uintptr_t { return (uintptr_t)window; }

    // UI thread: pump SDL events, apply pending state (mouse capture, cursor)
    auto pumpEvents() -> void;

    auto fullScreen() const -> bool { return fullscreen; }
    auto setFullScreen(bool state) -> void;
    auto focused() const -> bool { return hasFocus; }
    auto setTitle(const std::string& title) -> void;

    // mouse position in drawable pixels (lightpen/gun); thread-safe snapshot
    auto mousePosition() -> GUIKIT::Position;

    // pixel size of the drawable
    auto drawableSize(int& w, int& h) -> void;

    auto loadSplashScreen() -> void;
    auto loadDragnDropOverlay() -> void;

    // persist window size/fullscreen in the global settings
    auto saveState() -> void;

private:
    auto handleEvent(SDL_Event& ev) -> void;
    auto handleDrop(SDL_Event& ev) -> void;
    auto updateMouseCapture() -> void;
    auto requestQuit() -> void;
    auto splashHit(float x, float y) -> int;

    SDL_Window* window = nullptr;
    bool fullscreen = false;
    bool hasFocus = false;
    bool captured = false;
    bool cursorHidden = false;
    uint64_t lastMouseMove = 0;
    std::atomic<int> mouseX{0};
    std::atomic<int> mouseY{0};
    int windowedWidth = 0;
    int windowedHeight = 0;

    // drag & drop: SDL3 delivers one DROP_FILE per file between BEGIN and COMPLETE
    std::vector<std::string> dropFiles;
    bool dragging = false;
    int dropZone = 0;

    GUIKIT::Image splash;
    GUIKIT::Image dndOverlays[2];
};

extern SdlWindow* sdlWindow;
