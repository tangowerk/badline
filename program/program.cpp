
#include "program.h"
#include "input/manager.h"
#include "tools/filesetting.h"
#include "tools/filepool.h"
#include "tools/chronos.h"
#include "view/status.h"
#include "states/states.h"
#include "audio/manager.h"
#include "firmware/manager.h"
#include "video/palette.h"
#include "cmd/cmd.h"
#include "media/autoloader.h"
#include "media/fileloader.h"
#include "media/recentFiles.h"
#include "thread/emuThread.h"
#include "helper/fileHelper.h"
#include "helper/miscHelper.h"
#include "helper/settingsHelper.h"

#include "mcp/server.h"
#include "window/sdlWindow.h"
#include "overlay/overlay.h"
#include "firmware/roms.h"
#include <chrono>
#include <clocale>
#include <thread>
#include "view/message.h"

Program* program = nullptr;
DRIVER::Input* inputDriver = new DRIVER::Input;
DRIVER::Audio* audioDriver = new DRIVER::Audio;
DRIVER::Video* videoDriver = new DRIVER::Video;
std::vector<Emulator::Interface*> emulators;
Emulator::Interface* activeEmulator = nullptr;
std::vector<GUIKIT::Settings*> settingsStorage;
GUIKIT::Settings* globalSettings = nullptr;
FilePool* filePool = nullptr;
Logger* logger = nullptr;
Cmd* cmd = nullptr;
std::vector<FileSetting*> FileSetting::instances = {};
VideoManager* activeVideoManager = nullptr;
InputManager* activeInputManager = nullptr;
bool Program::focused = false;
Message* appMessage = nullptr;

#include "video.cpp"

extern "C" int badline_main(int argc, char** argv) {
    // gtk_init did this before M9: user locale (keyboard layout guess via LC_MESSAGES),
    // but "C" number formatting (settings, shaders)
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");

    cmd = new Cmd(argc, argv);
    if (cmd->hasInvalidParam()) {
        cmd->printInvalidParam();
        return -1;
    }

    if (cmd->helpRequested || cmd->versionRequested) {
        cmd->printHelp();
        return 0;
    }
    
    Program program;
    if (!GUIKIT::Application::isQuit)
        GUIKIT::Application::run();
    
    return GUIKIT::Application::exitCode;
}

Program::Program() {
    program = this;
    GUIKIT::Application::name = APP_NAME;
    GUIKIT::Application::vendor = AUTHOR;
    srand(time(nullptr) );

    globalSettings = new GUIKIT::Settings;
	autoloader = new Autoloader;
	fileloader = new Fileloader;
    emuThread = new EmuThread;
    settingsStorage.push_back( globalSettings );
    appMessage = new Message; // also headless: stderr / MCP capture
	if(!cmd->noGui) {
		// SDL window instead of the GTK main window; GTK windows are not created anymore
		sdlWindow = new SdlWindow;
		statusHandler = new StatusHandler;
		audioManager = new AudioManager;
	}
    
	logger = new Logger;
	filePool = new FilePool;

    fpsChangeTimer.setInterval(500);
    fpsChangeTimer.onFinished = [this]() {
        if (quitInProgress)
            return;
        emuThread->lock();
        if (audioManager)
            audioManager->setResampler();
        
        if (statusHandler)
            statusHandler->resetFrameCounter();

        fpsChangeTimer.setEnabled(false);
        emuThread->unlock();
    };

    addEmulators();
    init();

	if(!cmd->noGui) {
		InputManager::build();
		if (!sdlWindow->create()) {
			GUIKIT::Application::exitCode = 1;
			GUIKIT::Application::isQuit = true;
			return;
		}
		finishStartup();
    } else {
        finishStartup();
    }
}

