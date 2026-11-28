
#pragma once
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>

struct EmuThread {
    EmuThread();
    ~EmuThread();

    bool enabled = false;
    std::atomic<bool> attention;
    std::atomic<bool> acknowledged;
    std::atomic<bool> debugging;
    std::atomic<bool> updateBorder;
    std::atomic<bool> debuggerLock{false};

    enum {  EVT_AUTO_LOAD_NO_TRAPS = 4, EVT_DISMISS_PLACEHOLDER = 8,
            EVT_UPDATE_FPS = 0x10,
            EVT_POLL_HOTKEYS = 0x80 };

    std::atomic<unsigned> events;

    std::atomic<bool> ready;
    std::atomic<bool> kill;

    std::mutex statusMutex;
    std::mutex hotkeyMutex;
    std::mutex videoMutex;

    auto lock(bool unlockDebugging = false) -> bool;
    auto unlock() -> void;
    auto locked() -> bool { return attention || acknowledged; }

    auto lockDebugger() -> void;
    auto unlockDebugger() -> void;

    auto lockHotkeys() -> void;
    auto lockStatus() -> void;
    auto unlockStatus() -> void;
    auto unlockHotkeys() -> void;
    auto lockVideo() -> void;
    auto unlockVideo() -> void;

    auto enable(bool state) -> void;


    auto initWorker() -> void;
    auto handleUIEvents() -> void;
    auto clearEvents() -> void;
};

extern EmuThread* emuThread;