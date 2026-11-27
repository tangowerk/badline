
#include "../program.h"
#include "../tools/chronos.h"
#include "emuThread.h"

#include "../input/manager.h"
#include "../media/autoloader.h"
#include "../media/fileloader.h"
#include "../view/status.h"
#include "../monitor/binaryMonitor.h"

EmuThread* emuThread = nullptr;

EmuThread::EmuThread() {
    kill = false;
    attention = false;
    acknowledged = false;
    updateBorder = false;
    debugging = false;
    enabled = false;
    events = 0;
}

EmuThread::~EmuThread() {
    enable(false);
}

auto EmuThread::enable(bool state) -> void {

    if (state == enabled)
        return;  

    if (state) {
        while (kill) {
            std::this_thread::yield();
        }
        initWorker();
    } else {
        kill = true;
        unlock();
        while (kill) {
            std::this_thread::yield();
        }
    }
	
	enabled = state;
}

auto EmuThread::lockDebugger() -> void {
    debugging = true;
    while (debugging) {

        if (freeContext) {
            freeContext = false;
            videoDriver->freeContext();
        }

        if (attention && debugging) {
            attention = false;
            acknowledged = true;

            while(acknowledged) {
                std::this_thread::yield();
            }

            if (!debugging) {
                break;
            }
        }

        program->loopDebugging();
    }
}

auto EmuThread::unlockDebugger() -> void {
    if (debugging) {
        debugging = false;
        acknowledged = false;
        if (program->binaryMonitor.clientConnected())
            program->binaryMonitor.sendResume();
    }
}

auto EmuThread::lock(bool unlockDebugging) -> bool {
    if  (!enabled)
        return false;

    if (unlockDebugging) {
        debuggerLock = true;
        unlockDebugger();
    }

    if (acknowledged /* check for nesting */ )
        return false;

    freeContext = true;
    attention = true;
    while(attention) {
        std::this_thread::yield();
    }

    return true;
}

auto EmuThread::unlock() -> void {
    debuggerLock = false;
    acknowledged = false;
}

auto EmuThread::initWorker() -> void {

    std::thread worker([this] {
        kill = false;
        attention = false;
        acknowledged = false;
        freeContext = false;

        while (true) {

            if (freeContext) {
                freeContext = false;
                videoDriver->freeContext();
            }

            if (attention) {
                attention = false;
                acknowledged = true;

                while(acknowledged) {
                    std::this_thread::yield();
                }
            }

            if (kill) {
                videoDriver->freeContext();
                // linux hack to prevent slowdown when killing thread
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                kill = false;
                return;
            }

            if (updateBorder && activeEmulator) {
                videoMutex.lock();
                program->updateCrop(activeEmulator);
                updateBorder = false;
                videoMutex.unlock();
            }

            program->loop();

        }
    });

    worker.detach();
}

auto EmuThread::handleUIEvents() -> void {

    if (events) {
        unsigned _events = events;
        events = 0;

        if (_events & EVT_POLL_HOTKEYS)
            InputManager::pollHotkeys();

        if (_events & EVT_SHADER_ERROR) {
            auto manager = VideoManager::getInstance(activeEmulator);
            if (manager)
                manager->finishPreset();
        }

        if(_events & EVT_UPDATE_FPS)
            program->fpsChangeTimer.setEnabled();

        if (_events & EVT_AUTO_LOAD_NO_TRAPS) {
            lock(true);
            fileloader->autoload(activeEmulator, autoloader->getLatestDrive(activeEmulator), 0, false, true);
            unlock();
        }

    }
}

auto EmuThread::clearEvents() -> void {
    updateBorder = false;
    events = 0;
}

auto EmuThread::lockHotkeys() -> void {
    if (enabled)
        hotkeyMutex.lock();
}

auto EmuThread::unlockHotkeys() -> void {
    if (enabled)
        hotkeyMutex.unlock();
}

auto EmuThread::lockStatus() -> void {
    if (enabled)
        statusMutex.lock();
}

auto EmuThread::unlockStatus() -> void {
    if (enabled)
        statusMutex.unlock();
}

auto EmuThread::lockVideo() -> void {
    if (enabled)
        videoMutex.lock();
}

auto EmuThread::unlockVideo() -> void {
    if (enabled)
        videoMutex.unlock();
}

