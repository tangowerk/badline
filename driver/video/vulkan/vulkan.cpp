// Vulkan output into an SDL3 window (M-VK1). The renderer itself is Rust (crates/vk, C API in
// bvk.h); this adapter keeps the picture buffer, geometry and overlay frame (incl. screen text).
// The window is created by the program layer with SDL_WINDOW_VULKAN; init() receives it.

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include "bvk.h"
#include "../viewport.h"
#include "../../tools/tools.h"

namespace DRIVER {

static_assert((int)BVK_REC_REPEAT == (int)PACING_REPEAT && (int)BVK_REC_REDRAW == (int)PACING_REDRAW && (int)BVK_REC_TIMING == (int)PACING_TIMING
    && (int)BVK_REC_WAITED == (int)PACING_WAITED && (int)BVK_REC_LOST == (int)PACING_LOST
    && (int)BVK_REC_BLEND == (int)PACING_BLEND, "BvkPresent flags");
static_assert((int)BVK_FEEDBACK_TIMING == (int)PACING_FEEDBACK_TIMING && (int)BVK_FEEDBACK_WAIT == (int)PACING_FEEDBACK_WAIT, "feedback");
static_assert((int)BVK_PRESENT_FIFO == (int)PRESENT_FIFO && (int)BVK_PRESENT_MAILBOX == (int)PRESENT_MAILBOX
    && (int)BVK_PRESENT_IMMEDIATE == (int)PRESENT_IMMEDIATE, "present modes");

struct SDLVK : public Video {

    SDL_Window* window = nullptr;
    BvkRenderer* vk = nullptr;
    std::string error;
    bool reportedError = false;

    // emu thread (lock/unlockAndRedraw) and UI thread (redraw, settings) both render
    std::recursive_mutex mutex;

    ViewScreen viewScreen;
    Viewport viewport;
    Rotation rotation = ROT_0;
    bool linearFilter = false;
    int presentMode = BVK_PRESENT_FIFO;
    unsigned repeat = 1;
    unsigned queueDepth = 0;
    uint64_t gridNs = 0;     // target time grid period (0 = off)
    uint64_t refreshNs = 0;  // display refresh period from SDL (0 = unknown)
    uint64_t frameSeq = 0;
    float contentFps = 50.0f;
    // presents for the MCP `pacing` tool (~2 min at 100 Hz x 2 copies)
    std::deque<PacingSample> samples;
    static constexpr size_t MaxSamples = 24000;
    bool stalled = false;
    bool absoluteTime = false;
    // stall/recovery or target time support changed: the program re-evaluates the pacing
    std::atomic<bool> capsChanged{false};
    float mvp[16];

    std::vector<uint8_t> indexBuffer; // C64 colour indices (lockIndexed)
    BvkPicture picture = {};
    bool hasPicture = false;
    unsigned frameWidth = 0;
    unsigned frameHeight = 0;
    bool hasFrame = false;
    uint8_t options = 0;

    std::mutex overlayMutex;
    std::shared_ptr<const OverlayFrame> overlayFrame;
    unsigned atlasId = 0;
    bool atlasUploaded = false;


    ScreenshotCallback screenshotCallback;

    SDLVK() {
        updateMvp();
    }

    ~SDLVK() {
        term();
    }

    auto init(uintptr_t handle) -> bool {
        term();
        window = (SDL_Window*)handle;
        if (!window) {
            error = "no window";
            return false;
        }

        BvkCreateInfo info = {};
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        const char* videoDriver = SDL_GetCurrentVideoDriver();
        if (videoDriver && std::string(videoDriver) == "wayland") {
            info.kind = BVK_SURFACE_WAYLAND;
            info.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            info.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        } else if (videoDriver && std::string(videoDriver) == "x11") {
            info.kind = BVK_SURFACE_XLIB;
            info.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            info.window = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        } else {
            error = std::string("unsupported SDL video driver ") + (videoDriver ? videoDriver : "(none)");
            return false;
        }
        if (!info.display || (!info.surface && !info.window)) {
            error = "SDL window has no native handles";
            return false;
        }

        char err[512] = {0};
        vk = bvk_create(&info, err, sizeof err);
        if (!vk) {
            error = err;
            return false;
        }
        bvk_set_present_mode(vk, presentMode);
        bvk_set_pacing(vk, repeat, queueDepth, gridNs, refreshNs);
        if (hasPicture)
            bvk_set_picture(vk, &picture);
        samples.clear();
        reportedError = false;
        return true;
    }

    auto term() -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (vk)
            bvk_destroy(vk);
        vk = nullptr;
        atlasUploaded = false;
    }