auto Program::finishStartup() -> void {
    InputManager::initDriver();
    AudioManager::initDriver();
    if (!initVideo() && !cmd->noDriver && getVideoDriver() == "Vulkan") {
        // no silent fallback (SPEC "Decisions M-VK1")
        fprintf(stderr, "Vulkan renderer failed: %s\n", videoDriverError.c_str());
        GUIKIT::Application::exitCode = 1;
        GUIKIT::Application::isQuit = true;
        return;
    }

    cmd->autoloadImages();

    if (GUIKIT::Application::exitCode || GUIKIT::Application::isQuit)
        return;

    Socket::init();
    if (cmd->binaryMonitor) {
        binaryMonitor.setServer( cmd->binaryMonitorAddress );
        if (cmd->initbreak) {
            if (binaryMonitor.checkForClientToAccept(1000))
                binaryMonitor.initBreak();
        }
    }

    // agent-driven sessions must not block on startup dialogs (e.g. missing media)
    std::vector<std::string> startupMessages;
    if (cmd->mcp)
        Message::capture = &startupMessages;

    if (cmd->fetchRoms && !cmd->debug) {
        std::string error;
        bool ok = Roms::download([](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }, error);
        if (ok)
            std::fprintf(stderr, "ROMs are in %s\n", Roms::userDir().c_str());
        else
            std::fprintf(stderr, "ROM download failed: %s\n", error.c_str());
    }

    if (!activeEmulator)
        power(binaryMonitor.clientConnected() ? getEmulator("C64") : getLastUsedEmu(), false);

    // Commodore ROMs are not shipped: offer the download (window) or explain how to get them
    if (!cmd->debug) {
        auto missing = Roms::missing();
        if (!missing.empty()) {
            if (sdlWindow)
                sdlWindow->askRomDownload(missing);
            else {
                std::string list;
                for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
                std::fprintf(stderr, "Missing ROMs: %s. Run with -fetch-roms to download them from %s, "
                    "or copy them into %s\n", list.c_str(), Roms::sourceHost().c_str(), Roms::userDir().c_str());
            }
        }
    }

    Message::capture = nullptr;
    for (auto& m : startupMessages)
        std::fprintf(stderr, "%s\n", m.c_str());

    if (!cmd->debug) {
        overlay = new Overlay;
        if (!overlay->init()) {
            delete overlay;
            overlay = nullptr;
        }
        updateOnScreenText();
        // FPS screen text from the saved setting (was StatusHandler::init before M9)
        if (statusHandler)
            statusHandler->updateOnScreenFPS();
    }

    if (cmd->mcp && !mcpServer) {
        mcpServer = new McpServer;
        if (!mcpServer->start(cmd->mcpPort)) {
            delete mcpServer;
            mcpServer = nullptr;
        }
    }

	initUserInterface();
    
    initialized = true;
}

auto Program::initUserInterface() -> void {
    if (GUIKIT::Application::isQuit)
        return;

    if (cmd->noGui) {
		emuThread->enable( false );
        // no audio manager: never produce samples (autostart may have set a warp mode already;
        // -debugcart keeps the video output for -exitscreenshot)
        if (activeEmulator && !cmd->aggressiveWarp)
            activeEmulator->setWarpMode( (unsigned)Emulator::Interface::WarpMode::NoAudioOut );
        GUIKIT::Application::loop = [this]() { loopNoGui(); };
    } else if (cmd->debug) {
        emuThread->enable( false );
        GUIKIT::Application::loop = [this]() { loop(); };
    } else {
        videoDriver->freeContext();
		GUIKIT::Application::loop = [this]() { loopUserInterface(); };
		emuThread->enable( true );
    }
}

auto Program::addEmulators() -> void {
    auto emulatorC64 = new LIBC64::Interface;
    emulatorC64->bind = this;
    emulators.push_back( emulatorC64 );

    for( auto emulator : emulators ) {
        auto settings = new GUIKIT::Settings;

        settings->setGuid(dynamic_cast<void*> (emulator));

        settingsStorage.push_back( settings );

        firmwareManagers.push_back( new FirmwareManager( emulator, emulatorC64 == emulator ) );

        // headless: MCP savestates
        states.push_back( new States( emulator ) );

		if (cmd->noGui)
			continue;

        inputManagers.push_back( new InputManager( emulator ) );

        videoManagers.push_back( new VideoManager( emulator ) );
        
        if (dynamic_cast<LIBC64::Interface*>( emulator ))
            paletteManagers.push_back( new PaletteManager( emulator ) );
    }        
}

auto Program::init() -> void {
    
    if (!cmd->debug) {
        SettingsHelper::loadSettings();

    }
    
    cmd->parse();
    
    for( auto emulator : emulators )        
        initEmulator( emulator );
    	
	logger->setSavePath( FileHelper::generatedFolder("") );

	if (!cmd->debug)
        MiscHelper::addFileFonts();

    upgradeCropSettings();
}

auto Program::initEmulator( Emulator::Interface* emulator ) -> void {
    auto _settings = getSettings(emulator);

    MiscHelper::setJit(emulator);

    for (auto& connector : emulator->connectors)
        emulator->connect(&connector, MiscHelper::getDevice(emulator, &connector));
    
    for (auto& model : emulator->models)
        emulator->setModelValue( model.id, _settings->get<int>( _underscore(model.name), model.defaultValue, model.range) );
    
    updateCrop( emulator );

    PaletteManager* paletteManager = PaletteManager::getInstance(emulator);
    if (paletteManager)
        paletteManager->load();

    setPalette( emulator );
    
    MiscHelper::setExpansionSelection( emulator );

    MiscHelper::setRunAhead( emulator );

    MiscHelper::setRewind( emulator );

    if (dynamic_cast<LIBC64::Interface*>( emulator ))
        setMemoryPattern( emulator );
}

