#include <SDL3/SDL.h>
#include <cstdio>

#include "sdlWindow.h"
#include "../program.h"
#include "../cmd/cmd.h"
#include "../thread/emuThread.h"
#include "../input/manager.h"
#include "../media/autoloader.h"
#include "../view/status.h"
#include "../firmware/roms.h"
#include "../firmware/manager.h"
#include "../overlay/overlay.h"
#include <thread>

SdlWindow* sdlWindow = nullptr;

// first start: 3x the visible C64 picture (PAL, default border)
static const int DefaultWidth = 384 * 3;
static const int DefaultHeight = 272 * 3;

auto SdlWindow::create() -> bool {
    SDL_SetHint(SDL_HINT_APP_ID, "badline");
    // emulator hotkeys must reach us, also Alt+Tab-like combos are left to the compositor
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    windowedWidth = globalSettings->get<int>("window_width", DefaultWidth, {160, 16384});
    windowedHeight = globalSettings->get<int>("window_height", DefaultHeight, {120, 16384});
    bool startFullscreen = globalSettings->get<bool>("window_fullscreen", false) || cmd->startInFullscreen;

    if (!createWindow(program->getVideoDriver() == "Vulkan"))
        return false;

    if (startFullscreen)
        setFullScreen(true);

    showWindow();
    return true;
}

