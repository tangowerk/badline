
/**
 * v 2.2
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>

#include "tools/hid.h"

namespace DRIVER {

struct Viewport {
    unsigned width = 0;
    unsigned height = 0;
    int x = 0;
    int y = 0;
    float ratio = 0.0f;
    float ratioRot = 0.0f;
};

// Overlay UI drawn on top of the picture: glyph atlas (R8) + textured quads.
// Built by the program layer (UI thread), drawn by the render thread.
struct OverlayFrame {
    std::shared_ptr<const std::vector<uint8_t>> atlas;
    unsigned atlasWidth = 0;
    unsigned atlasHeight = 0;
    unsigned atlasId = 0; // changes when the atlas content changes
    // 6 vertices per quad, each: x, y (window pixels, origin top left), u, v, r, g, b, a
    std::vector<float> vertices;
    unsigned width = 0; // window pixel size the vertices were built for
    unsigned height = 0;
    bool fullWindow = false; // menu or drop tiles: screenshots capture the whole window
};

// one present of the pacing-capable renderer (Vulkan), see bvk.h BvkPresent
struct PacingSample {
    uint64_t frame;       // emulator frame sequence (0 = redraw)
    uint64_t submitNs;    // CLOCK_MONOTONIC before present
    uint64_t displayNs;   // display time, 0 = unknown
    uint32_t flags;       // PACING_*
    uint32_t copy;        // 0 = first present of the picture
    uint64_t targetNs;    // blend: predicted display time (vblank), 0 = none
};
enum { PACING_REPEAT = 1, PACING_REDRAW = 2, PACING_TIMING = 4, PACING_WAITED = 8, PACING_LOST = 16, PACING_BLEND = 32 };
enum { PACING_FEEDBACK_NONE = 0, PACING_FEEDBACK_WAIT = 1, PACING_FEEDBACK_TIMING = 2 };
enum { PRESENT_MAILBOX = 0, PRESENT_FIFO = 1, PRESENT_IMMEDIATE = 2 };

struct PacingCaps {
    int feedback = PACING_FEEDBACK_NONE;
    int timeDomain = 0;   // Vulkan time domain of displayNs (1 = CLOCK_MONOTONIC)
    uint64_t refreshNs = 0;
    bool stalled = false; // a present wait timed out, nothing displayed since (hidden window)
    bool absoluteTime = false; // presents can carry target times (grid pacing)
    bool cadence = false; // grid presents shown at the grid cadence (VRR active)
    bool blend = false;   // grid without VRR: phase-weighted frame blending per vblank
};

// colour decoding of index pictures on the GPU (Vulkan, M-VK3b), see bvk.h BvkPicture
struct Picture {
    uint32_t palette[16] = {}; // 0xRRGGBB
    float brightness = 0.0f;   // offset on the 0..255 scale
    float contrast = 1.0f;
    float saturation = 1.0f;
    float gamma = 1.0f;
    bool palPass = false;      // C64 PAL/NTSC encoding pass
    bool ntsc = false;
    bool firstLineOdd = false; // raster parity of the first picture line
    float phaseError = 0.0f;   // degrees
    float hanover = 0.0f;      // -50..50
    float lumaRise = 1.0f;     // pixels
    float lumaFall = 1.0f;
    int chromaTaps = 0;        // low pass taps per side, 0 = off
    float cutoffU = 0.0f;      // cycles per pixel
    float cutoffV = 0.0f;
};

enum Options { OPT_DisallowShader = 4, OPT_TakeScreenshot = 8, OPT_DisallowFilter = 0x10 };
enum Rotation { ROT_0, ROT_90, ROT_180, ROT_270 };

struct Video {
    using ScreenshotCallback = std::function<void (uint8_t* _data, unsigned _width, unsigned _height)>;

    virtual auto init(uintptr_t handle) -> bool { return true; }
    virtual auto term() -> void {}
    // reason of a failed init (empty if unknown)
    virtual auto lastError() -> std::string { return ""; }
    // GPU used by the renderer (empty if unknown)
    virtual auto deviceName() -> std::string { return ""; }

    // picture as C64 colour indices, decoded on the GPU with setPicture (false: not supported)
    virtual auto lockIndexed(uint8_t*& data, unsigned& pitch, unsigned _width, unsigned _height, uint8_t options = 0) -> bool { return false; }
    virtual auto setPicture(const Picture& picture) -> void {}
    virtual auto redraw() -> void {}
    virtual auto unlockAndRedraw() -> void {}
    virtual auto clear() -> void {}
    virtual auto setLinearFilter(bool state) -> void {}
    virtual auto synchronize(bool state) -> void {}
	virtual auto hasSynchronized() -> bool { return false; }
    virtual auto forceResize() -> void {}
    virtual auto lockResize() -> void {}
    virtual auto unlockResize() -> void {}

    // nullptr hides the overlay; thread-safe
    virtual auto setOverlay(std::shared_ptr<const OverlayFrame> frame) -> void {}
    virtual auto hasOverlay() -> bool { return false; }

    // frame pacing (Vulkan, M-Pacing1): every new picture is presented `repeat` times; with
    // queueDepth d > 0 present N waits until present N-d is on screen (video master)
    virtual auto supportsPacing() -> bool { return false; }
    virtual auto setPacing(unsigned repeat, unsigned queueDepth, int presentMode, uint64_t gridNs = 0, uint64_t refreshNs = 0) -> void {}
    // emulated frame rate as presented (slang OriginalFPS)
    virtual auto setContentFps(double fps) -> void {}
    // slang presets (Vulkan, M-VK3a): `result` gets a JSON summary or the error text; an empty
    // path removes the preset. `overrides`: saved parameter values "id=value;id=value"
    virtual auto setSlangPreset(const std::string& path, const std::string& overrides, std::string& result) -> bool {
        result = "shader presets need the Vulkan renderer";
        return false;
    }
    virtual auto setSlangParam(const std::string& id, float value) -> bool { return false; }
    virtual auto slangState() -> std::string { return "null"; }
    virtual auto pacingCaps() -> PacingCaps { return {}; }
    // copies the collected presents (oldest first); `clear` drops them afterwards
    virtual auto pacingSamples(std::vector<PacingSample>& out, bool clear) -> void {}
    // true once after the renderer stalled/recovered or its target time support changed
    // (the pacing decision is re-evaluated)
    virtual auto pacingChanged() -> bool { return false; }



    virtual auto setAspectRatio(int mode, bool _integerScaling) -> void {} // mode: 0: off, 1: TV, 2: Native, 3: Native Alt
    virtual auto getAspectRatio() -> int { return 0; }
    virtual auto getViewport() -> Viewport& { static Viewport vp = {0}; return vp; }
    virtual auto setIntegerScalingDimension( unsigned _w, unsigned _h, bool _ds) -> void {}
    virtual auto getIntegerScalingDimension(unsigned& _w, unsigned& _h) -> void { }

    virtual auto setScreenshotCallback(ScreenshotCallback callback) -> void {}


    virtual auto setRotation(Rotation rotation) -> void {}
    virtual auto getRotation() -> Rotation { return ROT_0; }


    virtual ~Video() = default;
    // the Vulkan renderer (the only one since M-VK3d)
    static auto create() -> Video*;
};

struct Audio {
    virtual auto init(uintptr_t handle) -> bool { return true; }
    virtual auto term() -> void {}
    virtual auto clear() -> void {}        
    virtual auto setFrequency(unsigned value) -> void {}
    virtual auto getFrequency() -> unsigned { return 48000u; }
    virtual auto setLatency(unsigned value) -> void {}
    // true: the latency is the client buffer only (bursty producer with DRC, video master);
    // false: total latency incl. the server (PulseAudio ADJUST_LATENCY)
    virtual auto setClientBuffer(bool state) -> void {}
    virtual auto synchronize(bool state) -> void {}
    virtual auto addSamples( const uint8_t* buffer, unsigned size) -> void {}
    virtual auto getCenterBufferDeviation() -> double { return 0.0; }
    // pacing statistics: buffer underflows and bytes dropped because the buffer was full
    virtual auto xruns(unsigned& underflows, unsigned& droppedBytes) -> void { underflows = droppedBytes = 0; }
    virtual auto resetXruns() -> void {}
    virtual auto expectFloatingPoint() -> bool { return true; }
    virtual auto getMinimumLatency() -> unsigned { return 1; }
	virtual auto hasSynchronized() -> bool { return true; }

    virtual ~Audio() = default;
    static auto create(const std::string& driver) -> Audio*;
	static auto available() -> std::vector<std::string>;
	static auto preferred() -> std::string;    
};

struct Input {
    using KeyCallback = std::function<void ()>;
    enum class Button : unsigned { Left = 1u, Right = 2u, Middle = 4u, Back = 8u, Forward = 16u };

    virtual auto init(uintptr_t handle) -> bool { return true; }
    virtual auto term() -> void {}
    virtual auto mAcquire() -> void {}
    virtual auto mUnacquire() -> void {}
    virtual auto mIsAcquired() -> bool { return false; }
	virtual auto poll() -> std::vector<Hid::Device*> { return {}; }
    virtual auto setKeyboardCallback( KeyCallback* callback ) -> void {}
    // SDL3: window events pumped on the UI thread (const SDL_Event*)
    virtual auto handleEvent(const void* event) -> void {}
	
	virtual ~Input() = default;
    static auto create(const std::string& driver) -> Input*;
	static auto available() -> std::vector<std::string>;
	static auto preferred() -> std::string;
};

}