auto Program::getMemoryPatternFromConfig(Emulator::Interface* emulator, Emulator::Interface::MemoryPattern& pattern) -> void {
    auto settings = getSettings( emulator );

    pattern.value = settings->get<unsigned>("memory_value", 255);
    pattern.offset = settings->get<unsigned>("memory_offset", 0);
    pattern.invertEvery = settings->get<unsigned>("memory_invert_every", 64);
    pattern.secondValue = settings->get<unsigned>("memory_second_value", 0);
    pattern.secondInvertEvery = settings->get<unsigned>("memory_second_invert_every", 0);

    pattern.randomPatternLength = settings->get<unsigned>("memory_random_pattern", 1);
    pattern.repeatRandomPattern = settings->get<unsigned>("memory_random_repeat", 256);
    pattern.randomChance = settings->get<unsigned>("random_chance", 0);
}

auto Program::setMemoryPattern(Emulator::Interface* emulator) -> void {
    Emulator::Interface::MemoryPattern pattern;
    getMemoryPatternFromConfig(emulator, pattern);
    emulator->setMemoryInitParams( pattern );
}

auto Program::power( Emulator::Interface* emulator, bool regular ) -> void {
    isPause &= 4; // an open overlay keeps the emulation paused
    bool emuSwap = activeEmulator != emulator;
    powerOff();
    
    activeEmulator = emulator;
    auto settings = getSettings( emulator );
    activeVideoManager = VideoManager::getInstance( emulator );
    activeInputManager = InputManager::getManager(emulator);

    if (activeInputManager)
        activeInputManager->setupKeycodeTransfer();
	uint8_t* data = nullptr;
    std::vector<std::string> brokenPaths;

    emulator->setExpansion( settings->get<unsigned>("expansion", 0) );
    auto expansion = emulator->getExpansion();
    
    for(auto& mediaGroup : emulator->mediaGroups) {

        if (mediaGroup.isExpansion() && (&mediaGroup != expansion->mediaGroup) && (&mediaGroup != expansion->mediaGroupExpanded) )
            // allow only expansion media groups for the currently used expansion
            continue;

        bool IPMode = mediaGroup.isExpansion() && mediaGroup.expansion->isRS232();

        auto selectedMedia = mediaGroup.selected;

        for(auto& media : mediaGroup.media) {
            
            if (selectedMedia && !media.parent && (selectedMedia != &media) )
                // only one media element at a time can be used for this group
                continue;

            if (selectedMedia && media.parent && (selectedMedia != media.parent) )
                // only secondary and tertiary for this media element
                continue;
            
            auto fSetting = FileSetting::getInstance( emulator, _underscore( media.name ) );

            media.guid = (uintptr_t)(nullptr);

            if (!IPMode) {
                GUIKIT::File* file = filePool->get(GUIKIT::File::resolveRelativePath(fSetting->path));
                if (!file)
                    continue;

                if (!FileHelper::loadImageDataWhenOk(file, fSetting->id, &mediaGroup, data)) {
                    if (regular && !GUIKIT::Vector::find(brokenPaths, fSetting->path))
                        brokenPaths.push_back(fSetting->path);

                    continue;
                }
                media.guid = uintptr_t(file);

                emulator->insertMedium(&media, data, file->archiveDataSize(fSetting->id));
                emulator->writeProtect(&media, fSetting->writeProtect);

                if (!mediaGroup.isProgram()) {
                    filePool->assign(_ident(emulator, media.name + "store"), file);
                    filePool->assign(_ident(emulator, media.name), file);
                }

                if (regular)
                    FileHelper::updateSaveIdent(&media, fSetting);

            } else { // IP socket mode
                MiscHelper::prepareSocket( &media, emulator, fSetting->path );
            }

            if (!cmd->noGui)
                States::getInstance( activeEmulator )->updateImage( fSetting, &media );

            if (mediaGroup.isExpansion()) {
                for(auto& jumper : mediaGroup.expansion->jumpers) {
                    bool state = settings->get<bool>( _underscore(media.name + "_jumper_" + jumper.name), jumper.defaultState );
                    emulator->setExpansionJumper( &media, jumper.id, state );
                }
            }
        }
    }

    FirmwareManager::getInstance( activeEmulator )->insert();
	if (!cmd->noGui) {
		FileHelper::errorOpen( brokenPaths );

		filePool->unloadOrphaned();

	    audioManager->muteTimer.setEnabled();
		audioManager->power();

		if (emuSwap) {
            setVideoFilter();
		    updateHDR();
		    updateBFI();
            activeVideoManager->rebuildShader = true;
            setVideoDimension(activeEmulator);
		    updateOnScreenText();
		    initFastForward();

        }


		MiscHelper::resetRunAhead();


		globalSettings->set("last_used_emu", activeEmulator->ident);

        statusHandler->reset();

	}
	
	activeEmulator->power();
    if (emuSwap)
        setRotation();

    updateFullscreenSetting();
}

