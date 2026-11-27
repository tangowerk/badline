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

    window = SDL_CreateWindow(APP_NAME " " VERSION, windowedWidth, windowedHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }

    if (startFullscreen)
        setFullScreen(true);

    SDL_ShowWindow(window);
    // let the compositor map the window before the GL context is created
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    lastMouseMove = SDL_GetTicks();
    return true;
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

    // hide an idle cursor over the picture
    bool idle = (SDL_GetTicks() - lastMouseMove) > 2000;
    if (idle && !cursorHidden && !captured) {
        SDL_HideCursor();
        cursorHidden = true;
    }
}

auto SdlWindow::updateMouseCapture() -> void {
    bool want = inputDriver->mIsAcquired() && hasFocus;
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
            if (!program->hasActiveDebugger())
                program->isPause &= ~2;
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            hasFocus = false;
            Program::focused = false;
            if (inputDriver->mIsAcquired() && !fullscreen)
                inputDriver->mUnacquire();
            if (!program->hasActiveDebugger()) {
                static auto pauseFocusLoss = globalSettings->getOrInit("pause_focus_loss", false);
                program->isPause &= ~2;
                program->isPause |= (!!*pauseFocusLoss) << 1;
            }
        } break;

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

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev.button.button == SDL_BUTTON_LEFT && videoDriver->visibleSplashScreen()) {
                emuThread->lock(true);
                if (splashHit(ev.button.x, ev.button.y) != -1)
                    videoDriver->hideSplashScreen();
                emuThread->unlock();
            }
            break;

        case SDL_EVENT_DROP_BEGIN:
        case SDL_EVENT_DROP_POSITION:
        case SDL_EVENT_DROP_FILE:
        case SDL_EVENT_DROP_COMPLETE:
            handleDrop(ev);
            break;
    }

    // keyboard, mouse and joystick state for the emulation
    inputDriver->handleEvent(&ev);
}

auto SdlWindow::handleDrop(SDL_Event& ev) -> void {
    float density = SDL_GetWindowPixelDensity(window);
    if (density <= 0.0f)
        density = 1.0f;

    switch (ev.type) {
        case SDL_EVENT_DROP_BEGIN:
            dropFiles.clear();
            dropZone = 0;
            dragging = true;
            emuThread->lock();
            // the dropped files are unknown until release: offer all four drive slots
            videoDriver->setDragnDropOverlaySlots(4);
            videoDriver->enableDragnDropOverlay(true);
            emuThread->unlock();
            break;

        case SDL_EVENT_DROP_POSITION:
            if (dragging)
                dropZone = videoDriver->sendDragnDropOverlayCoordinates((int)(ev.drop.x * density), (int)(ev.drop.y * density));
            break;

        case SDL_EVENT_DROP_FILE:
            if (ev.drop.data)
                dropFiles.push_back(ev.drop.data);
            break;

        case SDL_EVENT_DROP_COMPLETE: {
            dragging = false;
            emuThread->lock();
            videoDriver->enableDragnDropOverlay(false);

            if (!dropFiles.empty()) {
                Autoloader::Mode mode = Autoloader::Mode::DragnDrop;
                unsigned selection = 0;
                if (dropZone & 0x8000) {
                    selection = dropZone & 0xff;
                    mode = (dropZone & 0x100) ? Autoloader::Mode::AutoStartWithSlot : Autoloader::Mode::OpenWithSlot;
                }
                autoloader->init(dropFiles, mode, selection);
                autoloader->loadFiles();
            }
            dropZone = 0;
            dropFiles.clear();
            emuThread->unlock();
            SDL_RaiseWindow(window);
        } break;
    }
}

// left click on the splash screen: -1 outside the picture
auto SdlWindow::splashHit(float x, float y) -> int {
    float density = SDL_GetWindowPixelDensity(window);
    if (density <= 0.0f)
        density = 1.0f;
    int px = (int)(x * density), py = (int)(y * density);
    auto& vp = videoDriver->getViewport();
    if (px < vp.x || py < vp.y || px > vp.x + (int)vp.width || py > vp.y + (int)vp.height)
        return -1;
    return 1;
}

auto SdlWindow::loadDragnDropOverlay() -> void {
    for (int line = 0; line < 2; line++) {
        GUIKIT::File file(program->imgFolder() + "mediaSlot" + std::to_string(line) + ".png");
        GUIKIT::Image& dndOverlay = dndOverlays[line];

        if (!file.open())
            return;

        uint8_t* data = file.read();
        if (!data || !dndOverlay.loadPng(data, file.getSize()))
            return;
    }

    DRIVER::Video::DnDOverlayCallback cb = [this](DRIVER::Viewport& vp, unsigned line) -> uint8_t* {
        GUIKIT::Image& dndOverlay = dndOverlays[line];
        vp.height = (dndOverlay.height * vp.width) / dndOverlay.width;
        return dndOverlay.resize(vp.width, vp.height);
    };
    videoDriver->setDragnDropOverlayCallback(cb);
}

auto SdlWindow::loadSplashScreen() -> void {
    if (!globalSettings->get<bool>("splash_screen", true))
        return;

    GUIKIT::File file(program->imgFolder() + "startscreen.png");
    if (!file.open())
        return;

    uint8_t* data = file.read();
    if (!data || !splash.loadPng(data, file.getSize()))
        return;

    DRIVER::Video::SplashscreenCallback cb = [this](DRIVER::Viewport& vp, bool hide) -> uint8_t* {
        if (hide) {
            splash.free();
            return nullptr;
        }
        auto& winVp = videoDriver->getViewport();
        float targetRatio = (float)splash.width / (float)splash.height;

        vp.width = winVp.width;
        vp.height = static_cast<int>(winVp.width / targetRatio);

        if (vp.height > winVp.height) {
            vp.height = winVp.height;
            vp.width = static_cast<int>(winVp.height * targetRatio);
        }

        vp.x = (winVp.width - vp.width) / 2;
        vp.y = (winVp.height - vp.height) / 2;

        return splash.resize(vp.width, vp.height);
    };

    videoDriver->showSplashScreen(110, cb);
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
