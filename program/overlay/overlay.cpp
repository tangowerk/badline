#include "overlay.h"

#include <SDL3/SDL.h>
#include <algorithm>

#include "../program.h"
#include "../cmd/cmd.h"
#include "../thread/emuThread.h"
#include "../mcp/json.h"
#include "../mcp/server.h"
#include "../window/sdlWindow.h"
#include "../firmware/roms.h"
#include "media.h"

using namespace OverlayUi;

Overlay* overlay = nullptr;

namespace {

const unsigned PauseBit = 4; // Program::isPause: 1 user, 2 focus loss, 4 overlay
const auto FadeTime = std::chrono::milliseconds(120);
const int SidebarWidth = 112;
const int EntryHeight = 16;
const int MinLogicalHeight = 360;
const int MinLogicalWidth = 480; // narrow (tiled) windows: pages need this much room

// default window size of SdlWindow (headless layout)
const int DefaultWidth = 384 * 3;
const int DefaultHeight = 272 * 3;

struct NavName { const char* name; Overlay::Nav nav; };
const NavName NavNames[] = {
    {"up", Overlay::Nav::Up}, {"down", Overlay::Nav::Down}, {"left", Overlay::Nav::Left}, {"right", Overlay::Nav::Right},
    {"ok", Overlay::Nav::Ok}, {"back", Overlay::Nav::Back}, {"pageup", Overlay::Nav::PageUp}, {"pagedown", Overlay::Nav::PageDown},
    {"home", Overlay::Nav::Home}, {"end", Overlay::Nav::End}, {"backspace", Overlay::Nav::Backspace}, {"delete", Overlay::Nav::Delete},
    {"tab", Overlay::Nav::NextTab}, {"shift_tab", Overlay::Nav::PrevTab}, {"insert", Overlay::Nav::Insert},
    {"bookmark", Overlay::Nav::Bookmark}, {"all_files", Overlay::Nav::AllFiles}, {"eject", Overlay::Nav::Eject},
    {"parent", Overlay::Nav::Parent},
};

}

auto Overlay::navFromName(const std::string& name, Nav& out) -> bool {
    for (auto& n : NavNames)
        if (name == n.name) {
            out = n.nav;
            return true;
        }
    return false;
}

auto Overlay::init() -> bool {
    std::string error;
    if (!font.load(program->fontFolder(), error)) {
        std::fprintf(stderr, "Overlay disabled: %s\n", error.c_str());
        return false;
    }

    auto key = globalSettings->get<std::string>("overlay_key", "F12");
    toggleScancode = SDL_GetScancodeFromName(key.c_str());
    if (toggleScancode == SDL_SCANCODE_UNKNOWN)
        toggleScancode = SDL_SCANCODE_F12;
    auto button = globalSettings->get<std::string>("overlay_pad_button", "guide");
    toggleButton = button.empty() ? -1 : (int)SDL_GetGamepadButtonFromString(button.c_str());

    // disk directories as C64 screen codes (drawn with the C64 Pro glyphs at U+EE00)
    for (auto emu : emulators)
        if (auto c64 = dynamic_cast<LIBC64::Interface*>(emu))
            c64->convertPetsciiToScreencode(true);

    buildPages();

    if (sdlWindow) {
        int w, h;
        sdlWindow->drawableSize(w, h);
        setSize(w, h);
    } else
        setSize(globalSettings->get<int>("window_width", DefaultWidth, {160, 16384}),
                globalSettings->get<int>("window_height", DefaultHeight, {120, 16384}));
    return true;
}

// ------------------------------------------------------------ pages