auto Program::getSpeedBySelectedProfile(float& speed, bool& percent) -> unsigned {
    auto settings = Program::getSettings( activeEmulator );

    if (warp.mode == Program::Warp::FastForward) {
        speed = warp.ff_speed;
        percent = warp.ff_percent;
        return ~0;
    }

    auto speedProfile = settings->get<unsigned>("speed_profile", 1, {0, SpeedProfiles - 1});
    getSpeed(speedProfile, speed, percent);
    return speedProfile;
}

auto Program::getSpeed(unsigned pos, float& speed, bool& percent) -> void {
    percent = false;
    speed = 50.0;

    auto& stats = activeEmulator->stats;

    switch (pos) {
        case 0: speed = stats.fps; break;
        case 1: speed = stats.isPal() ? 50.0 : 60.0; break;
        case 2: speed = 5.0; break;
        case 3: speed = 25.0; break;
        case 4: speed = stats.isPal() ? 60.0 : 50.0; break; // alternate
        case 5: speed = 70.0; break;
        case 6: speed = 75.0; break;
        case 7: speed = 80.0; break;
        case 8: speed = 90.0; break;
        case 9: speed = 100.0; break;
        case 10: speed = 120.0; break;
        case 11: speed = 250.0; break; // maximum
        case 12: {
            auto settings = Program::getSettings( activeEmulator );
            speed = settings->get<float>("custom_speed", 59.95);
            percent = settings->get<bool>("custom_speed_percent", false);
        } break;
    }
}

auto Program::isMaximumSpeed() -> bool {
    if (!activeEmulator || warp.mode == Program::Warp::FastForward)
        return false;

    auto settings = Program::getSettings( activeEmulator );
    return settings->get<unsigned>("speed_profile", 1, {0, SpeedProfiles - 1}) == (SpeedProfiles - 2);
}

auto Message::showOnScreen(const std::string& text, bool warn) -> void {
    if (!statusHandler || !program || program->quitInProgress)
        return;
    emuThread->lockStatus();
    statusHandler->setMessage(text, warn, true, 5);
    emuThread->unlockStatus();
}

auto Program::togglePause() -> void {
    if (!activeEmulator)
        return;
    isPause ^= 1;
    audioDriver->clear();
    if (!isPause && statusHandler)
        statusHandler->resetFrameCounter();
}

auto Program::reset( Emulator::Interface* emulator ) -> void {
    if (activeEmulator == emulator) {
        emulator->reset();
        MiscHelper::resetRunAhead();
    } else
        power(emulator);
}

auto Program::powerOff() -> void {
    if ( activeEmulator ) {
        setWarp( Program::Warp::Off );
        activeEmulator->powerOff();
        
        for(auto& mediaGroup : activeEmulator->mediaGroups) {
            auto selectedMedia = mediaGroup.selected;

            for(auto& media : mediaGroup.media) {
                
                filePool->assign( _ident(activeEmulator, media.name), nullptr);

                if (!selectedMedia
                    || (!media.parent && (selectedMedia == &media))
                    || (media.parent && (selectedMedia == media.parent))
                )
                    activeEmulator->ejectMedium( &media );
				
				if (!cmd->noGui)
					States::getInstance( activeEmulator )->updateImage( nullptr, &media );
            }				
        }
		activeEmulator->unsetExpansion();
        if (auto inputManager = InputManager::getManager(activeEmulator))
            inputManager->resetTouchlessAutofire();
	}
	
	if (!cmd->noGui) {
        emuThread->clearEvents();
		statusHandler->clear();
		if (activeVideoManager)
			activeVideoManager->powerOff();
        videoDriver->hintExclusiveFullscreen( false );
		audioDriver->clear();
		audioManager->powerOff();
		filePool->unloadOrphaned();
		FileHelper::updateSaveIdent( nullptr );
		InputManager::urgentUpdate = true;
		InputManager::resetJit();
	}

    activeEmulator = nullptr;
    activeVideoManager = nullptr;
    warp.autoMode = Warp::Off;
}

