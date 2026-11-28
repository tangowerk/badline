
#include "cmd/cmd.h"
#include "video/slang.h"
#include <cstring>

auto Program::initVideo() -> bool {

	if (videoDriver) {
        SlangPresets::wait(); // a preset load uses the driver
        delete videoDriver;
    }
    
    if (cmd->noDriver) {
        videoDriver = new DRIVER::Video;
        return true;
    }

    videoDriver = DRIVER::Video::create();

    VideoManager::setSynchronize();
    setVideoFilter();
    setVideoDimension();

    bool ok = videoDriver->init( sdlWindow ? sdlWindow->handle() : 0 );
    videoDriverError = ok ? "" : videoDriver->lastError();
    if (!ok) {
        if (videoDriverError.empty())
            videoDriverError = "Vulkan initialization failed";
        delete videoDriver;
        videoDriver = new DRIVER::Video;
    }

    for (auto emulator : emulators)
        VideoManager::getInstance(emulator)->reloadSettings();

    videoDriver->setScreenshotCallback([this](uint8_t* _data, unsigned _width, unsigned _height) {
        takeScreenshot(_data, _width, _height);
    } );

    // the saved slang preset (or -shader), compiled on a worker thread
    if (ok)
        SlangPresets::applyWanted();

    updateOnScreenText();
    return ok;
}

auto Program::bufferScreenshot(uint8_t* _data, unsigned _size) -> void {
    auto& screenShot = screenshot;
    unsigned useSize = _size * 3;

    if (!screenShot.bufferSize)
        screenShot.bufferSize = useSize;
    else if (screenShot.bufferSize != useSize) {
        screenshot.clearBuffer();
        screenShot.bufferSize = useSize;
    }

    uint8_t* ptr = new uint8_t[useSize];
    screenShot.buffer.push_back(ptr);
    std::memcpy(ptr, _data, useSize);
}

auto Program::takeScreenshot(uint8_t* _data, unsigned _width, unsigned _height) -> void {
    auto& screenShot = screenshot;
    screenShot.sharedMutex.lock();
    GUIKIT::Image::Encoded encoded;
    encoded.data = nullptr;
    GUIKIT::Image image;
    std::vector<uint32_t> colorTable;

    if (screenShot.saveState) {
        GUIKIT::Image image(_width, _height, _data, GUIKIT::Image::Format::RGB);
        image.scaleLinear(200, 150);
        encoded = image.generate(GUIKIT::Image::Type::PNG);

    } else if (screenShot.twoFrames && !screenShot.buffer.size()) {
        bufferScreenshot(_data, _width * _height);

    } else {
        if (screenShot.writePalette) {
            for (auto& pal : activeVideoManager->palette->paletteColors)
                colorTable.push_back(pal.rgb);
            
            encoded = image.generate(screenShot.type, colorTable, {_data}, _width, _height);

        } else {
            if (screenShot.buffer.size()) {
                auto screenBefore = screenShot.buffer[0];

                if (screenShot.bufferSize == (_width * _height * 3)) {
                    uint8_t* src = screenBefore;
                    uint8_t* desc = _data;

                    for (unsigned y = 0; y < _height; y++) {
                        for (unsigned x = 0; x < (_width * 3); x++)
                            *desc++ = ((unsigned)*desc + (unsigned)*src++) >> 1;
                    }
                }
                screenshot.clearBuffer();
            }

            encoded = image.generate(screenShot.type, _data, _width, _height);
        }
    }

    if (encoded.data) {
        GUIKIT::File file;
        std::string _replace = std::to_string(Chronos::getTimestampInSecondsReal());
        if (screenShot.gun)
            _replace += "_" + std::to_string(screenShot.gun++);

        std::string _path = screenShot.path;

        GUIKIT::String::replace(_path, "#ident#", _replace);

        file.setFile(_path);
        file.open(GUIKIT::File::Mode::Write);
        file.write(encoded.data, encoded.size);
        file.unload();
        delete[] encoded.data;
    }

    screenShot.sharedMutex.unlock();
}

auto Program::setVideoDimension(Emulator::Interface* emulator) -> void {
    if (!activeEmulator || (emulator && (emulator != activeEmulator)))
        return;

    auto settings = Program::getSettings( activeEmulator );

    int aspectMode = settings->get<int>("aspect_mode", 1, {0, 3});
    bool integerScaling = settings->get<bool>("integer_scaling", false);

    videoDriver->setAspectRatio( aspectMode, integerScaling );
}