    auto lastError() -> std::string { return error; }

    auto deviceName() -> std::string {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        return vk ? bvk_device_name(vk) : "";
    }

    // VSync off (warp): MAILBOX first, the emulation is not held at display rate
    auto synchronize(bool state) -> void {
        setPacing(repeat, queueDepth, state ? BVK_PRESENT_FIFO : BVK_PRESENT_MAILBOX, gridNs, refreshNs);
    }

    auto hasSynchronized() -> bool { return presentMode == BVK_PRESENT_FIFO; }

    // ------------------------------------------------------------ pacing

    auto supportsPacing() -> bool { return true; }

    auto setPacing(unsigned _repeat, unsigned _queueDepth, int _presentMode, uint64_t _gridNs = 0, uint64_t _refreshNs = 0) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        gridNs = _gridNs;
        refreshNs = _refreshNs;
        repeat = std::max(1u, _repeat);
        queueDepth = _queueDepth;
        presentMode = _presentMode;
        if (vk) {
            bvk_set_present_mode(vk, presentMode);
            bvk_set_pacing(vk, repeat, queueDepth, gridNs, refreshNs);
        }
    }

    auto pacingCaps() -> PacingCaps {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        PacingCaps caps;
        if (vk) {
            BvkPacingCaps c = {};
            bvk_pacing_caps(vk, &c);
            caps.feedback = c.feedback;
            caps.timeDomain = c.time_domain;
            caps.refreshNs = c.refresh_ns;
            caps.stalled = c.stalled != 0;
            caps.absoluteTime = c.absolute_time != 0;
            caps.cadence = c.cadence != 0;
            caps.blend = c.blend != 0;
        }
        return caps;
    }

    auto pacingChanged() -> bool { return capsChanged.exchange(false); }

    auto setContentFps(double fps) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (fps > 0.0)
            contentFps = (float)fps;
    }

    // ------------------------------------------------------------ slang presets

    auto setSlangPreset(const std::string& path, const std::string& overrides, std::string& result) -> bool {
        char err[2048] = {0};
        BvkPreset* preset = nullptr;
        if (!path.empty()) {
            // parse + compile without the render lock (the emulation keeps presenting)
            preset = bvk_preset_load(path.c_str(), overrides.c_str(), err, sizeof err);
            if (!preset) {
                result = err;
                return false;
            }
            result = bvk_preset_summary(preset);
        } else
            result = "null";
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!vk) {
            bvk_preset_free(preset);
            result = "no Vulkan renderer";
            return false;
        }
        if (bvk_set_preset(vk, preset, err, sizeof err) != BVK_OK) {
            result = err;
            return false;
        }
        return true;
    }

    auto setSlangParam(const std::string& id, float value) -> bool {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        return vk && bvk_set_param(vk, id.c_str(), value) == BVK_OK;
    }

    auto slangState() -> std::string {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!vk)
            return "null";
        std::string s(bvk_preset_state(vk, nullptr, 0) + 1, '\0');
        bvk_preset_state(vk, s.data(), s.size());
        s.resize(s.size() - 1);
        return s;
    }

    auto pacingSamples(std::vector<PacingSample>& out, bool clear) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        collectSamples();
        out.assign(samples.begin(), samples.end());
        if (clear)
            samples.clear();
    }

    auto collectSamples() -> void {
        if (!vk)
            return;
        BvkPresent buf[64];
        unsigned n;
        while ((n = bvk_take_presents(vk, buf, 64)) > 0) {
            for (unsigned i = 0; i < n; i++) {
                if (samples.size() >= MaxSamples)
                    samples.pop_front();
                samples.push_back({buf[i].frame, buf[i].submit_ns, buf[i].display_ns, buf[i].flags, buf[i].copy, buf[i].target_ns});
            }
            if (n < 64)
                break;
        }
    }

    // ------------------------------------------------------------ frame

    auto lockIndexed(uint8_t*& data, unsigned& pitch, unsigned _width, unsigned _height, uint8_t _options = 0) -> bool {
        if (!vk)
            return false;
        std::lock_guard<std::recursive_mutex> lock(mutex);
        resizeFrame(_width, _height);
        if (indexBuffer.size() != (size_t)_width * _height)
            indexBuffer.assign((size_t)_width * _height, 0);
        options = _options;
        pitch = _width;
        data = indexBuffer.data();
        return data != nullptr;
    }

    auto resizeFrame(unsigned _width, unsigned _height) -> void {
        if (_width != frameWidth || _height != frameHeight) {
            frameWidth = _width;
            frameHeight = _height;
            viewScreen.update(viewport);
        }
    }

    auto setPicture(const Picture& p) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        std::memcpy(picture.palette, p.palette, sizeof picture.palette);
        picture.brightness = p.brightness;
        picture.contrast = p.contrast;
        picture.saturation = p.saturation;
        picture.gamma = p.gamma;
        picture.pal_pass = p.palPass;
        picture.ntsc = p.ntsc;
        picture.first_line_odd = p.firstLineOdd;
        picture.phase_error = p.phaseError;
        picture.hanover = p.hanover;
        picture.luma_rise = p.lumaRise;
        picture.luma_fall = p.lumaFall;
        picture.chroma_taps = p.chromaTaps;
        picture.cutoff_u = p.cutoffU;
        picture.cutoff_v = p.cutoffV;
        hasPicture = true;
        if (vk)
            bvk_set_picture(vk, &picture);
    }

    auto unlockAndRedraw() -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        hasFrame = frameWidth && frameHeight;
        ++frameSeq;
        render(true, true, options & OPT_TakeScreenshot, true, frameSeq);
    }

    auto redraw() -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        render(false, true, false, true);
    }

    // only a black window (no overlay, no text)
    auto clear() -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        render(false, false, false, false);
    }

    auto updateWindowSize(bool force = false) -> void {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0)
            return;
        if (!force && (unsigned)w == viewScreen.windowWidth && (unsigned)h == viewScreen.windowHeight)
            return;
        viewScreen.update(viewport, w, h);
    }

    // seq: emulator frame sequence of a new picture (0 = redraw, presented once)
    auto render(bool upload, bool showFrame, bool screenshot, bool decorations, uint64_t seq = 0) -> void {
        if (!vk || !window)
            return;
        updateWindowSize();

        BvkFrame f = {};
        f.frame_seq = seq;
        f.window_width = viewScreen.windowWidth;
        f.window_height = viewScreen.windowHeight;
        if (upload && hasFrame) {
            f.indices = indexBuffer.data();
            f.frame_width = frameWidth;
            f.frame_height = frameHeight;
            f.frame_pitch = frameWidth;
        }
        f.show_frame = showFrame && hasFrame;
        // viewport.y counts from the bottom (GL convention of the shared viewport code): mirror it
        f.vp_x = viewport.x;
        f.vp_y = (int)viewScreen.windowHeight - viewport.y - (int)viewport.height;
        f.vp_width = viewport.width;
        f.vp_height = viewport.height;
        std::memcpy(f.mvp, mvp, sizeof mvp);
        f.linear = linearFilter && !(options & OPT_DisallowFilter);
        f.shader_off = (options & OPT_DisallowShader) != 0;
        f.rotation = (int)rotation;
        f.fps = contentFps;

        std::shared_ptr<const OverlayFrame> overlay;
        {
            std::lock_guard<std::mutex> lock(overlayMutex);
            overlay = overlayFrame;
        }
        if (decorations && overlay && !overlay->vertices.empty() && overlay->atlas) {
            if (!atlasUploaded || atlasId != overlay->atlasId) {
                setTexture(BVK_TEX_ATLAS, BVK_FMT_R8, overlay->atlasWidth, overlay->atlasHeight, overlay->atlas->data());
                atlasId = overlay->atlasId;
                atlasUploaded = true;
            }
            f.overlay_vertices = overlay->vertices.data();
            f.overlay_vertex_count = overlay->vertices.size() / 8;
            f.overlay_width = overlay->width;
            f.overlay_height = overlay->height;
        }

        std::vector<uint8_t> shot;
        if (screenshot && screenshotCallback) {
            // with the menu or drop tiles the whole window is captured, otherwise the picture area
            if (f.overlay_vertex_count && overlay->fullWindow) {
                f.shot_x = 0, f.shot_y = 0, f.shot_width = f.window_width, f.shot_height = f.window_height;
            } else {
                f.shot_x = f.vp_x, f.shot_y = f.vp_y, f.shot_width = f.vp_width, f.shot_height = f.vp_height;
            }
            // clip to the window (picture larger than the window)
            int x0 = std::max(f.shot_x, 0), y0 = std::max(f.shot_y, 0);
            int x1 = std::min(f.shot_x + f.shot_width, (int)f.window_width);
            int y1 = std::min(f.shot_y + f.shot_height, (int)f.window_height);
            f.shot_x = x0, f.shot_y = y0, f.shot_width = x1 - x0, f.shot_height = y1 - y0;
            if (f.shot_width > 0 && f.shot_height > 0) {
                shot.resize((size_t)f.shot_width * f.shot_height * 3);
                f.screenshot = shot.data();
            }
        }

        char err[512] = {0};
        int res = bvk_render(vk, &f, err, sizeof err);
        collectSamples();
        BvkPacingCaps caps = {};
        bvk_pacing_caps(vk, &caps);
        if ((caps.stalled != 0) != stalled) {
            stalled = caps.stalled != 0;
            capsChanged = true;
        }
        // target times are known only once the swapchain exists, i.e. after the pacing was
        // first decided at startup (without this re-evaluation the start stayed audio master)
        if ((caps.absolute_time != 0) != absoluteTime) {
            absoluteTime = caps.absolute_time != 0;
            capsChanged = true;
        }
        if (res == BVK_ERROR) {
            if (!reportedError)
                fprintf(stderr, "Vulkan: %s\n", err);
            reportedError = true;
            return;
        }
        // no black file when nothing was captured: a skipped frame loses the shot (MCP reports it)
        if (res == BVK_OK && f.screenshot)
            screenshotCallback(shot.data(), f.shot_width, f.shot_height);
        else if (res == BVK_NO_SHOT || (res == BVK_SKIPPED && f.screenshot))
            fprintf(stderr, "Vulkan: screenshot not captured\n");
    }

    auto setTexture(int slot, int format, unsigned w, unsigned h, const void* data) -> void {
        char err[512] = {0};
        if (bvk_set_texture(vk, slot, format, w, h, data, err, sizeof err) != BVK_OK)
            fprintf(stderr, "Vulkan: %s\n", err);
    }

    // ------------------------------------------------------------ overlay

    auto setOverlay(std::shared_ptr<const OverlayFrame> frame) -> void {
        std::lock_guard<std::mutex> lock(overlayMutex);
        overlayFrame = frame;
    }

    auto hasOverlay() -> bool { return vk != nullptr; }

    // ------------------------------------------------------------ geometry

    auto updateMvp() -> void {
        Matrix4x4 projection = {
            2.0f,  0.0f, 0.0f, 0.0f,
            0.0f,  2.0f, 0.0f, 0.0f,
            0.0f,  0.0f, 2.0f, 0.0f,
            -1.0f,-1.0f, 0.0f, 1.0f,
        };
        projection.data[10] = -1.0;

        // same matrix as GL3::updateRotation
        if (rotation) {
            float radian = M_PI * ((float)rotation * 270.0 / 180.0f);
            Matrix4x4 rot = {
                {cosf(radian), sinf(radian), 0.0f, 0.0f,
                 -sinf(radian), cosf(radian), 0.0f, 0.0f,
                 0.0f, 0.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 0.0f, 1.0f}};
            MatrixMultiply(mvp, projection.data, 4, 4, rot.data, 4, 4);
        } else
            std::memcpy(mvp, projection.data, sizeof mvp);

        mvp[1] *= -1.0f;
        mvp[5] *= -1.0f;
        mvp[9] *= -1.0f;
        mvp[13] *= -1.0f;
    }

    auto setLinearFilter(bool state) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        linearFilter = state;
    }

    auto setAspectRatio(int mode, bool _integerScaling) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        viewScreen.mode = (ViewScreen::Mode)mode;
        viewScreen.hasIntegerScaling = _integerScaling;
        viewScreen.update(viewport);
    }

    auto getAspectRatio() -> int { return (int)viewScreen.mode; }

    auto setRotation(Rotation _rotation) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        rotation = _rotation;
        viewScreen.flipped = rotation == ROT_90 || rotation == ROT_270;
        viewScreen.update(viewport);
        updateMvp();
    }

    auto getRotation() -> Rotation { return rotation; }

    auto setIntegerScalingDimension(unsigned _w, unsigned _h, bool _ds) -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        viewScreen.scaling.width = _w;
        viewScreen.scaling.height = _h;
        viewScreen.scaling.doubleSize = _ds;
    }

    auto getIntegerScalingDimension(unsigned& _w, unsigned& _h) -> void {
        _w = viewScreen.scaling.width >> 1;
        _h = viewScreen.scaling.height >> 1;
    }

    auto getViewport() -> Viewport& { return viewport; }

    auto forceResize() -> void {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (window)
            updateWindowSize(true);
    }

    auto lockResize() -> void { mutex.lock(); }

    auto unlockResize() -> void {
        if (window)
            updateWindowSize();
        mutex.unlock();
    }

    auto setScreenshotCallback(ScreenshotCallback callback) -> void {
        screenshotCallback = callback;
    }
};

}