auto Program::loopNoGui() -> void {
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();

	while (!GUIKIT::Application::isQuit) {
        if (!mcpServer) {
            activeEmulator->run();
            continue;
        }

        mcpServer->uiTick();
        if (mcpHandleQuit())
            return;
        mcpServer->emuTick();

        if (isPause && !mcpServer->wantsFrame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            next = Clock::now();
            continue;
        }

        activeEmulator->run();
        mcpServer->frameDone();

        // real-time speed unless unthrottled (warp, -unthrottled, testbench)
        if (cmd->unthrottled || cmd->debug || warp.mode != Warp::Off) {
            next = Clock::now();
            continue;
        }
        double fps = activeEmulator->getRegionGeometry() == Emulator::Interface::Region::Ntsc ? 59.826 : 50.125;
        next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        auto now = Clock::now();
        if (next > now)
            std::this_thread::sleep_until(next);
        else if (now - next > std::chrono::milliseconds(100))
            next = now; // fell behind (e.g. long MCP call): do not catch up
    }
}

auto Program::loop() -> void {
    if (!emuThread->enabled) {
        if (sdlWindow) {
            sdlWindow->pumpEvents();
            if (GUIKIT::Application::isQuit || quitInProgress)
                return;
            focused = sdlWindow->focused();
        }
        if (mcpServer) {
            mcpServer->uiTick();
            if (mcpHandleQuit())
                return;
        }
    }

    InputManager::poll();

    // refresh rate changed, the renderer stalled/recovered (hidden window) or its target time
    // support changed (swapchain created): new pacing decision
    bool refreshChanged = sdlWindow && sdlWindow->refreshChanged.exchange(false);
    if (videoDriver->pacingChanged() || refreshChanged)
        VideoManager::setSynchronize();

    if (mcpServer)
        mcpServer->emuTick();

    if (!isPause || (mcpServer && mcpServer->wantsFrame())) {
		unsigned frames = loopFrames;
		
		if (frames) {
			while(frames--) {
			    activeEmulator->run();
                if (mcpServer) {
                    mcpServer->frameDone();
                    // frame stepping while paused ends exactly
                    if (isPause && !mcpServer->wantsFrame())
                        break;
                }
            }
		} else {
			activeEmulator->run();
            if (mcpServer)
                mcpServer->frameDone();
        }

        if (audioManager->rewind)
            audioManager->reverseAndFlushBuffer();
	} else {
        GUIKIT::System::sleep( 10 );
		audioDriver->clear();
	    repeatLastFrame();
	}

    if (statusHandler->hasUpdates())
        statusHandler->update();
}

auto Program::loopDebugging() -> void {
    InputManager::poll();
    if (mcpServer)
        mcpServer->emuTick();
    GUIKIT::System::sleep( 10 );
    audioDriver->clear();
    repeatLastFrame();

    if (statusHandler->hasUpdates())
        statusHandler->update();
}

// when emu thread is active
auto Program::loopUserInterface() -> void {
    GUIKIT::System::sleep( 1 );
    if (sdlWindow) {
        sdlWindow->pumpEvents();
        if (GUIKIT::Application::isQuit || quitInProgress)
            return;
        focused = sdlWindow->focused();
    }
    emuThread->handleUIEvents();

    if (mcpServer) {
        mcpServer->uiTick();
        if (mcpHandleQuit())
            return;
    }

    if (binaryMonitor.serverRunning()) {
        if (!binaryMonitor.clientConnected()) {
            if (binaryMonitor.checkForClientToAccept()) {
                if (cmd->initbreak)
                    binaryMonitor.initBreak();
                binaryMonitor.update();
            }
        } else
            binaryMonitor.update();
    }
}

auto Program::hasFocus() -> bool {
    return focused;
}

auto Program::quit() -> void {
    videoDriver->disableExclusiveFullscreen();
    quitInProgress = true;
    if (mcpServer) {
        // stop before locking: pending calls get cancelled
        mcpServer->stop();
    }
    emuThread->lock(true);
    powerOff();
    if (statusHandler)
        statusHandler->clearUpdates();
    delete emuThread;
    fpsChangeTimer.setEnabled(false);

    // headless sessions (pipelines, agents) never write the desktop settings
    if (sdlWindow)
        sdlWindow->saveState();

    if (!cmd->debug && !cmd->noGui) {
        for(auto rF : fileloader->recentFiles)
            rF->save();

        if (globalSettings->get<bool>("save_settings_on_exit", true))
		    SettingsHelper::saveSettings( true );
        else
            SettingsHelper::forceSavingSomeGlobalSettings();
    }
    
	for(auto inputManager : inputManagers)
		delete inputManager;
    for(auto firmwareManager : firmwareManagers)
        delete firmwareManager;
    for( auto paletteManager : paletteManagers )
        delete paletteManager;
        
    delete inputDriver;
    delete audioDriver;
    delete videoDriver;
    if (sdlWindow) {
        sdlWindow->destroy();
        delete sdlWindow;
        sdlWindow = nullptr;
    }
	delete logger;
	delete filePool;
    delete cmd;
	delete autoloader;
    delete fileloader;
    
    for(auto settings : settingsStorage)
        delete settings;

    // don't deinitialize "global settings", because of APP shutdown may trigger Window resizing which needs info from global settings

    for(auto emulator : emulators)
        delete emulator;
    emulators.clear();
    VideoManager::unloadDataStorage();

    // in case of exit request from emulation core
    GUIKIT::Application::loop = nullptr;
    Socket::clean();
}