auto Program::videoRefresh8(const uint8_t* frame, unsigned width, unsigned height, unsigned linePitch) -> void {
	if (cmd->noGui)
		return;
	
    statusHandler->updateFrameCounter();
	
    if (frame)
        activeVideoManager->renderFrame(frame, width, height, linePitch);
}

auto Program::repeatLastFrame() -> void {
    if (cmd->noGui)
        return;

    auto cropData = activeEmulator->cropData();

    if (cropData) {
        activeVideoManager->renderFrame(cropData, activeEmulator->cropWidth(), activeEmulator->cropHeight(),
            activeEmulator->cropPitch());
    } else {
        // power() drops the last frame; while paused (e.g. reset/eject from the overlay) no new one
        // is emulated, so present a black picture, otherwise the overlay is never redrawn
        unsigned width = activeEmulator->cropWidth(), height = activeEmulator->cropHeight();
        if (!width || !height) {
            width = 384;
            height = 272;
        }
        static std::vector<uint8_t> black;
        if (black.size() < width * height)
            black.assign(width * height, 0);
        activeVideoManager->renderFrame(black.data(), width, height, 0);
    }

    if (VideoManager::takeScreenShots && isPause) {
        screenshot.pause = isPause;
        isPause = 0;
    }
}

auto Program::setVideoFilter() -> void {
	if (activeEmulator)
		videoDriver->setLinearFilter( Program::getSettings( activeEmulator )->get<bool>("video_filter", true) );
}

auto Program::setPalette( Emulator::Interface* emulator ) -> void {
    
    auto paletteManager = PaletteManager::getInstance( emulator );
    
    if (!paletteManager)
        return;
    
    auto videoManager = VideoManager::getInstance( emulator );
    
    videoManager->setPalette( paletteManager->getCurrentPalette() );
}

auto Program::getCropHotkeyDefault() -> unsigned {
    return 1 | 2 | 4 | 8 | 0x80 | 0x100 | 0x200;
}

auto Program::getScaleHotkeyDefault() -> unsigned {
    return 1 | 2 | 4 | 8;
}

auto Program::getCropDefault(Emulator::Interface* emulator, int pos, int direction) -> unsigned {
    return 0;
}

auto Program::upgradeCropSettings() -> void {
    for(auto emulator : emulators) {
        auto settings = Program::getSettings( emulator );

        if (!settings->get<bool>("upd_crop", false)) {
            settings->set<bool>("upd_crop", true);

            auto cropType = settings->get<int>("crop_type", (unsigned) Emulator::Interface::CropType::Monitor, {0u, 11u});
            auto valCropAC = settings->get<bool>("crop_aspect_correct", false);
            settings->remove("crop_aspect_correct");

            switch(cropType) {
                case 2: cropType = valCropAC ? 2 : 3; break;
                case 3: cropType = valCropAC ? 4 : 5; break;
                case 4: cropType = 6; break;
                default:
                    continue;
            }

            settings->set<unsigned>("crop_type", cropType);
        }
    }
}

auto Program::getCrop(Emulator::Interface* emulator, Emulator::Interface::Crop& crop) -> bool {
    typedef Emulator::Interface::CropType CropType;
    auto settings = Program::getSettings( emulator );
    int type = settings->get<int>("crop_type", (unsigned)Emulator::Interface::CropType::Monitor, {0u, 11u});

    if ((CropType)type == CropType::AllSidesRatio || (CropType)type == CropType::AllSides) {
        crop.right = crop.top = crop.bottom = crop.left = settings->get<unsigned>("crop_all", 0, {0u, 100u});
    } else if (type >= (int)Emulator::Interface::CropType::Free) {
        int offset = type - (int)Emulator::Interface::CropType::Free;
        std::string _offset = offset ? std::to_string(offset) : "";

        crop.left = settings->get<unsigned>("crop_left" + _offset, getCropDefault(emulator, offset, 0), {0u, 100u});
        crop.right = settings->get<unsigned>("crop_right" + _offset, getCropDefault(emulator, offset, 1), {0u, 100u});
        crop.top = settings->get<unsigned>("crop_top" + _offset, getCropDefault(emulator, offset, 2), {0u, 100u});
        crop.bottom = settings->get<unsigned>("crop_bottom" + _offset, getCropDefault(emulator, offset, 3), {0u, 100u});
    } else
        return false;

    return true;
}

