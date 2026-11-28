#pragma once

// In-window overlay UI (F12 / gamepad Guide): sidebar + pages, pauses the emulation while open.
// Lives on the UI thread; in headless mode the state machine runs without drawing (MCP tests).

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
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
        bool bare = false; // no title: the widgets fill the whole content area
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
    auto rightClick(int x, int y) -> void;
    auto selectPage(const std::string& id) -> bool;

    // window pixel size; computes scale and logical size
    auto setSize(int width, int height) -> void;
    // UI thread: animation + hand the frame to the video driver
    auto tick() -> void;
    // republish the current frame (new video driver)
    auto invalidate() -> void { dirty = true; frameShown = false; }

    auto describe(Json& out) -> void;
    auto scale() const -> int { return scaleFactor; }

    // screen text (messages, FPS, DRC), any thread: duration in seconds (0 = until replaced),
    // empty text clears; drawn in a window corner while the menu is closed, timed messages
    // go to the menu message line while it is open
    auto screenText(const std::string& text, unsigned duration, bool warn = false) -> void;
    // DRIVER::ScreenTextDescription::Position (0 bottom right .. 5 top left)
    auto setScreenTextPosition(unsigned position) -> void;
    // busy spinner (any thread): shown top left while at least one id is active
    auto busy(const std::string& id, bool on) -> void;

    // drag & drop tiles while a file is dragged over the window (UI thread, drop.cpp);
    // MCP simulates drags with the same calls
    auto dragBegin() -> void;
    auto dragMove(int x, int y) -> void; // logical coordinates
    auto dragEnd(const std::vector<std::string>& files) -> void; // empty: drag left the window
    auto isDragging() const -> bool { return dragActive; }
    // window coordinates (SDL, unscaled) -> logical overlay coordinates
    auto toLogical(float wx, float wy, float density, int& x, int& y) const -> void;

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

    struct DropTile {
        unsigned drive = 0; // 0 = drive 8
        std::string image;  // inserted image, empty if none
        OverlayUi::Rect tile, insert, start;
    };
    auto dropTiles() const -> std::vector<DropTile>;
    auto drawMenu(OverlayUi::Painter& p) -> void;
    auto drawScreenText(OverlayUi::Painter& p, const std::string& text, bool warn, bool timed) -> void;
    auto drawBusy(OverlayUi::Painter& p) -> void;
    // UI thread: takes over screen text/busy changes; true = a new frame is needed
    auto pollStatus() -> bool;
    // fade of the timed message (0 = none), alpha of what drawScreenText shows
    auto messageAlpha() const -> float;
    auto screenTextAlpha() const -> float;
    auto drawDrop(OverlayUi::Painter& p) -> void;
    auto dropHint() const -> std::string;
    auto describeDrop(Json& out) const -> void;

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
    float lastTextAlpha = 0.0f;
    std::chrono::steady_clock::time_point fadeStart;

    std::string message;
    std::chrono::steady_clock::time_point messageUntil;

    int pixelWidth = 0, pixelHeight = 0;
    int scaleFactor = 1;
    int logicalWidth = 640, logicalHeight = 400;
    int offsetX = 0, offsetY = 0;
    OverlayUi::Rect panel, content;
    unsigned atlasId = 1;

    bool dragActive = false;
    int dragHover = -1; // tile index * 2, + 1 for the start half
    bool frameShown = false;

    // written from any thread (statusMutex), taken over by pollStatus on the UI thread
    struct StatusState {
        std::string permText;   // FPS / DRC: shown until replaced
        bool permWarn = false;
        std::string msgText;    // timed message, covers the permanent text
        bool msgWarn = false;
        unsigned msgDuration = 0;
        uint64_t msgSerial = 0;
        unsigned position = 0;
        std::set<std::string> busy;
    };
    mutable std::mutex statusMutex;
    StatusState status;
    uint64_t msgSerial = 0;

    // UI thread copy
    std::string permText, msgText;
    bool permWarn = false, msgWarn = false;
    std::chrono::steady_clock::time_point msgStart, msgUntil;
    unsigned textPosition = 0;
    int permBoxWidth = 0; // permanent texts (FPS) keep their widest box, no jitter
    std::set<std::string> busyIds;
    std::chrono::steady_clock::time_point busyStart;
    int busyStep = -1;

    int toggleScancode = 0;
    int toggleButton = -1;
    bool triggerDown[2] = {false, false};
};

extern Overlay* overlay;

// screen text through the overlay (no-op without one, e.g. -debugcart), any thread
inline auto showScreenText(const std::string& text, unsigned duration, bool warn = false) -> void {
    if (overlay)
        overlay->screenText(text, duration, warn);
}