auto Overlay::buildPages() -> void {
    auto page = [this](const std::string& id, const std::string& title, bool preview = false) -> Page& {
        auto p = std::make_unique<Page>();
        p->id = id;
        p->title = title;
        p->preview = preview;
        if (preview)
            p->subtitle = "Preview - these settings have no effect yet";
        pages.push_back(std::move(p));
        return *pages.back();
    };

    {
        auto& p = page("resume", "Resume");
        p.action = [this]() { close(); };
    }
    {
        auto& p = page("media", "Media");
        p.bare = true;
        auto browser = p.add(new MediaBrowser());
        browser->font = &font;
        browser->notify = [this](const std::string& text) { notify(text); };
        browser->closeOverlay = [this]() { close(); };
        browser->canAct = [this]() { return canRunMachineAction(); };
        p.refresh = [browser](Page&) { browser->refresh(); };
    }
    {
        auto& p = page("savestates", "Savestates", true);
        auto list = p.add(new ListView({{"Slot", 72}, {"Saved", 0}}));
        list->label = "Slots";
        std::vector<std::vector<std::string>> rows;
        for (int i = 0; i < 10; i++)
            rows.push_back({"Slot " + std::to_string(i), "-"});
        list->setRows(rows);
        p.add(new Button("Save to slot", [this]() { notify("Preview: no effect yet"); }));
        p.add(new Button("Load from slot", [this]() { notify("Preview: no effect yet"); }));
    }
    {
        auto& p = page("video", "Video", true);
        p.add(new Choice("Shader", {"None", "CRT", "CRT + bezel"}, 0));
        p.add(new Toggle("Integer scaling", true));
        p.add(new Choice("Border", {"Visible", "None", "Full"}, 0));
        p.add(new Slider("Brightness", 0, 200, 5, 100, "%"));
        p.add(new Slider("Saturation", 0, 200, 5, 100, "%"));
        p.add(new Choice("Region", {"PAL", "NTSC"}, 0));
    }
    {
        auto& p = page("audio", "Audio", true);
        p.add(new Slider("Volume", 0, 100, 5, 80, "%"));
        p.add(new Choice("SID model", {"6581", "8580"}, 0));
        p.add(new Toggle("Drive sounds", true));
    }
    {
        auto& p = page("input", "Input", true);
        p.add(new Choice("Joystick port", {"Port 2", "Port 1"}, 0));
        p.add(new Choice("Keyboard mapping", {"Positional", "Symbolic"}, 0));
        p.add(new Toggle("Swap joystick ports", false));
        p.add(new TextInput("Overlay hotkey", "F12", 16));
    }
    {
        auto& p = page("system", "System", true);
        p.add(new Choice("C64 model", {"C64 PAL (6569)", "C64C PAL (8565)", "C64 NTSC (6567)", "C64C NTSC (8562)"}, 0));
        p.add(new Choice("Drive type", {"1541-II", "1541", "1571", "1581"}, 0));
        p.add(new TextInput("ROM folder", Roms::userDir(), 512));
    }
    {
        auto& p = page("reset", "Reset");
        p.subtitle = "Soft reset keeps the memory, hard reset power cycles (media stay inserted)";
        p.add(new Button("Soft reset", [this]() { resetMachine(false); }));
        p.add(new Button("Hard reset", [this]() { resetMachine(true); }));
    }
    {
        auto& p = page("quit", "Quit");
        p.action = [this]() { ask("Quit", "Quit Badline?", [this]() { quitProgram(); }); };
    }

    for (auto& p : pages)
        p->focus = firstFocusable(*p);
}

// ------------------------------------------------------------ open / close

auto Overlay::open() -> void {
    if (opened || !font.loaded())
        return;
    opened = true;
    visible = true;
    float a = lastAlpha < 0 ? 0.0f : lastAlpha;
    fadeStart = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(FadeTime * a);

    program->isPause |= PauseBit;
    if (audioDriver)
        audioDriver->clear();
    // keys held when opening must not stay pressed for the C64
    if (inputDriver) {
        SDL_Event ev{};
        ev.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        inputDriver->handleEvent(&ev);
    }

    if (currentPage().refresh)
        currentPage().refresh(currentPage());
    dirty = true;
}

auto Overlay::close() -> void {
    if (!opened)
        return;
    if (auto w = focusedWidget(); w && w->editing())
        w->nav(Nav::Back);
    dialog.reset();
    opened = false;
    float a = lastAlpha < 0 ? 1.0f : lastAlpha;
    fadeStart = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(FadeTime * (1.0f - a));
    if (!sdlWindow)
        visible = false; // nothing to fade headless

    program->isPause &= ~PauseBit;
    dirty = true;
}

