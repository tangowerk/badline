
#pragma once

#include "../program.h"

#define FPS_MEASUREMENTS 300

struct DeviceState {    
    Emulator::Interface::Media* media = nullptr;
    bool write;
    unsigned position;
    uint16_t LED;
    bool motorOff;
    uint8_t inputsPerFrame;
    bool update = true;
};

struct FpsCounter {
    uint64_t deltas[FPS_MEASUREMENTS];
    uint64_t sum;
    uint32_t pos;
    uint64_t last;
    uint32_t measures;
    float fps;
    unsigned updateDelay;
    unsigned updateIntervall = 1000;
    unsigned decimalPoints = 3;
};

struct StatusHandler {
    
    auto messageUpdate() const -> bool { return control & 1; }
    auto deviceUpdate() const -> bool { return control & 2; }
    auto drcBufferUpdate() const -> bool { return control & 8; }
    auto fpsCounterUpdate() const -> bool { return control & 0x10; }
    
    auto setMessageUpdate() -> void { control |= 1; }
    auto setDeviceUpdate() -> void { control |= 2; }
    auto setDrcBufferUpdate() -> void  { control |= 8; }
    auto setFpsCounterUpdate() -> void  { control |= 0x10; }

    auto clearUpdates( uint16_t mask = ~0 ) -> void { control &= ~mask; }
    auto hasUpdates() -> bool { return !!control; }

    auto reset() -> void;
    auto clear() -> void;
    auto update() -> void;
    auto updateDeviceState(Emulator::Interface::Media* media, bool write, unsigned position, uint8_t LED, bool motorOff) -> void;
    auto setMessage(const std::string& txt, bool warn = false, bool force = false, unsigned duration = 3) -> void;
    auto updateFrameCounter() -> void;
    auto resetFrameCounter() -> void;
    auto setFpsRefresh() -> void;
    auto updateOnScreenFPS() -> void;
    auto setMessageLevel() -> void;

    uint16_t control = 0;
    std::vector<DeviceState> deviceStates; // drive/tape/cartridge state for a later HUD
    FpsCounter fpsCounter;

    bool showFPSScreen = false;
    bool showOnlyUrgentMessages = false;

    struct {
        std::string txt = "";
        unsigned duration = 0;
        bool warn = false;
        
        auto clear() -> void {
            txt = "";
            duration = 0;
            warn = false;
        }
    } message;
};

extern StatusHandler* statusHandler;