auto Program::installFolder() -> std::string {
    return GUIKIT::System::getResourceFolder(appFolder());
}

auto Program::userFolder() -> std::string {
    return GUIKIT::System::getUserDataFolder();
}

auto Program::dataFolder() -> std::string {
    return installFolder() + DATA_FOLDER;
}

auto Program::fontFolder() -> std::string {
    return installFolder() + FONT_FOLDER;
}

auto Program::settingsFile( std::string ident ) -> std::string {

	return GUIKIT::System::getUserDataFolder(appFolder()) + ident + SETTINGS_FILE;
}

auto Program::settingsFileFromEmuFolder( std::string ident ) -> std::string {
    return installFolder() + ident + SETTINGS_FILE;
}

auto Program::presetFolder() -> std::string {
    return installFolder() + PRESET_FOLDER;
}

auto Program::soundFolder() -> std::string {
    return installFolder() + SOUND_FOLDER;
}

auto Program::log(std::string data, bool newLine) -> void {
	logger->log(data, newLine);
}

auto Program::exit(int code) -> void {
    GUIKIT::Application::exitCode = code;

    if (!cmd->screenshotPath.empty())
        cmd->saveExitScreenshot();
    
	if (cmd->noGui) {
		program->quit();
        GUIKIT::Application::quit();
			
	} else if (!emuThread || !emuThread->enabled) {
        if (sdlWindow)
            sdlWindow->saveState();
        program->quit();
        GUIKIT::Application::quit();
    }

    // this function doesn't close the APP when emu thread is enabled.
    // debug cart always disables emu thread.
    // other possible exit calls (e.g. broken autostart path) happen before emu thread is spawned
}

auto Program::updateDeviceState( Emulator::Interface::Media* media, bool write, unsigned position, uint8_t LED, bool motorOff ) -> void {
	if (!media || cmd->noGui)
		return;

	statusHandler->updateDeviceState( media, write, position, LED, motorOff );
}

auto Program::updateLedState(Emulator::Interface::LedId ledId, uint8_t state) -> void {
    // LED states (power, caps, 2 MHz) were shown only in the GTK status bar (HUD milestone)
}

auto Program::appFolder() -> std::string {
	std::string _appFolder = APP_NAME;
	return GUIKIT::String::toLowerCase( _appFolder );
}

auto Program::questionToWrite(Emulator::Interface::Media* media) -> bool {
    
    // Denise asked before writing changed disk/flash media back into the image file. Without a
    // dialog the answer is 'no', unless the user turned the question off ("question_media_write").
    auto file = (GUIKIT::File*)media->guid;
    if (cmd->debug || cmd->noGui || !file)
        return false;
    if (file->isArchived() || file->isReadOnly())
        return false; // removing the write protection of C64 archives is not supported
    return !globalSettings->get<bool>("question_media_write", true);
}

auto Program::autoStartFinish(bool soft) -> void {
    if (!activeEmulator || (warp.autoMode == Warp::Off))
        return;

    if (soft && warp.motorControlled)
        return;

    InputManager::activateHiddenHotkey( Hotkey::Id::WarpOff );
}

auto Program::hintAutoWarp(uint8_t state) -> void {

    if (!activeEmulator || (warp.autoMode == Warp::Off) || warp.manuell || !warp.motorControlled)
        return;

    bool motorOn = state & 1;
    bool inputRequested = state & 2;
    bool suggestWarp = motorOn;

    if (motorOn) {
        if (warp.inputControlled && inputRequested)
            suggestWarp = false;
    }

    if (suggestWarp)
        InputManager::activateHiddenHotkey( Hotkey::Id::Warp );
    else
        InputManager::activateHiddenHotkey( Hotkey::Id::WarpOff );
}

