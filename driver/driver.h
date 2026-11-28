
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
#include "tools/shaderpass.h"
#define MAX_SHADERS 64
#define MAX_TEXTURES 64
#define MAX_FRAME_HISTORY 128

namespace DRIVER {

struct Viewport {
    unsigned width = 0;
    unsigned height = 0;
    int x = 0;
    int y = 0;
    float ratio = 0.0f;
    float ratioRot = 0.0f;
};

struct DiskFile {
    std::string ident = "";
    std::string path = "";
    bool isLUT = false;
    uint8_t* data = nullptr;
    unsigned size = 0;
    unsigned width = 0;
    unsigned height = 0;
};

struct AppData {
    float ledDriveState = 0.0;
    float cropTop = 0.0;
    float cropLeft = 0.0;
    float lace = 0.0;
    float hires = 0.0;
    float pal = 0.0;
    float subRegion = 0.0;
    float flags = 0.0; // free to use
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

enum Options { OPT_HoldFrame = 1, OPT_Interlace = 2, OPT_DisallowShader = 4, OPT_TakeScreenshot = 8,
                OPT_DisallowFilter = 0x10, OPT_Pause = 0x20, OPT_Rewind = 0x40, OPT_RGB10 = 0x80 };
enum Rotation { ROT_0, ROT_90, ROT_180, ROT_270 };

struct Video {
    using ScreenshotCallback = std::function<void (uint8_t* _data, unsigned _width, unsigned _height)>;

    virtual auto init(uintptr_t handle) -> bool { return true; }
    virtual auto term() -> void {}
    // reason of a failed init (empty if unknown)
    virtual auto lastError() -> std::string { return ""; }
    // GPU used by the renderer (empty if unknown)
    virtual auto deviceName() -> std::string { return ""; }

    virtual auto lock(unsigned*& data, unsigned& pitch, unsigned _width, unsigned _height, uint8_t options = 0) -> bool { return false; }
    virtual auto redraw() -> void {}
    virtual auto unlockAndRedraw() -> void {}
    virtual auto clear() -> void {}
    virtual auto setLinearFilter(bool state) -> void {}
    virtual auto setShader(ShaderPreset* preset) -> void {}
    virtual auto setShaderProgressCallback( std::function<void (int pass, bool hasErrors)> onCallback ) -> void {}
    virtual auto setShaderCacheCallback( std::function<void (DiskFile& diskFile)> onCallback ) -> void {}
    virtual auto useShaderCache(bool state) -> void {}
    virtual auto getShaderNativeVertexCode(std::string& slang, std::string& out) -> bool { return false; }
    virtual auto getShaderNativeFragmentCode(std::string& slang, std::string& out) -> bool { return false; }
    virtual auto synchronize(bool state) -> void {}
	virtual auto hasSynchronized() -> bool { return false; }
    virtual auto hardSync(bool state) -> void {}
    virtual auto setThreaded(bool state) -> void {}
    virtual auto hasThreaded() -> bool { return false; }
    virtual auto waitRenderThread() -> void {}
    // shader compilation in progress (any thread): drives the overlay busy spinner
    virtual auto setBusyCallback(std::function<void (bool busy)> onCallback) -> void {}
    virtual auto forceResize() -> void {}
    virtual auto freeContext() -> void {}
    virtual auto lockResize() -> void {}
    virtual auto unlockResize() -> void {}

    virtual auto hintResizing(bool state) -> void {}
    virtual auto needResizingPreparations(bool useEmuThread) -> bool { return false; }
    virtual auto prepareResizing() -> void {}
    virtual auto endResizing() -> void {}


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

    virtual auto setVRR(bool state, float speed = 0.0) -> void {}
    virtual auto hasVRR() -> bool { return false; }
    virtual auto changeThreadPriorityToRealtime(bool state) -> void {}

    virtual auto getAppData() -> AppData* { return nullptr; }

    virtual auto setAspectRatio(int mode, bool _integerScaling) -> void {} // mode: 0: off, 1: TV, 2: Native, 3: Native Alt
    virtual auto getAspectRatio() -> int { return 0; }
    virtual auto getViewport() -> Viewport& { static Viewport vp = {0}; return vp; }
    virtual auto setIntegerScalingDimension( unsigned _w, unsigned _h, bool _ds) -> void {}
    virtual auto getIntegerScalingDimension(unsigned& _w, unsigned& _h) -> void { }

    virtual auto shaderSupport() -> bool { return false; }
    virtual auto setScreenshotCallback(ScreenshotCallback callback) -> void {}
	/** direct 3D only */
	virtual auto hasExclusiveFullscreen() -> bool { return false; }
	virtual auto hintExclusiveFullscreen(bool state, float rate = 0.0) -> void {}
    virtual auto disableExclusiveFullscreen() -> void {}

    virtual auto activateApp(bool state) -> void {}

    virtual auto canHardSync() -> bool { return false; }
    virtual auto canExclusiveFullscreen() -> bool { return false; }
    virtual auto setRotation(Rotation rotation) -> void {}
    virtual auto getRotation() -> Rotation { return ROT_0; }

    virtual auto HDRsupport() -> bool { return false; }
    virtual auto setHDR(bool state, float maxNits, float paperWhiteNits, float contrast, bool expandGamut) -> void {}
    virtual auto setBFI(unsigned frames, unsigned darkFrames) -> void {}

    virtual ~Video() = default;
    static auto create(const std::string& driver) -> Video*;
	static auto available() -> std::vector<std::string>;
	static auto preferred() -> std::string;
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
    // the window was recreated (renderer switch); devices and mappings stay
    virtual auto setWindow(uintptr_t handle) -> void {}
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