auto SdlWindow::createWindow(bool vulkan) -> bool {
    // the window is bound to its API at creation (EGL/GLX window or plain Vulkan surface)
    window = SDL_CreateWindow(APP_NAME " " VERSION, windowedWidth, windowedHeight,
        (vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        std::fprintf(stderr, "%s%s\n", vulkan ? "Vulkan renderer failed: " : "", error.c_str());
        return false;
    }
    error.clear();
    return true;
}

auto SdlWindow::showWindow() -> void {
    SDL_ShowWindow(window);
    // let the compositor map the window before the GL context / Vulkan swapchain is created
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    lastMouseMove = SDL_GetTicks();
    updateRefreshRate();
}

auto SdlWindow::recreate(bool vulkan) -> bool {
    bool wasFullscreen = fullscreen;
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    // per-window state is applied again by pumpEvents
    captured = false;
    cursorHidden = false;
    textInput = false;

    if (!createWindow(vulkan))
        return false;
    if (wasFullscreen)
        setFullScreen(true);
    showWindow();
    return true;
}

auto SdlWindow::updateRefreshRate() -> void {
    float rate = 0.0f;
    if (window) {
        if (auto mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
            rate = mode->refresh_rate;
    }
    currentRefreshRate = rate;
}

auto SdlWindow::destroy() -> void {
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
}

auto SdlWindow::setTitle(const std::string& title) -> void {
    if (window)
        SDL_SetWindowTitle(window, title.c_str());
}

auto SdlWindow::mousePosition() -> GUIKIT::Position {
    return GUIKIT::Position(mouseX, mouseY);
}

auto SdlWindow::drawableSize(int& w, int& h) -> void {
    w = h = 0;
    if (window)
        SDL_GetWindowSizeInPixels(window, &w, &h);
}

auto SdlWindow::setFullScreen(bool state) -> void {
    if (!window)
        return;
    fullscreen = state;
    // borderless desktop fullscreen (Wayland has no exclusive mode)
    SDL_SetWindowFullscreenMode(window, nullptr);
    SDL_SetWindowFullscreen(window, state);

    if (state)
        inputDriver->mAcquire();
    else
        inputDriver->mUnacquire();

    if (statusHandler)
        statusHandler->updateOnScreenFPS();
}

auto SdlWindow::saveState() -> void {
    globalSettings->set<bool>("window_fullscreen", fullscreen);
    if (windowedWidth > 0 && windowedHeight > 0) {
        globalSettings->set<int>("window_width", windowedWidth);
        globalSettings->set<int>("window_height", windowedHeight);
    }
}

auto SdlWindow::requestQuit() -> void {
    if (program->quitInProgress)
        return;
    saveState();
    program->quit();
    GUIKIT::Application::quit();
}

auto SdlWindow::pumpEvents() -> void {
    if (!window)
        return;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        handleEvent(ev);
        if (!window || program->quitInProgress)
            return;
    }

    if (romDownloadDone.exchange(false))
        romDownloadFinished();

    updateMouseCapture();

    // text fields of the overlay need SDL text input (layout aware characters)
    bool wantText = overlay && overlay->wantsTextInput();
    if (wantText != textInput) {
        textInput = wantText;
        if (wantText) SDL_StartTextInput(window);
        else SDL_StopTextInput(window);
    }

    if (overlay)
        overlay->tick();

    // hide an idle cursor over the picture (not over the overlay)
    bool idle = (SDL_GetTicks() - lastMouseMove) > 2000 && !(overlay && overlay->isOpen());
    if (idle && !cursorHidden && !captured) {
        SDL_HideCursor();
        cursorHidden = true;
    }
}

auto SdlWindow::updateMouseCapture() -> void {
    // the overlay needs a free pointer
    bool want = inputDriver->mIsAcquired() && hasFocus && !(overlay && overlay->isOpen());
    if (want == captured)
        return;
    captured = want;
    SDL_SetWindowRelativeMouseMode(window, want);
    if (!want && cursorHidden) {
        SDL_ShowCursor();
        cursorHidden = false;
    }
}

auto SdlWindow::handleEvent(SDL_Event& ev) -> void {
    if (romPrompt != RomPrompt::None && handleRomPrompt(ev))
        return;

    switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            requestQuit();
            return;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            hasFocus = true;
            Program::focused = true;
            program->isPause &= ~2;
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            hasFocus = false;
            Program::focused = false;
            if (inputDriver->mIsAcquired() && !fullscreen)
                inputDriver->mUnacquire();
            {
                static auto pauseFocusLoss = globalSettings->getOrInit("pause_focus_loss", false);
                program->isPause &= ~2;
                program->isPause |= (!!*pauseFocusLoss) << 1;
            }
        } break;

        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            updateRefreshRate();
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            if (!fullscreen && !(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
                windowedWidth = ev.window.data1;
                windowedHeight = ev.window.data2;
            }
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            // the renderer picks up the new drawable size on its next frame
            break;

        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            fullscreen = true;
            break;

        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            fullscreen = false;
            break;

        case SDL_EVENT_MOUSE_MOTION: {
            float density = SDL_GetWindowPixelDensity(window);
            if (density <= 0.0f) density = 1.0f;
            mouseX = (int)(ev.motion.x * density);
            mouseY = (int)(ev.motion.y * density);
            lastMouseMove = SDL_GetTicks();
            if (cursorHidden && !captured) {
                SDL_ShowCursor();
                cursorHidden = false;
            }
        } break;

        case SDL_EVENT_DROP_BEGIN:
        case SDL_EVENT_DROP_POSITION:
        case SDL_EVENT_DROP_FILE:
        case SDL_EVENT_DROP_COMPLETE:
            handleDrop(ev);
            break;
    }

    // the overlay takes keys, mouse and pad buttons while open (and its toggle key always)
    if (overlay) {
        float density = SDL_GetWindowPixelDensity(window);
        if (overlay->handleEvent(ev, density > 0.0f ? density : 1.0f))
            return;
        if (overlay->isOpen() && (ev.type == SDL_EVENT_MOUSE_MOTION || ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP))
            return;
    }

    // keyboard, mouse and joystick state for the emulation
    inputDriver->handleEvent(&ev);
}