auto Program::initAutoWarp(Emulator::Interface::MediaGroup* mediaGroup, bool initOnly) -> void {
    if (!activeEmulator)
        return;

    auto _settings = getSettings( activeEmulator );

    warp.autoMode = (Warp::Mode)_settings->get<unsigned>("auto_warp", 0, {0, 3});
    warp.manuellEndsAutoWarp = _settings->get<bool>("manuell_ends_auto_warp", true);
    warp.motorControlled = true;
    warp.inputControlled = false;

    if (warp.autoMode != Warp::Off) {
        if (mediaGroup->isDisk()) {
            if (dynamic_cast<LIBC64::Interface*>(activeEmulator))
                warp.motorControlled = !_settings->get<bool>("auto_warp_disk_first_file", true);

            warp.inputControlled = _settings->get<bool>("auto_warp_off_input", false);
        } else if (mediaGroup->isTape()) {
            warp.motorControlled = !_settings->get<bool>("auto_warp_tape_first_file", false);
            warp.inputControlled = false;
        }

        if (!initOnly)
            setWarp(warp.autoMode);
    }
}

auto Program::initFastForward() -> void {
    auto _settings = getSettings( activeEmulator );

    warp.ff_each = _settings->get<unsigned>("fastforward_speed_each", 0, {0, 3});
    warp.ff_speed = _settings->get<float>("fastforward_speed", 500.0);
    warp.ff_percent = _settings->get<bool>("fastforward_speed_percent", false);
}

auto Program::jam( Emulator::Interface::Media* media ) -> void {

    if (cmd->noGui)
        return;

    std::string out = "CPU Jam";
    if (media)
        out = "CPU " + media->name + " Jam";

    emuThread->lockStatus();
    statusHandler->setMessage( out, true );
    emuThread->unlockStatus();
}

auto Program::trapsResult(Emulator::Interface::Media* media, bool error) -> void {
    if (activeEmulator && media && media->group->isDisk()) {
        if (error) {
            if (emuThread->enabled)
                emuThread->events |= EmuThread::EVT_AUTO_LOAD_NO_TRAPS;
            else
                fileloader->autoload(activeEmulator, media, 0, false, true );
        } else {
            auto manager = FirmwareManager::getInstance( activeEmulator );
            if (manager->getStoreLevelInConfig() != 0) {
                manager->insert(true);
            }
        }
    }
}

auto Program::getLastUsedEmu() -> Emulator::Interface* {
	
	auto ident = globalSettings->get<std::string>("last_used_emu", "");
	Emulator::Interface* defaultEmu = nullptr;

	for (auto emulator : emulators) {

		if (dynamic_cast<LIBC64::Interface*>(emulator))
            defaultEmu = emulator;
		
		if (emulator->ident == ident)
			return emulator;		
	}
	
	return defaultEmu;
}

auto Program::getEmulator( std::string ident ) -> Emulator::Interface* {

    for( auto emulator : emulators ) {

        if (emulator->ident == ident) 
            return emulator;            
    }
    
    return nullptr;
}

auto Program::setThreadPriority( Emulator::Interface::ThreadPriority priority, float minProcessingTimeInMilliSeconds, float maxProcessingTimeInMilliSeconds) -> bool {

    bool result = GUIKIT::ThreadPriority::setPriority( (GUIKIT::ThreadPriority::Mode)priority, minProcessingTimeInMilliSeconds, maxProcessingTimeInMilliSeconds );

    if (!result) {
        if (priority == Emulator::Interface::ThreadPriority::Realtime)
            result = GUIKIT::ThreadPriority::setPriority(GUIKIT::ThreadPriority::Mode::High);
    }

    return result;
}

auto Program::getC64ModelValue(LIBC64::Interface::ModelId modelId) -> int {
    if (activeEmulator && dynamic_cast<LIBC64::Interface*>(activeEmulator))
        return activeEmulator->getModelValue(modelId);
    return 0;
}

auto Program::getSettings( Emulator::Interface* emulator ) -> GUIKIT::Settings* {
    for(auto settings : settingsStorage) {
        if ( (Emulator::Interface*)(settings->getGuid()) == emulator )
            return settings;
    }
    // global setting
    return settingsStorage[0];
}

auto Program::debugger(Emulator::Interface::DebuggerSnapshot* snapshot) -> void {
    auto mcp = McpServer::StopDecision::Ignore;
    if (mcpServer && !quitInProgress && !emuThread->debuggerLock)
        mcp = mcpServer->debuggerEvent(snapshot);

    if (mcp == McpServer::StopDecision::Continue) {
        // MCP step sequence in progress: keep running silently
        snapshot->mutex.unlock();
        return;
    }

    if (binaryMonitor.clientConnected()) {
        if (quitInProgress || !emuThread->enabled || emuThread->debuggerLock) {
            snapshot->mutex.unlock();
            return;
        }
        bool stop = snapshot->callbackAction != DebuggerAction::AutoUpdate;
        if (stop)
            binaryMonitor.responseStopped( static_cast<LIBC64::DebuggerSnapshot*>(snapshot) );
        snapshot->mutex.unlock();
        if (stop)
            emuThread->lockDebugger();
        return;
    }

    snapshot->mutex.unlock();

    if (mcp == McpServer::StopDecision::Stop) {
        if (emuThread->enabled)
            emuThread->lockDebugger();
        else
            loopStopped();
    }
}