auto Program::setCrop(Emulator::Interface* emulator, std::string ident, int value) -> void {
    typedef Emulator::Interface::CropType CropType;
    bool isDimension = ident == "crop_left" || ident == "crop_right" || ident == "crop_bottom" || ident == "crop_top";
    auto settings = Program::getSettings( emulator );

    if (isDimension) {
        int type = settings->get<int>("crop_type", (unsigned)Emulator::Interface::CropType::Monitor, {0u, 11u});
        if ((CropType) type == CropType::AllSidesRatio || (CropType) type == CropType::AllSides) {
            ident = "crop_all";

        } else if (type >= (int) Emulator::Interface::CropType::Free) {
            int offset = type - (int) Emulator::Interface::CropType::Free;
            std::string _offset = offset ? std::to_string(offset) : "";

            ident += _offset;
        }
    }

    settings->set<unsigned>( ident, value );
}

auto Program::getCropMessage( Emulator::Interface* emulator, Emulator::Interface::CropType cropType) -> std::string {
    typedef Emulator::Interface::CropType CropType;
    std::string out;

    switch(cropType) {
        case CropType::Off: out = "disabled"; break;
        case CropType::Monitor: out = "Monitor"; break;
        case CropType::AutoRatio: out = "Crop complete and maintain display ratio"; break;
        case CropType::Auto: out = "Crop complete"; break;
        case CropType::AllSidesRatio: out = "Crop all sides equally and maintain display ratio"; break;
        case CropType::AllSides: out = "Crop all sides equally"; break;
        default:
        case CropType::Free: out = "Crop for each side manually"; break;
    }

    return out + " (" + std::to_string(int(cropType)) + ")";
}

auto Program::getScaleMessage(Emulator::Interface* emulator, int aspectMode ) -> std::string {
    std::string out;

    switch(aspectMode) {
        case 0: out = "Window"; break;
        default:
        case 1: out = "CRT TV"; break;
        case 2: out = "Native"; break;
        case 3: out = "Native (free)"; break;
    }

    return out + " (" + std::to_string(aspectMode) + ")";
}

auto Program::updateCrop( Emulator::Interface* emulator ) -> void {
    auto settings = Program::getSettings( emulator );
    int type = settings->get<int>("crop_type", (unsigned)Emulator::Interface::CropType::Monitor, {0u, 6u});
    Emulator::Interface::Crop crop = {0};
    getCrop(emulator, crop);

	emulator->cropFrame( (Emulator::Interface::CropType) type, crop );
}

auto Program::toggleWarp( Warp::Mode mode) -> void {
    if (warp.manuellEndsAutoWarp && warp.motorControlled)
        warp.autoMode = Warp::Off;

    if (mode == warp.mode)
        mode = Warp::Mode::Off;

    setWarp(mode, true);
}

auto Program::setWarp( Warp::Mode mode, bool manuell ) -> void {
    unsigned forward = 0;
    bool resetFrameCounter = !(warp.mode != Warp::Off && mode != Warp::Mode::Off);

    switch (mode) {
        case Warp::Mode::Off:
            warp.manuell = false;
            loopFrames = 0;
            if (warp.mode == Warp::Mode::FastForward) {
                warp.mode = Warp::Off;
                audioManager->setSynchronize();
                audioManager->setResampler();
                audioManager->setVolume();
            } else {
                warp.mode = Warp::Off;
                VideoManager::setSynchronize();
            }

            if (audioManager)
                audioManager->drive.reset();

            break;
        case Warp::Mode::Normal:
        case Warp::Mode::Aggressive: {
            warp.manuell = manuell;

            if (warp.mode == Warp::Mode::FastForward) {
                warp.mode = mode;
                audioManager->setSynchronize();
                audioManager->setResampler();
                audioManager->setVolume();
            } else {
                warp.mode = mode;
                VideoManager::setSynchronize();
            }

            forward = (unsigned)Emulator::Interface::WarpMode::NoAudioOut | (unsigned)Emulator::Interface::WarpMode::ReduceVideoOutputEach16th;
            if (mode == Warp::Mode::Aggressive)
                forward |= (unsigned)Emulator::Interface::WarpMode::NoVideoSequencer;
        } break;

        case Warp::FastForward: {
            warp.mode = mode;
            warp.manuell = manuell;
            audioManager->setSynchronize();
            audioManager->setResampler();
            audioManager->volumeAdjust = 0.0;
            if (warp.ff_each == 1) forward = (unsigned)Emulator::Interface::WarpMode::ReduceVideoOutputEach4th;
            else if (warp.ff_each == 2) forward = (unsigned)Emulator::Interface::WarpMode::ReduceVideoOutputEach8th;
            else if (warp.ff_each == 3) forward = (unsigned)Emulator::Interface::WarpMode::ReduceVideoOutputEach16th;
        } break;
    }

    if (statusHandler && resetFrameCounter)
        statusHandler->resetFrameCounter();

    // headless: there is no audio manager to receive samples
    if (cmd->noGui)
        forward |= (unsigned)Emulator::Interface::WarpMode::NoAudioOut;

    activeEmulator->setWarpMode( forward );

}

