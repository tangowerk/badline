#pragma once

#include "../../emulation/interface.h"

namespace GUIKIT {
    struct Settings;
}

// Emulator pictures to the video driver as C64 colour indices; the palette, picture settings
// and the C64 PAL/NTSC pass run in the Vulkan decode shader (M-VK3b).
struct VideoManager {
    VideoManager(Emulator::Interface* emulator);

    static unsigned takeScreenShots;

    // frame pacing decision of the Vulkan renderer (setPacing), read by the MCP `pacing` tool
    struct Pacing {
        enum State { None, VideoMaster, AudioMaster, LowestLatency, Unthrottled, Grid } state = None;
        unsigned mode = 0;      // setting `pacing`: 0 Auto, 1 Lowest latency
        unsigned repeat = 1;    // presents per emulator frame
        double refresh = 0.0;   // host refresh (Hz, SDL)
        double fps = 0.0;       // emulated frame rate incl. speed profile
        double clock = 1.0;     // emulation speed factor under video master (refresh / (repeat * fps))
        unsigned queueDepth = 0; // presents in flight before the emulation waits (0 = no wait)
        uint64_t gridNs = 0;    // Grid: target time period (ns)
        bool stalled = false;   // renderer stalled (hidden window): audio master
    };
    static Pacing pacing;

    static auto setSynchronize() -> void;
    static auto setPacing() -> void;
    static auto pacingStateName() -> std::string;

    bool suppressShaderByHotkey = false;

    Emulator::Interface* emulator;
    GUIKIT::Settings* settings;
    Emulator::Interface::Palette* palette;

    // picture settings (C64 settings), see reloadSettings
    struct Picture {
        unsigned saturation = 100;  // video_saturation 0..200 %
        unsigned contrast = 100;    // video_contrast 0..200 %
        unsigned brightness = 100;  // video_brightness 0..200 (100 = neutral)
        unsigned gamma = 100;       // video_gamma 30..280 (100 = 1.0)
        bool palPass = true;        // video_pal_pass: C64 PAL/NTSC encoding pass
        float phaseErrorPal = 22.5f;  // video_pal_phase_error degrees
        float phaseErrorNtsc = 0.0f;  // video_ntsc_phase_error degrees
        int hanover = 0;            // video_pal_hanover -50..50
        float lumaRise = 2.0f;      // video_luma_rise pixels 1..4
        float lumaFall = 1.2f;      // video_luma_fall pixels 1..4
        unsigned chromaTaps = 0;    // video_chroma_taps 0 (off), 3..21 odd
    } picture;

    // plain palette as 0xAARRGGBB (raw images)
    uint32_t colorTable[16] = {};
    // luma of the plain palette (audio luma interference)
    float lumaTable[16] = {};

    auto setPalette(Emulator::Interface::Palette* palette) -> void;
    auto reloadSettings() -> void;
    // C64 settings <- `picture`, then applied
    auto savePicture() -> void;
    // Vulkan decode settings from palette, picture settings, region and crop
    auto updatePicture() -> void;

    auto renderFrame(const uint8_t* src, unsigned width, unsigned height, unsigned srcPitch) -> void;
    auto takeScreenshot(unsigned unscaled, const uint8_t* src, unsigned width, unsigned height, unsigned pitch) -> void;
    auto sampleLuma(unsigned width, unsigned height, const uint8_t* src, unsigned srcPitch) -> void;
    auto powerOff() -> void {}

    static auto getInstance( Emulator::Interface* emulator ) -> VideoManager*;

    auto toggleShaderTemporary() -> bool;

private:
    int cropTopParity = -1;
};

extern std::vector<VideoManager*> videoManagers;
