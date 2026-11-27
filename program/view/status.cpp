
#include "status.h"
#include "../window/sdlWindow.h"
#include "../audio/manager.h"
#include "../cmd/cmd.h"
#include "../tools/chronos.h"
#include "../thread/emuThread.h"
#include "../input/manager.h"
#include "../helper/miscHelper.h"

StatusHandler* statusHandler = nullptr;

auto StatusHandler::updateDeviceState( Emulator::Interface::Media* media, bool write, unsigned position, uint8_t LED, bool motorOff ) -> void {
    setDeviceUpdate();
    
    for (auto& deviceState : deviceStates) {
        if (deviceState.media == media) {
            deviceState.write = write;
            deviceState.position = position;
            if (LED & 0x80) { // can be changed more times a frame
                deviceState.LED <<= 2;
                deviceState.LED |= LED & 3;
                deviceState.inputsPerFrame++;
                if (LED & 0x40) { // flicker
                    deviceState.LED <<= 2;
                    deviceState.inputsPerFrame++;
                }
            } else {
                deviceState.LED = LED & 3;
                deviceState.inputsPerFrame = 0;
            }

            deviceState.motorOff = motorOff;
            deviceState.update = true;
            return;
        }
    }

    uint16_t _led = LED & 3;
    uint8_t _inputsPerFrame = (LED & 0x80) ? 1 : 0;

    deviceStates.push_back({media, write, position, _led, motorOff, _inputsPerFrame, true});
}

auto StatusHandler::setMessage(const std::string& txt, bool warn, bool force, unsigned duration) -> void {
    if (!force && showOnlyUrgentMessages && !warn)
        return;

    if (txt.empty())
        duration = 0;

    message.txt = txt;
    message.duration = duration;
    message.warn = warn;
    
    setMessageUpdate();
}

auto StatusHandler::resetFrameCounter() -> void {
    if (!activeEmulator)
        return;

    auto& stats = activeEmulator->getStatsForSelectedRegion();

    float speed;
    bool percent;
    program->getSpeedBySelectedProfile(speed, percent);

    if (percent) {
        fpsCounter.fps = (stats.fps * (double)speed) / 100.0;

    } else {
        fpsCounter.fps = speed;
    }

    fpsCounter.sum = 0;
    fpsCounter.pos = 0;
    fpsCounter.measures = 0;
    fpsCounter.updateDelay = 0;
    fpsCounter.last = Chronos::getTimestampInMicroseconds();
    program->loopFrames = 0;
}

auto StatusHandler::updateOnScreenFPS() -> void {
    auto fpsScreen = globalSettings->get<unsigned>("fps_screen", 0);

    bool _showFPSScreen = showFPSScreen;
    if (fpsScreen & (1 << (sdlWindow && sdlWindow->fullScreen() ? 1 : 0)))    showFPSScreen = true;
    else                                                    showFPSScreen = false;

    if (_showFPSScreen == showFPSScreen)
        return;

    if (!showFPSScreen)
        videoDriver->showScreenText( "", 0 );
    else
        statusHandler->setFpsCounterUpdate();
}

auto StatusHandler::updateFrameCounter() -> void {
    float deviation;

    if (fpsCounter.measures == FPS_MEASUREMENTS) {
        fpsCounter.sum -= fpsCounter.deltas[fpsCounter.pos];
        deviation = 0.99;
    } else {
        fpsCounter.measures++;
        deviation = 0.8;
    }

    uint64_t cur = Chronos::getTimestampInMicroseconds();

    uint64_t delta = cur - fpsCounter.last;

    fpsCounter.deltas[fpsCounter.pos++] = delta;

    fpsCounter.sum += delta;

    fpsCounter.fps = (deviation * fpsCounter.fps) + (1.0 - deviation) * ((double)fpsCounter.measures / ((double)fpsCounter.sum / 1000000.0) );

    if (fpsCounter.pos == FPS_MEASUREMENTS) {
        fpsCounter.pos = 0;
    }

    fpsCounter.last = cur;

    unsigned limit = ((unsigned)fpsCounter.fps * fpsCounter.updateIntervall) / 1000;

    if (++fpsCounter.updateDelay >= limit) {
        if (!cmd->debug && (program->warp.mode != Program::Warp::Off || !audioDriver->hasSynchronized()))
            // check input polling and message loop every 50 ms
            program->loopFrames = ((unsigned)fpsCounter.fps * 50 ) / 1000;
        else
            // check input polling every frame
            program->loopFrames = 0;

        fpsCounter.updateDelay = 0;
        if (!cmd->noGui) {
            setFpsCounterUpdate();
        }
    }
}

auto StatusHandler::setMessageLevel() -> void {
    auto settings = Program::getSettings(activeEmulator);
    showOnlyUrgentMessages = settings->get<bool>("only_urgent_messages", false);
}

auto StatusHandler::clear() -> void {
    message.clear();
    clearUpdates();
    setMessage("", false, true);
}

auto StatusHandler::setFpsRefresh() -> void {
    if (!activeEmulator)
        return;
    auto settings = Program::getSettings( activeEmulator );
    fpsCounter.updateIntervall = settings->get<unsigned>("fps_refresh", 1000, {200u, 3000u});

    auto pointsBefore = fpsCounter.decimalPoints;
    fpsCounter.decimalPoints = settings->get<unsigned>("fps_decimal_point", 1u, {0u, 3u});

    if (pointsBefore != fpsCounter.decimalPoints) {
        videoDriver->showScreenText( "", 0 );
        setFpsCounterUpdate();
    }
}

auto StatusHandler::reset() -> void {
    resetFrameCounter();
    setFpsRefresh();
    setMessageLevel();
}

// only screen texts: messages, DRC statistics, FPS (no status bar since M5)
auto StatusHandler::update() -> void {
    auto& drcS = audioManager->statistics;

    emuThread->lockStatus();

    if (activeEmulator) {
        std::string _FPS;
        if (fpsCounterUpdate() && showFPSScreen) {
            _FPS = fpsCounter.decimalPoints
                    ? GUIKIT::String::formatFloatingPoint(fpsCounter.fps, fpsCounter.decimalPoints)
                    : std::to_string((unsigned)round(fpsCounter.fps));
        }

        if (messageUpdate()) {
            videoDriver->showScreenText( message.txt, message.duration, message.warn );

        } else if (drcS.enable) {
            if (drcBufferUpdate()) {
                std::string OSDText = "DRC: ";
                OSDText += GUIKIT::String::formatFloatingPoint(drcS.current, 2) + "% ";
                OSDText += "[ " + GUIKIT::String::formatFloatingPoint(drcS.min, 2) + " : " + GUIKIT::String::formatFloatingPoint(drcS.max, 2) + " ]";
                OSDText += " Ø " + GUIKIT::String::formatFloatingPoint(drcS.average, 2) + "%";
                videoDriver->showScreenText( OSDText, 0 );
            }
        } else if (showFPSScreen) {
            if (fpsCounterUpdate())
                videoDriver->showScreenText( _FPS, 0 );
        }
    }

    clearUpdates();
    emuThread->unlockStatus();
}
