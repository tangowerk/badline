
#pragma once

#include "resampler/data.h"
#include "resampler/cosine.h"
#include "../program.h"
#include "../../emulation/interface.h"
#include "mixer/drive.h"

#define MAX_AUDIO_BUF_SIZE (2048 * 21)

struct AudioManager {
    
    AudioManager();
    ~AudioManager();
    
    unsigned bufferPos = 0;
    unsigned bufferSize = 0;
    float buffer[MAX_AUDIO_BUF_SIZE];
    float volumeAdjust;
    
    int16_t outBuffer[65536];
    float outBufferFloat[65536];
    Resampler::Cosine cosine;
    Resampler::Data rData;
    Mixer::Drive drive;
    GUIKIT::Timer muteTimer;

    bool mixDriveSounds = false;

    struct {
        bool enable = false;
        unsigned lastTS;
    } measureUiUpdate;

    bool rewind = false;
    double ratio;    
    bool dynamicRateControl;
    double rateDelta = 0.005;

    // Vulkan video master (M-Pacing1): audio does not block, DRC keeps the buffer centred;
    // videoClock = emulation speed factor set by the display (refresh / (repeat * fps))
    bool videoMaster = false;
    unsigned appliedLatency = 0;
    bool appliedClientBuffer = false;
    // buffer setup for video master (client buffer, >= 2 frames); kept while unthrottled
    bool videoBuffer = false;
    double videoClock = 1.0;
    double resamplerInput = 0.0;
    double drcFiltered = 0.0;
    auto setVideoMaster(bool state, double clock, bool updateBuffer) -> void;

    // DRC/buffer statistics for the MCP `pacing` tool (reset there)
    struct {
        double devMin = 0, devMax = 0, devSum = 0;
        double adjMin = 0, adjMax = 0;
        unsigned count = 0;
    } drcStats;
    auto resetDrcStats() -> void;
    
    struct {
        float sum = 0;
        unsigned count = 0;
        float average = 0;
        bool enable = false;
        float current;
        float min;
        float max;
        float minRaw;
        float maxRaw;

    } statistics;

    struct LumaInterference {
        bool enabled = false;
        bool update = false;

        float avgFrame;
        float avgLines[313] = {};
        float vol = 0.0f;
        unsigned lines;

        unsigned linePos;
        unsigned lineDiff;

        unsigned noisePos;
        unsigned noiseDiff;

    } lumaInterference;
        
    double inputFPS;

    Emulator::Interface::Stats stat;

    template<unsigned bits>
    static auto sclamp(const signed x) -> signed {
        const signed b = 1U << (bits - 1);
        const signed m = b - 1;
        return (x > m) ? m : (x < -b) ? -b : x;
    }     

    auto process( int16_t sampleLeft, int16_t sampleRight ) -> void;
    auto flush() -> void;
    
    auto setLatency() -> void;
    auto setFrequency() -> void;     
    auto setSynchronize() -> void;
    auto setVolume() -> void;
    auto setInterference() -> void;
    auto updateInterference() -> void;
    auto setRateControl() -> void;
    auto resetDriveSounds() -> void;
    auto setDriveSounds(bool init = true) -> void;
    auto setTapeNoise( ) -> void;

    auto setBufferSize() -> void;
    auto setResampler() -> void;
    auto calcStatistics( float adjust ) -> void;
    auto power() -> void;
    auto powerOff() -> void;
    auto applyInterference() -> void;

    auto checkIfUINeedsAnUpdate() -> void;
    auto reverseAndFlushBuffer() -> void;
    auto setRewind(bool state) -> void;

    auto mixDriveSound( Emulator::Interface::Media* media, Emulator::Interface::DriveSound driveSound, bool alternate, uint8_t data ) -> void;

    static auto initDriver() -> void;
    static auto getSelectedDriver() -> std::string;
};

extern AudioManager* audioManager;