// CPU stopped by an MCP breakpoint/step without emu thread (headless, -debugcart)
auto Program::loopStopped() -> void {
    emuThread->debugging = true;
    while (emuThread->debugging && !GUIKIT::Application::isQuit && !quitInProgress) {
        if (mcpServer) {
            // Ui jobs that power/reset/load must not run inside the CPU: leave first,
            // they run after the current frame (like EmuLock(true) in GUI mode)
            if (mcpServer->stopLoopShouldExit()) {
                emuThread->debugging = false;
                break;
            }
            mcpServer->uiTick();
            mcpServer->emuTick();
        }
        if (sdlWindow)
            sdlWindow->pumpEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    emuThread->debugging = false;
}

// UI thread: after uiTick; returns true when the app is quitting
auto Program::mcpHandleQuit() -> bool {
    if (!mcpServer || !mcpServer->quitRequested)
        return false;
    // give the HTTP thread time to send the response of the quit call
    static auto since = std::chrono::steady_clock::now();
    static bool seen = false;
    if (!seen) {
        seen = true;
        since = std::chrono::steady_clock::now();
    }
    if (std::chrono::steady_clock::now() - since < std::chrono::milliseconds(200))
        return false;
    mcpServer->quitRequested = false;
    if (sdlWindow)
        sdlWindow->saveState();
    if (cmd->noGui || !emuThread->enabled)
        exit(0);
    else {
        quit();
        GUIKIT::Application::quit();
    }
    return true;
}

auto Program::readMedia(Emulator::Interface::Media* media, uint8_t* buffer, unsigned length, uint64_t offset) -> unsigned {
    return FileHelper::readMedia( media, buffer, length, offset );
}

auto Program::writeMedia(Emulator::Interface::Media* media, uint8_t* buffer, unsigned length, uint64_t offset) -> unsigned {
    return FileHelper::writeMedia( media, buffer, length, offset );
}

auto Program::readAssignedMedia(Emulator::Interface::Media* media, uint8_t*& buffer, bool preview) -> unsigned {
    return FileHelper::readAssignedMedia( media, buffer, preview );
}

auto Program::writeAssignedMedia(Emulator::Interface::Media* media, uint8_t* buffer, unsigned length) -> unsigned {
    return FileHelper::writeAssignedMedia( media, buffer, length );
}

auto Program::getFileNameFromMedia(Emulator::Interface::Media* media) -> std::string {
    return FileHelper::getFileNameFromMedia( media );
}

auto Program::unloadMedia(Emulator::Interface::Media* media) -> void {
    FileHelper::unloadMedia( media );
}

auto Program::truncateMedia(Emulator::Interface::Media* media) -> bool {
    return FileHelper::truncateMedia( media );
}

auto Program::isArchivedMedia(Emulator::Interface::Media* media) -> bool {
    return FileHelper::isArchivedMedia( media );
}

auto Program::getFileFromArchive(Emulator::Interface::Media* media, unsigned id) -> Emulator::Interface::Data {
    return FileHelper::getFileFromArchive(media, id);
}

auto Program::getFileList(Emulator::Interface::Media* media, const std::string& sub) -> std::vector<std::pair<unsigned, std::string>> {
    return FileHelper::getFileList( media, sub );
}

auto Program::libraryMissing(std::string plugin) -> void {
    MiscHelper::libraryMissing(plugin);
}

auto Program::jitPoll(int delay) -> bool {
    if (cmd->noGui)
        return false;

    return InputManager::jitPoll(delay);
}

auto Program::inputPoll( uint16_t deviceId, uint16_t inputId) -> int16_t {
    auto guid = activeEmulator->devices[deviceId].inputs[inputId].guid;
    auto mapping = (InputMapping*)guid;
    int16_t state = mapping ? mapping->state : 0;

    if (unlikely(mcpServer && mcpServer->inputActive))
        state |= mcpServer->inputState(deviceId, inputId);

    return state;
}

auto Program::audioSample(int16_t sampleLeft, int16_t sampleRight) -> void {
    audioManager->process( sampleLeft, sampleRight );
}

auto Program::audioFlush() -> void {
    if (audioManager->bufferPos)
        audioManager->flush();
}

auto Program::mixDriveSound( Emulator::Interface::Media* media, Emulator::Interface::DriveSound driveSound, bool alternate, uint8_t data ) -> void {
    if (cmd->noDriver || cmd->debug)
        return;

    audioManager->mixDriveSound( media, driveSound, alternate, data );
}