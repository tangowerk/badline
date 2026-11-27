#pragma once

// In-window overlay UI (F12 / gamepad Guide): sidebar + pages, pauses the emulation while open.
// Lives on the UI thread; in headless mode the state machine runs without drawing (MCP tests).

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"

union SDL_Event;
struct Json;

struct Overlay {
    using Nav = OverlayUi::Nav;

    struct Page {
        std::string id, title, subtitle;
        bool preview = false;
        // sidebar entry without content: runs on select (Resume, Quit)
        std::function<void()> action;
        std::function<void(Page&)> refresh; // when the page is shown
        std::vector<std::unique_ptr<OverlayUi::Widget>> widgets;
        int focus = -1;

        template<typename T> auto add(T* w) -> T* { widgets.emplace_back(w); return w; }
    };

    enum class Zone { Sidebar, Content };

    auto init() -> bool;

    auto isOpen() const -> bool { return opened; }
    auto open() -> void;
    auto close() -> void;
    auto toggle() -> void { opened ? close() : open(); }

    // window events (UI thread); true = consumed, not for the emulation
    auto handleEvent(const SDL_Event& ev, float pixelDensity) -> bool;
    auto wantsTextInput() const -> bool;

    // input in logical terms (also used by MCP)
    auto nav(Nav n) -> void;
    auto textInput(const std::string& s) -> void;
    auto pointer(int x, int y, int clicks) -> void; // clicks 0 = move only, logical coordinates
    auto wheel(int x, int y, int dy) -> void;
    auto selectPage(const std::string& id) -> bool;

    // window pixel size; computes scale and logical size
    auto setSize(int width, int height) -> void;
    // UI thread: animation + hand the frame to the video driver
    auto tick() -> void;

    auto describe(Json& out) -> void;
    auto scale() const -> int { return scaleFactor; }

    static auto navFromName(const std::string& name, Nav& out) -> bool;

private:
    auto buildPages() -> void;
    auto currentPage() -> Page& { return *pages[pageIndex]; }
    auto focusedWidget() -> OverlayUi::Widget*;
    auto moveFocus(int dir) -> bool;
    auto firstFocusable(Page& p) -> int;
    auto showPage(int index) -> void;
    auto activateSidebar() -> void;
    auto layout() -> void;
    auto draw(float alpha) -> std::shared_ptr<DRIVER::OverlayFrame>;
    auto hints() -> std::vector<OverlayUi::Hint>;
    auto dialogNav(Nav n) -> void;
    auto finishDialog(int button) -> void;
    auto notify(const std::string& text) -> void;
    auto ask(const std::string& title, const std::string& message, std::function<void()> onYes) -> void;
    auto sidebarRect(int index) const -> OverlayUi::Rect;
    auto hitWidget(int x, int y) -> int;
    auto canRunMachineAction() -> bool;

    auto resetMachine(bool hard) -> void;
    auto quitProgram() -> void;

    OverlayFont font;
    std::vector<std::unique_ptr<Page>> pages;
    int pageIndex = 0;
    Zone zone = Zone::Sidebar;
    std::unique_ptr<OverlayUi::Dialog> dialog;
    int hoverSidebar = -1;
    int hoverWidget = -1;

    bool opened = false;
    bool visible = false; // open or fading out
    bool dirty = true;
    float lastAlpha = -1.0f;
    std::chrono::steady_clock::time_point fadeStart;

    std::string message;
    std::chrono::steady_clock::time_point messageUntil;

    int pixelWidth = 0, pixelHeight = 0;
    int scaleFactor = 1;
    int logicalWidth = 640, logicalHeight = 400;
    int offsetX = 0, offsetY = 0;
    OverlayUi::Rect panel, content;
    unsigned atlasId = 1;

    int toggleScancode = 0;
    int toggleButton = -1;
};

extern Overlay* overlay;