auto SdlWindow::handleDrop(SDL_Event& ev) -> void {
    // the tiles are drawn and evaluated by the overlay (overlay/drop.cpp);
    // without overlay (fonts missing) a drop is a plain autoload
    if (!overlay) {
        if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data)
            dropFiles.push_back(ev.drop.data);
        else if (ev.type == SDL_EVENT_DROP_COMPLETE && !dropFiles.empty()) {
            emuThread->lock();
            autoloader->init(dropFiles, Autoloader::Mode::DragnDrop);
            autoloader->loadFiles();
            emuThread->unlock();
            dropFiles.clear();
        }
        return;
    }
    float density = SDL_GetWindowPixelDensity(window);
    if (density <= 0.0f)
        density = 1.0f;

    switch (ev.type) {
        case SDL_EVENT_DROP_BEGIN:
            dropFiles.clear();
            dragging = true;
            overlay->dragBegin();
            break;

        case SDL_EVENT_DROP_POSITION:
            if (dragging) {
                int x, y;
                overlay->toLogical(ev.drop.x, ev.drop.y, density, x, y);
                overlay->dragMove(x, y);
            }
            break;

        case SDL_EVENT_DROP_FILE:
            if (ev.drop.data)
                dropFiles.push_back(ev.drop.data);
            break;

        case SDL_EVENT_DROP_COMPLETE: {
            dragging = false;
            bool dropped = !dropFiles.empty();
            overlay->dragEnd(dropFiles);
            dropFiles.clear();
            if (dropped)
                SDL_RaiseWindow(window);
        } break;
    }
}

auto SdlWindow::showText(const std::string& text, bool warn, unsigned seconds) -> void {
    if (!statusHandler)
        return;
    emuThread->lockStatus();
    statusHandler->setMessage(text, warn, true, seconds);
    emuThread->unlockStatus();
}

auto SdlWindow::askRomDownload(const std::vector<std::string>& missing) -> void {
    std::string list;
    for (auto& m : missing)
        list += (list.empty() ? "" : ", ") + m;
    std::fprintf(stderr, "Missing ROMs: %s\n", list.c_str());

    romPrompt = RomPrompt::Asking;
    showText("C64 ROMs missing. Return: download from " + Roms::sourceHost() + "   Esc: skip", true, 3600);
}

// true = event consumed
auto SdlWindow::handleRomPrompt(SDL_Event& ev) -> bool {
    if (ev.type != SDL_EVENT_KEY_DOWN && ev.type != SDL_EVENT_KEY_UP)
        return false;
    if (romPrompt == RomPrompt::Downloading)
        return true;
    if (ev.type == SDL_EVENT_KEY_UP || ev.key.repeat)
        return true;

    if (ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_KP_ENTER) {
        romPrompt = RomPrompt::Downloading;
        showText("Downloading ROMs from " + Roms::sourceHost() + " ...", false, 3600);
        std::thread([this]() {
            std::string error;
            romDownloadOk = Roms::download([](const std::string& line) {
                std::fprintf(stderr, "%s\n", line.c_str());
            }, error);
            romDownloadError = error;
            romDownloadDone = true;
        }).detach();
        return true;
    }

    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
        romPrompt = RomPrompt::None;
        std::string hint = "ROMs skipped. Copy them into " + Roms::userDir() + " or start with -fetch-roms";
        std::fprintf(stderr, "%s\n", hint.c_str());
        showText(hint, true, 8);
        return true;
    }
    // other keys are swallowed while asking
    return true;
}

// UI thread
auto SdlWindow::romDownloadFinished() -> void {
    romPrompt = RomPrompt::None;

    if (!romDownloadOk) {
        std::fprintf(stderr, "ROM download failed: %s\n", romDownloadError.c_str());
        showText("ROM download failed: " + romDownloadError, true, 10);
    }

    // power on with whatever is available now
    if (activeEmulator) {
        emuThread->lock(true);
        if (auto manager = FirmwareManager::getInstance(activeEmulator))
            manager->clear();
        program->power(activeEmulator);
        emuThread->unlock();
    }

    if (romDownloadOk) {
        std::fprintf(stderr, "ROMs installed in %s\n", Roms::userDir().c_str());
        showText("ROMs installed in " + Roms::userDir(), false, 5);
    }
}