// ------------------------------------------------------------ focus

auto Overlay::firstFocusable(Page& p) -> int {
    for (int i = 0; i < (int)p.widgets.size(); i++)
        if (p.widgets[i]->focusable())
            return i;
    return -1;
}

auto Overlay::focusedWidget() -> Widget* {
    auto& p = currentPage();
    if (zone != Zone::Content || p.focus < 0 || p.focus >= (int)p.widgets.size())
        return nullptr;
    return p.widgets[p.focus].get();
}

auto Overlay::moveFocus(int dir) -> bool {
    auto& p = currentPage();
    for (int i = p.focus + dir; i >= 0 && i < (int)p.widgets.size(); i += dir)
        if (p.widgets[i]->focusable()) {
            p.focus = i;
            return true;
        }
    return false;
}

auto Overlay::showPage(int index) -> void {
    index = std::clamp(index, 0, (int)pages.size() - 1);
    if (index == pageIndex)
        return;
    if (auto w = focusedWidget(); w && w->editing())
        w->nav(Nav::Back);
    pageIndex = index;
    hoverWidget = -1;
    if (currentPage().refresh)
        currentPage().refresh(currentPage());
}

auto Overlay::selectPage(const std::string& id) -> bool {
    for (int i = 0; i < (int)pages.size(); i++)
        if (pages[i]->id == id) {
            showPage(i);
            zone = Zone::Sidebar;
            dirty = true;
            return true;
        }
    return false;
}

auto Overlay::activateSidebar() -> void {
    auto& p = currentPage();
    if (p.action)
        p.action();
    else if (p.focus >= 0)
        zone = Zone::Content;
}

auto Overlay::wantsTextInput() const -> bool {
    if (!opened || dialog || zone != Zone::Content)
        return false;
    auto& p = *pages[pageIndex];
    if (p.focus < 0 || p.focus >= (int)p.widgets.size())
        return false;
    auto& w = p.widgets[p.focus];
    return w->editing() || w->acceptsText();
}

// ------------------------------------------------------------ input

auto Overlay::nav(Nav n) -> void {
    if (!opened)
        return;
    dirty = true;

    // pad B / Shift+Return mean Back / Return outside the media browser
    auto editingWidget = focusedWidget();
    if (dialog || (editingWidget && editingWidget->editing()) || zone == Zone::Sidebar) {
        if (n == Nav::Parent) n = Nav::Back;
        if (n == Nav::Insert) n = Nav::Ok;
    }

    if (dialog)
        return dialogNav(n);

    if (zone == Zone::Sidebar) {
        switch (n) {
            case Nav::Up: showPage(pageIndex - 1); break;
            case Nav::Down: showPage(pageIndex + 1); break;
            case Nav::Home: case Nav::PageUp: showPage(0); break;
            case Nav::End: case Nav::PageDown: showPage((int)pages.size() - 1); break;
            case Nav::Ok: case Nav::Right: activateSidebar(); break;
            case Nav::Back: close(); break;
            case Nav::PrevTab: showPage(pageIndex - 1); break;
            case Nav::NextTab: showPage(pageIndex + 1); break;
            default: break;
        }
        return;
    }

    auto w = focusedWidget();
    if (!w) {
        zone = Zone::Sidebar;
        return;
    }
    // an edited text field keeps all keys
    if (w->nav(n) || w->editing())
        return;
    if (n == Nav::Insert) {
        w->nav(Nav::Ok);
        return;
    }
    // shoulder buttons page through lists that have no tabs
    if (n == Nav::NextTab || n == Nav::PrevTab) {
        w->nav(n == Nav::NextTab ? Nav::PageDown : Nav::PageUp);
        return;
    }

    switch (n) {
        case Nav::Up: moveFocus(-1); break;
        case Nav::Down: moveFocus(1); break;
        case Nav::Left: case Nav::Back: case Nav::Parent: zone = Zone::Sidebar; break;
        default: break;
    }
}