auto Program::fpsChanged() -> void {
    if (quitInProgress)
        return;

    if (emuThread->enabled)
        emuThread->events |= EmuThread::EVT_UPDATE_FPS;
    else
        fpsChangeTimer.setEnabled();
}

auto Program::setRotation() -> void {
    if (!activeEmulator)
        return;
    auto _settings = Program::getSettings(activeEmulator);

    DRIVER::Rotation rotation = (DRIVER::Rotation)_settings->get<unsigned>("rotation", (unsigned)DRIVER::ROT_0, {0u, 3u});
    videoDriver->setRotation(rotation);
}

// screen text is drawn by the overlay (C64 Pro, M-VK2); only the corner is configurable
auto Program::updateOnScreenText() -> void {
    if (!activeEmulator || !overlay)
        return;
    overlay->setScreenTextPosition(Program::getSettings(activeEmulator)->get<unsigned>("screen_text_position", 0, {0u, 5u}));
}

auto Program::requestScreenshot() -> void {
    if (!activeEmulator)
        return;
    auto settings = Program::getSettings(activeEmulator);
    auto _path = FileHelper::generatedFolder(activeEmulator, "screen_record_path", "recordings/screenshots", FileHelper::FLAG_CREATE);
    auto _file = settings->get<std::string>("save_ident", "screenshot");
    auto screenshotFormat = settings->get<std::string>("screen_record_format", "png");
    bool withEffects = globalSettings->get<bool>("screenshot_with_effects", true);
    bool mergeTwoFrames = globalSettings->get<bool>("screenshot_merge_frames", false);
    unsigned unscaled = globalSettings->get<unsigned>("screenshot_unscaled", 0);
    bool usePalete = settings->get<bool>("screen_palette", true);
    unsigned screenGun = settings->get<unsigned>("screen_gun", 1, {1, 120});
    unsigned screenGunEach = settings->get<unsigned>("screen_gun_each", 1, { 1, 60 });
    bool delayScreenshot = settings->get<unsigned>("screen_shot_delay", false);

    _path += _file + "_#ident#";

    screenshot.sharedMutex.lock();
    if (screenshotFormat == "bmp") {
        screenshot.path = _path + ".bmp";
        screenshot.type = GUIKIT::Image::Type::BMP;
    } else if (screenshotFormat == "jpg") {
        screenshot.path = _path + ".jpg";
        screenshot.type = GUIKIT::Image::Type::JPG;
    } else if (screenshotFormat == "tga") {
        screenshot.path = _path + ".tga";
        screenshot.type = GUIKIT::Image::Type::TGA;
    } else {
        screenshot.path = _path + ".png";
        screenshot.type = GUIKIT::Image::Type::PNG;
    }

    screenshot.writePalette = (unscaled > 0) && dynamic_cast<LIBC64::Interface*>(activeEmulator) &&
        (screenshot.type == GUIKIT::Image::Type::BMP) && usePalete && !mergeTwoFrames;

    screenshot.withEffects = withEffects;
    screenshot.unscaled = unscaled;
    screenshot.twoFrames = mergeTwoFrames;
    screenshot.pause = 0;
    screenshot.saveState = false;
    screenshot.interval = screenGunEach;
    screenshot.intervalPos = delayScreenshot ? (50 * 3) : 1;

    screenshot.gun = screenGun > 1 ? 1 : 0;
    VideoManager::takeScreenShots = mergeTwoFrames ? 2 : 1;
    if (screenGun)
        VideoManager::takeScreenShots *= screenGun;

    if (mergeTwoFrames)
        screenshot.interval = 1;
    screenshot.clearBuffer();
    screenshot.sharedMutex.unlock();
}