auto Overlay::textInput(const std::string& s) -> void {
    if (!opened || dialog)
        return;
    if (auto w = focusedWidget())
        if (w->text(s))
            dirty = true;
}

auto Overlay::sidebarRect(int index) const -> Rect {
    return { panel.x, panel.y + 20 + index * EntryHeight, SidebarWidth, EntryHeight };
}

auto Overlay::hitWidget(int x, int y) -> int {
    auto& p = currentPage();
    for (int i = 0; i < (int)p.widgets.size(); i++)
        if (p.widgets[i]->focusable() && p.widgets[i]->rect.contains(x, y))
            return i;
    return -1;
}

auto Overlay::pointer(int x, int y, int clicks) -> void {
    if (!opened)
        return;
    layout();

    if (dialog) {
        for (int i = 0; i < (int)dialog->buttons.size(); i++)
            if (dialog->buttonRect(i).contains(x, y)) {
                if (dialog->selected != i) dirty = true;
                dialog->selected = i;
                if (clicks)
                    finishDialog(i);
                return;
            }
        if (clicks && !dialog->rect.contains(x, y))
            finishDialog(-1);
        return;
    }

    int side = -1;
    for (int i = 0; i < (int)pages.size(); i++)
        if (sidebarRect(i).contains(x, y))
            side = i;
    int widget = side < 0 ? hitWidget(x, y) : -1;

    if (side != hoverSidebar || widget != hoverWidget)
        dirty = true;
    hoverSidebar = side;
    hoverWidget = widget;

    if (!clicks)
        return;

    if (side >= 0) {
        showPage(side);
        zone = Zone::Sidebar;
        if (currentPage().action)
            currentPage().action();
        dirty = true;
        return;
    }

    if (widget >= 0) {
        auto& p = currentPage();
        if (auto w = focusedWidget(); w && w->editing() && p.focus != widget)
            w->nav(Nav::Ok);
        p.focus = widget;
        zone = Zone::Content;
        auto w = p.widgets[widget].get();
        // a click into an inactive text field starts editing, further clicks keep it
        if (!(w->editing() && w->kind() == "text"))
            w->click(x, y, clicks);
        dirty = true;
    }
}

auto Overlay::wheel(int x, int y, int dy) -> void {
    if (!opened || dialog || !dy)
        return;
    layout();
    dirty = true;
    if (panel.contains(x, y) && x < panel.x + SidebarWidth) {
        showPage(pageIndex - dy);
        return;
    }
    int widget = hitWidget(x, y);
    if (widget >= 0)
        currentPage().widgets[widget]->wheelAt(x, y, dy);
}

auto Overlay::rightClick(int x, int y) -> void {
    if (!opened)
        return;
    layout();
    if (!dialog) {
        int widget = hitWidget(x, y);
        if (widget >= 0) {
            auto& p = currentPage();
            auto w = p.widgets[widget].get();
            if (w->rightClick(x, y)) {
                p.focus = widget;
                zone = Zone::Content;
                dirty = true;
                return;
            }
        }
    }
    nav(Nav::Back);
}

auto Overlay::toLogical(float wx, float wy, float density, int& x, int& y) const -> void {
    x = ((int)(wx * density) - offsetX) / scaleFactor;
    y = ((int)(wy * density) - offsetY) / scaleFactor;
}

auto Overlay::handleEvent(const SDL_Event& ev, float density) -> bool {
    if (!font.loaded())
        return false;

    if (ev.type == SDL_EVENT_KEY_DOWN && (int)ev.key.scancode == toggleScancode) {
        if (!ev.key.repeat)
            toggle();
        return true;
    }
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && toggleButton >= 0 && ev.gbutton.button == toggleButton) {
        toggle();
        return true;
    }
    if (!opened)
        return false;

    auto logical = [&](float wx, float wy, int& x, int& y) { toLogical(wx, wy, density, x, y); };

    switch (ev.type) {
        case SDL_EVENT_KEY_DOWN: {
            Nav n;
            bool shift = ev.key.mod & SDL_KMOD_SHIFT;
            bool ctrl = ev.key.mod & SDL_KMOD_CTRL;
            if (ctrl) {
                switch (ev.key.scancode) {
                    case SDL_SCANCODE_B: nav(Nav::Bookmark); return true;
                    case SDL_SCANCODE_H: nav(Nav::AllFiles); return true;
                    case SDL_SCANCODE_E: nav(Nav::Eject); return true;
                    default: break;
                }
            }
            switch (ev.key.scancode) {
                case SDL_SCANCODE_TAB: n = shift ? Nav::PrevTab : Nav::NextTab; break;
                case SDL_SCANCODE_UP: n = Nav::Up; break;
                case SDL_SCANCODE_DOWN: n = Nav::Down; break;
                case SDL_SCANCODE_LEFT: n = Nav::Left; break;
                case SDL_SCANCODE_RIGHT: n = Nav::Right; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
                    // space types into text fields
                    if (ev.key.scancode == SDL_SCANCODE_SPACE && wantsTextInput()) return true;
                    n = shift && ev.key.scancode != SDL_SCANCODE_SPACE ? Nav::Insert : Nav::Ok; break;
                case SDL_SCANCODE_ESCAPE: n = Nav::Back; break;
                case SDL_SCANCODE_PAGEUP: n = Nav::PageUp; break;
                case SDL_SCANCODE_PAGEDOWN: n = Nav::PageDown; break;
                case SDL_SCANCODE_HOME: n = Nav::Home; break;
                case SDL_SCANCODE_END: n = Nav::End; break;
                case SDL_SCANCODE_BACKSPACE: n = Nav::Backspace; break;
                case SDL_SCANCODE_DELETE: n = Nav::Delete; break;
                default: return true;
            }
            nav(n);
            return true;
        }
        case SDL_EVENT_KEY_UP:
            return true;

        case SDL_EVENT_TEXT_INPUT:
            // Ctrl/Alt shortcuts are not text
            if (ev.text.text && !(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
                textInput(ev.text.text);
            return true;

        case SDL_EVENT_MOUSE_MOTION: {
            int x, y;
            logical(ev.motion.x, ev.motion.y, x, y);
            pointer(x, y, 0);
            return false; // the window still shows the cursor
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int x, y;
            logical(ev.button.x, ev.button.y, x, y);
            if (ev.button.button == SDL_BUTTON_LEFT)
                pointer(x, y, std::max(1, (int)ev.button.clicks));
            else if (ev.button.button == SDL_BUTTON_RIGHT)
                rightClick(x, y);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return true;

        case SDL_EVENT_MOUSE_WHEEL: {
            int x, y;
            logical(ev.wheel.mouse_x, ev.wheel.mouse_y, x, y);
            float dy = ev.wheel.y;
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
            wheel(x, y, dy > 0 ? 1 : (dy < 0 ? -1 : 0));
            return true;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (ev.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: nav(Nav::Up); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: nav(Nav::Down); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: nav(Nav::Left); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: nav(Nav::Right); break;
                case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: nav(Nav::Ok); break;
                case SDL_GAMEPAD_BUTTON_EAST: nav(Nav::Parent); break;
                case SDL_GAMEPAD_BUTTON_BACK: nav(Nav::Back); break;
                case SDL_GAMEPAD_BUTTON_WEST: nav(Nav::Insert); break;
                case SDL_GAMEPAD_BUTTON_NORTH: nav(Nav::Bookmark); break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: nav(Nav::PrevTab); break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: nav(Nav::NextTab); break;
            }
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            // triggers page through lists
            if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                int i = ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                bool down = ev.gaxis.value > 16000;
                if (down && !triggerDown[i])
                    nav(i ? Nav::PageDown : Nav::PageUp);
                if (down || ev.gaxis.value < 8000)
                    triggerDown[i] = down;
                return true;
            }
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return true;
    }
    return false;
}

// ------------------------------------------------------------ dialog

auto Overlay::ask(const std::string& title, const std::string& text, std::function<void()> onYes) -> void {
    dialog = std::make_unique<Dialog>();
    dialog->title = title;
    dialog->message = text;
    dialog->buttons = {"Yes", "No"};
    dialog->selected = 1; // the safe answer
    dialog->onResult = [onYes](int button) {
        if (button == 0 && onYes)
            onYes();
    };
    dirty = true;
}

auto Overlay::dialogNav(Nav n) -> void {
    int last = (int)dialog->buttons.size() - 1;
    switch (n) {
        case Nav::Left: case Nav::Up: dialog->selected = std::max(0, dialog->selected - 1); break;
        case Nav::Right: case Nav::Down: dialog->selected = std::min(last, dialog->selected + 1); break;
        case Nav::Ok: finishDialog(dialog->selected); break;
        case Nav::Back: finishDialog(-1); break;
        default: break;
    }
}

auto Overlay::finishDialog(int button) -> void {
    auto d = std::move(dialog);
    dirty = true;
    if (d && d->onResult)
        d->onResult(button);
}

auto Overlay::notify(const std::string& text) -> void {
    message = text;
    messageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    dirty = true;
}

// ------------------------------------------------------------ actions

auto Overlay::canRunMachineAction() -> bool {
    if (!activeEmulator) {
        notify("No emulator running");
        return false;
    }
    bool stopped = mcpServer ? mcpServer->cpuStopped() : (emuThread && emuThread->debugging);
    if (stopped) {
        notify("Not possible while the debugger has stopped the CPU");
        return false;
    }
    return true;
}

auto Overlay::resetMachine(bool hard) -> void {
    if (!canRunMachineAction())
        return;
    bool locked = emuThread->lock();
    if (hard)
        program->power(activeEmulator);
    else
        program->reset(activeEmulator);
    if (locked)
        emuThread->unlock();
    close();
}

auto Overlay::quitProgram() -> void {
    if (mcpServer) {
        // handled by Program::mcpHandleQuit after the running MCP call has answered
        mcpServer->quitRequested = true;
    } else if (sdlWindow) {
        SDL_Event ev{};
        ev.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&ev);
    } else
        GUIKIT::Application::quit();
}

// ------------------------------------------------------------ layout + drawing

auto Overlay::setSize(int width, int height) -> void {
    if (width <= 0 || height <= 0 || (width == pixelWidth && height == pixelHeight))
        return;
    pixelWidth = width;
    pixelHeight = height;
    scaleFactor = std::max(1, std::min(height / MinLogicalHeight, width / MinLogicalWidth));
    logicalWidth = width / scaleFactor;
    logicalHeight = height / scaleFactor;
    offsetX = (width - logicalWidth * scaleFactor) / 2;
    offsetY = (height - logicalHeight * scaleFactor) / 2;
    dirty = true;
}

auto Overlay::layout() -> void {
    panel = { 8, 8, std::max(0, logicalWidth - 16), std::max(0, logicalHeight - 16) };
    content = { panel.x + SidebarWidth + 12, panel.y + 10, std::max(0, panel.w - SidebarWidth - 24), std::max(0, panel.h - 10 - 24) };

    auto& p = currentPage();
    int y = p.bare ? content.y : content.y + 20 + (p.subtitle.empty() ? 4 : 16);
    // bare pages leave room for a second key hint line and a message line
    int bottom = content.y + content.h - (p.bare ? 24 : 0);
    const int gap = 2;
    int fixed = 0, expanding = 0;
    for (auto& w : p.widgets) {
        if (w->expands) expanding++;
        else fixed += w->preferredHeight() + gap;
    }
    int rest = bottom - y - fixed;
    for (auto& w : p.widgets) {
        int h = w->preferredHeight();
        if (w->expands)
            h = std::max(h, rest / std::max(1, expanding) - gap);
        w->rect = { content.x, y, content.w, h };
        y += h + gap;
    }

    if (dialog)
        dialog->layout(logicalWidth, logicalHeight, font);
}

auto Overlay::hints() -> std::vector<Hint> {
    if (dialog)
        return {{"←→", "choose"}, {"RETURN", "confirm"}, {"ESC", "cancel"}};
    std::vector<Hint> out;
    if (zone == Zone::Sidebar) {
        out.push_back({"↑↓", "section"});
        if (currentPage().action || currentPage().focus >= 0)
            out.push_back({"RETURN", "open"});
        out.push_back({"ESC", "close"});
    } else if (auto w = focusedWidget(); w && w->ownsHints()) {
        out = w->hints();
    } else {
        out.push_back({"↑↓", "move"});
        if (auto w = focusedWidget()) {
            auto h = w->hints();
            out.insert(out.end(), h.begin(), h.end());
            if (w->editing())
                return out;
        }
        out.push_back({"ESC", "back"});
    }
    out.push_back({SDL_GetScancodeName((SDL_Scancode)toggleScancode), "resume"});
    return out;
}

auto Overlay::draw(float alpha) -> std::shared_ptr<DRIVER::OverlayFrame> {
    auto frame = std::make_shared<DRIVER::OverlayFrame>();
    frame->atlas = font.atlas;
    frame->atlasWidth = font.atlasWidth;
    frame->atlasHeight = font.atlasHeight;
    frame->atlasId = atlasId;
    frame->width = pixelWidth;
    frame->height = pixelHeight;
    frame->vertices.reserve(6 * 8 * 1500);

    layout();
    if (alpha > 0.0f) {
        Painter p(font, *frame, scaleFactor, offsetX, offsetY, alpha);
        drawMenu(p);
    }
    if (dragActive) {
        Painter p(font, *frame, scaleFactor, offsetX, offsetY, 1.0f);
        drawDrop(p);
    }
    return frame;
}

auto Overlay::drawMenu(Painter& p) -> void {
    p.rawRect(0, 0, pixelWidth, pixelHeight, Theme::Dim);
    p.rect(panel.x, panel.y, panel.w, panel.h, Theme::Panel);
    p.rect(panel.x, panel.y, SidebarWidth, panel.h, Theme::Sidebar);
    p.text(panel.x + 8, panel.y + 8, "BADLINE", Theme::TextDim);

    for (int i = 0; i < (int)pages.size(); i++) {
        auto r = sidebarRect(i);
        bool sel = i == pageIndex;
        if (sel)
            p.rect(r.x, r.y, r.w, r.h, zone == Zone::Sidebar ? Theme::Selected : Color::hex(0x6c5eb5, 0.45f));
        else if (i == hoverSidebar)
            p.rect(r.x, r.y, r.w, r.h, Color::hex(0x6c5eb5, 0.3f));
        p.text(r.x + 8, r.y + 4, pages[i]->title, sel ? Theme::TextBright : Theme::Text);
    }

    auto& page = currentPage();
    if (!page.bare) {
        int tw = p.text(content.x, content.y, page.title, Theme::TextBright, OverlayFont::Pro, 2);
        if (page.preview)
            p.text(content.x + tw + 8, content.y + 8, "PREVIEW", Theme::Warn);
    }
    if (!page.bare && !page.subtitle.empty())
        p.textClip(content.x, content.y + 22, page.subtitle, content.w, Theme::Path);

    for (int i = 0; i < (int)page.widgets.size(); i++) {
        auto& w = page.widgets[i];
        bool focused = zone == Zone::Content && i == page.focus && !dialog;
        if (!focused && i == hoverWidget && w->kind() != "list" && w->kind() != "media")
            p.rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, Color::hex(0x6c5eb5, 0.25f));
        w->draw(p, focused);
    }

    if (page.action && !dialog)
        p.text(content.x, content.y + 28, "Press RETURN to " + (page.id == "resume" ? std::string("continue the emulation") : "quit"), Theme::Text);

    if (dialog) {
        p.rect(panel.x, panel.y, panel.w, panel.h, Theme::Dim);
        dialog->draw(p);
    }

    // key hints, wrapped into at most two lines
    std::vector<std::vector<std::pair<Hint, int>>> lines(1);
    {
        int x = 0;
        for (auto& h : hints()) {
            int w = font.textWidth(OverlayFont::Pro, h.key) + 6 + 4 + font.textWidth(OverlayFont::Pro, h.action);
            if (x && x + w > content.w) {
                if (lines.size() == 2)
                    break;
                lines.emplace_back();
                x = 0;
            }
            lines.back().push_back({h, w});
            x += w + 12;
        }
    }
    int y = panel.y + panel.h - 16 - 12 * ((int)lines.size() - 1);
    int top = y;
    for (auto& line : lines) {
        int x = content.x;
        for (auto& [h, w] : line) {
            int kw = font.textWidth(OverlayFont::Pro, h.key) + 6;
            p.rect(x, y - 1, kw, 10, Theme::KeyBg);
            p.text(x + 3, y, h.key, Theme::KeyText);
            p.text(x + kw + 4, y, h.action, Theme::TextDim);
            x += w + 12;
        }
        y += 12;
    }

    if (!message.empty() && std::chrono::steady_clock::now() < messageUntil)
        p.textClip(content.x, top - 14, message, content.w, Theme::Warn);
}

auto Overlay::tick() -> void {
    if (!videoDriver || !sdlWindow)
        return;
    int w, h;
    sdlWindow->drawableSize(w, h);
    setSize(w, h);

    float alpha = 0.0f;
    if (visible) {
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - fadeStart) / std::chrono::duration<float>(FadeTime);
        t = std::clamp(t, 0.0f, 1.0f);
        alpha = opened ? t : 1.0f - t;

        if (!message.empty() && std::chrono::steady_clock::now() >= messageUntil) {
            message.clear();
            dirty = true;
        }
        if (!opened && alpha <= 0.0f) {
            visible = false;
            lastAlpha = 0.0f;
            dirty = true;
        }
    }

    if (!visible && !dragActive) {
        if (frameShown)
            videoDriver->setOverlay(nullptr);
        frameShown = false;
        return;
    }
    if (!dirty && alpha == lastAlpha)
        return;

    dirty = false;
    if (visible)
        lastAlpha = alpha;
    frameShown = true;
    videoDriver->setOverlay(draw(alpha));
}

// ------------------------------------------------------------ MCP

auto Overlay::describe(Json& out) -> void {
    layout();
    out = Json::object();
    out.set("open", opened);
    out.set("scale", scaleFactor);
    Json size = Json::object();
    size.set("width", logicalWidth);
    size.set("height", logicalHeight);
    out.set("logical_size", size);
    out.set("page", currentPage().id);
    out.set("zone", zone == Zone::Sidebar ? "sidebar" : "content");

    Json sidebar = Json::array();
    for (int i = 0; i < (int)pages.size(); i++) {
        Json e = Json::object();
        e.set("id", pages[i]->id);
        e.set("title", pages[i]->title);
        auto r = sidebarRect(i);
        e.set("rect", Json(std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h)));
        sidebar.push(e);
    }
    out.set("sidebar", sidebar);
    out.set("preview", currentPage().preview);

    Json widgets = Json::array();
    auto& page = currentPage();
    for (int i = 0; i < (int)page.widgets.size(); i++) {
        auto& w = page.widgets[i];
        Json e = Json::object();
        e.set("kind", w->kind());
        e.set("label", w->label);
        e.set("value", w->value());
        if (i == page.focus) e.set("focused", zone == Zone::Content);
        if (w->editing()) e.set("editing", true);
        e.set("rect", Json(std::to_string(w->rect.x) + "," + std::to_string(w->rect.y) + "," + std::to_string(w->rect.w) + "," + std::to_string(w->rect.h)));
        Json detail;
        w->describe(detail);
        if (!detail.isNull())
            e.set("detail", detail);
        widgets.push(e);
    }
    out.set("widgets", widgets);

    if (dialog) {
        Json d = Json::object();
        d.set("title", dialog->title);
        d.set("message", dialog->message);
        Json b = Json::array();
        for (auto& s : dialog->buttons) b.push(s);
        d.set("buttons", b);
        d.set("selected", dialog->buttons[dialog->selected]);
        out.set("dialog", d);
    } else
        out.set("dialog", Json());

    if (!message.empty() && std::chrono::steady_clock::now() < messageUntil)
        out.set("message", message);
    Json drag;
    describeDrop(drag);
    out.set("drag", drag);
}
