
#include "../thread/renderThread.h"
#include "gl3.cpp"

// OpenGL 3.2 core output into an SDL3 window (EGL on Wayland, GLX on X11 via SDL).
// The window is created and owned by the program layer; init() receives the SDL_Window*.

namespace DRIVER {
    
struct SDLGL : public Video, GL3, RenderThread {

    SDL_Window* window = nullptr;
    SDL_GLContext glcontext = nullptr;
    bool useVRR = false;
    bool useResizing = false;
    uint8_t options = 0;
    unsigned shaderResizeTimer = 0;

    bool hasRendererContext = false;

    auto setOverlay(std::shared_ptr<const OverlayFrame> frame) -> void {
        overlay.set(frame);
    }

    auto hasOverlay() -> bool { return overlay.initialized; }

    auto setShaderCacheCallback( std::function<void (DiskFile& diskFile)> onCallback ) -> void {
        onShaderCacheCallback = onCallback;
    }

    auto setShaderProgressCallback( std::function<void (int pass, bool hasErrors)> onCallback ) -> void {
        onShaderProgressCallback = onCallback;
    }

    auto useShaderCache(bool state) -> void {
        settings.useShaderCache = state;
    }

    auto setSwapInterval(bool on) -> void {
        SDL_GL_SetSwapInterval(on ? 1 : 0);
    }

    auto swap() -> void {
        SDL_GL_SwapWindow(window);
    }

    auto init() -> bool {
        term();

        if (!window)
            return false;

        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

        glcontext = SDL_GL_CreateContext(window);
        if (!glcontext) {
            fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
            return false;
        }

        SDL_GL_MakeCurrent(window, glcontext);

        glGetIntegerv(GL_MAJOR_VERSION, &version.major);
        glGetIntegerv(GL_MINOR_VERSION, &version.minor);
        version.glsl = glGetString( GL_SHADING_LANGUAGE_VERSION );

        if (version.major == 0) {
            const char* _version = (const char*) glGetString(GL_VERSION);

            if (!_version || sscanf(_version, "%u.%u", &version.major, &version.minor) != 2) {
                version.major = 3;
                version.minor = 2;
            }
        }

        setSwapInterval(settings.synchronize);

        RenderThread::reset();
        bool res = GL3::init();
        clearCurrent();
        return res;
    }

    // shared context for the shader build worker; called while the main context is current
    auto getContext(bool shared = true) -> uintptr_t {
        GLUtility::sharedMutex.lock();
        SDL_GLContext prev = SDL_GL_GetCurrentContext();
        SDL_Window* prevWindow = SDL_GL_GetCurrentWindow();
        if (shared && !prev)
            SDL_GL_MakeCurrent(window, glcontext);

        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, shared ? 1 : 0);
        SDL_GLContext context = SDL_GL_CreateContext(window);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

        // creating a context makes it current: restore the previous binding
        if (prev)
            SDL_GL_MakeCurrent(prevWindow, prev);
        else
            SDL_GL_MakeCurrent(window, nullptr);
        GLUtility::sharedMutex.unlock();
        return (uintptr_t)context;
    }

    auto deleteContext(uintptr_t context) -> void {
        if(context) {
            GLUtility::sharedMutex.lock();
            if (SDL_GL_GetCurrentContext() == (SDL_GLContext)context)
                SDL_GL_MakeCurrent(window, nullptr);
            SDL_GL_DestroyContext((SDL_GLContext)context);
            GLUtility::sharedMutex.unlock();
        }
    }

    // worker thread: bind without surface if possible (the window surface is bound by the renderer)
    auto makeContextCurrent(uintptr_t context) -> void {
        GLUtility::sharedMutex.lock();
        if (!SDL_GL_MakeCurrent(nullptr, (SDL_GLContext)context))
            SDL_GL_MakeCurrent(window, (SDL_GLContext)context);
        GLUtility::sharedMutex.unlock();
    }

    auto init(uintptr_t _handle) -> bool {
        window = (SDL_Window*)_handle;
        return init();
    }
    
    auto synchronize(bool state) -> void {
        wait();
        resizeMutex.lock();
        settings.synchronize = state;
        makeCurrent();
        setSwapInterval(settings.synchronize);
        clearCurrent();
        resizeMutex.unlock();
    }
    
    auto hasSynchronized() -> bool { return settings.synchronize; }
    
    auto hardSync(bool state) -> void {
        wait();
        settings.hardSync = state;
    }

    auto setThreaded(bool state) -> void {

        if (state != threadEnabled) {
            RenderThread::enable(state);

            RenderThread::reset();
            auto& tex = frame.textures[0];
            tex.width = 0, tex.height = 0;

            clearCurrent();
        }
    }

    auto hasThreaded() -> bool { return threadEnabled; }

    auto shaderSupport() -> bool { return true; }
    
    auto setShader(ShaderPreset* preset) -> void {
        wait();
        resizeMutex.lock();
        makeCurrent();
        GL3::shader( preset );
        RenderThread::reset();
        clearCurrent();
        resizeMutex.unlock();
    }

    auto setLinearFilter(bool state) -> void {
        if (state == settings.linearFilter)
            return;
        wait();
        settings.linearFilter = state;
        resizeMutex.lock();
     //   makeCurrent();

        GL3::updateFilter();
      //  clearCurrent();
        resizeMutex.unlock();
    }

	auto lock(unsigned*& data, unsigned& pitch, unsigned _width, unsigned _height, uint8_t options = 0) -> bool {
        if (shaderReady) {
            wait();
            makeCurrent();
            GL3::shaderPostBuild();
            clearCurrent();
            shaderReady = false;
        }

        if (!shaderPasses && (options & OPT_RGB10) ) // YUV input needs a shader to progress it
            return false;

        if (threadEnabled)
            return RenderThread::lock(data, pitch, _width, _height, options);

        // resizing could generate 2 "makeCurrent" in a row without "clear" in between,
        bool _useResizing = useResizing;
        if (_useResizing)
            resizeMutex.lock();

        this->options = options;
        makeCurrent(true);
        if (GL3::initTexture(_width, _height, GL_RGBA8)) {
            updateRTS = true;
            updateHistory = true;
            viewScreen.update(viewport);
        }

        if (_useResizing) {
            clearCurrent();
            resizeMutex.unlock();
        }

		return GL3::lock(data, pitch);
	}

    auto adjustSize(unsigned& w, unsigned& h) -> void {
        viewScreen.update(viewport);
        updateRTS = true;
        updateHistory = true;
    }

    auto clear() -> void {
        wait();
        makeCurrent();
        GL3::clear();
        swap();
        clearCurrent();
    }

    auto forceResize() -> void {
        resizeWindow(true);
    }

    auto resizeWindow(bool _force = false) -> void {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0)
            return;

        unsigned _windowWidth = w;
        unsigned _windowHeight = h;

        if (!_force) {
            if ( (_windowWidth == viewScreen.windowWidth) && (_windowHeight == viewScreen.windowHeight) )
                return;
        }

        viewScreen.update(viewport, _windowWidth, _windowHeight);
#ifdef DRV_FREETYPE
        screenText.ftUpdateCoords();
#endif
        shaderResizeTimer = 10;
    }

    auto lockResize() -> void {
        resizeMutex.lock();
    //    resizeMutexThreaded.lock();
    }

    auto unlockResize() -> void {
        resizeWindow();

      //  resizeMutexThreaded.unlock();
        resizeMutex.unlock();
    }

    auto hintResizing(bool state) -> void {
        if (useVRR && useResizing && !state)
            lastCapTime = Chronos::getTimestampInMicroseconds();

        useResizing = state;
    }

    auto needResizingPreparations(bool useEmuThread) -> bool {
        return settings.synchronize || settings.vrr;
    }

    auto prepareResizing() -> void {
        wait();
        makeCurrent();
        if (settings.synchronize)
            setSwapInterval(false);
        useVRR = false;
        clearCurrent();
    }

    auto endResizing() -> void {
        wait();
        makeCurrent();
        if (settings.synchronize)
            setSwapInterval(true);
        useVRR = settings.vrr;
        clearCurrent();
    }

    auto unlockAndRedraw() -> void {
        if (threadEnabled) {
            resizeWindow();
            RenderThread::unlock();
        } else {
            _redraw();
        }
    }

    auto redraw() -> void {
        resizeMutex.lock();
        resizeMutexThreaded.lock();

        resizeWindow();
        makeCurrent();
        GL3::updateMainTexture( threadEnabled ? getLastBufferToRender() : nullptr );
        GL3::_redraw(options);
        overlay.show(viewScreen.windowWidth, viewScreen.windowHeight);
#ifdef DRV_FREETYPE
        screenText.showText(viewport);
#endif
        clearCurrent();

        resizeMutexThreaded.unlock();
        resizeMutex.unlock();
    }

    auto _redraw() -> void {
        resizeWindow();
        resizeMutex.lock();
        makeCurrent(true);

        if (shaderResizeTimer && !--shaderResizeTimer) {
            GL3::updateFrameSize();
        }
        GL3::updateMainTexture( threadEnabled ? getLastBufferToRender() : nullptr );

        subFrame = 1;
        if (settings.bfiFrames)
            totalFrames = (options & OPT_Pause) ? 1 : (settings.bfiFrames + 1);
        render();

        if (settings.bfiFrames && (settings.synchronize || useVRR) && !(options & (OPT_DisallowShader | OPT_Pause)))
            bfi();

        if (useResizing)
            clearCurrent();

        resizeMutex.unlock();
    }

    auto render() -> void {
        GL3::_redraw(options);

        overlay.show(viewScreen.windowWidth, viewScreen.windowHeight);

#ifdef DRV_FREETYPE
        screenText.showText(viewport);
#endif
        if (progressVisible && progress.initialized)
            progress.show(viewport, 20, 20);

        if ((options & OPT_TakeScreenshot) && subFrame == 1)
            takeScreenshot(GL_BACK);

        if (useVRR) {
            glFinish();
            waitVRR();
            swap();
        } else {
            swap();
            if (settings.hardSync && settings.synchronize) glFinish();
        }
    }

    auto refresh() -> void {
        resizeMutexThreaded.lock();
        makeCurrent();

        options = 0;
        RenderBuffer* renderBuffer = getBufferToRender();

        if (renderBuffer && renderBuffer->height) {
            renderBuffer->sharedMutex.lock();
            GL3::initTexture(renderBuffer->width, renderBuffer->height,GL_RGBA8);

            updateMainTexture(renderBuffer);
            options = renderBuffer->options;
            renderBuffer->sharedMutex.unlock();

            accessMutex.lock();
            frames--;
            accessMutex.unlock();
        }
        if (shaderResizeTimer && !--shaderResizeTimer) {
            GL3::updateFrameSize();
        }

        subFrame = 1;
        if (settings.bfiFrames)
            totalFrames = (options & OPT_Pause) ? 1 : (settings.bfiFrames + 1);
        render();

        if (settings.bfiFrames && (settings.synchronize || useVRR) && !(options & (OPT_DisallowShader | OPT_Pause)))
            bfi();

        clearCurrent();
        resizeMutexThreaded.unlock();
    }

    auto bfi() -> void {
        for (int i = 0; i < settings.lightFrames; i++) {
            subFrame++;
            render();
        }

        for (int i = 0; i < settings.darkFrames; i++) {
            GL3::clear();
            if (useVRR) {
                glFinish();
                waitVRR();
            }
            swap();
        }
    }

    auto term() -> void {
        wait();
        GL3::term();

        if (glcontext) {
            SDL_GL_MakeCurrent(window, nullptr);
            SDL_GL_DestroyContext(glcontext);
        }
        glcontext = nullptr;
        hasRendererContext = false;
    }

    auto makeCurrent(bool usePermanent = false) -> void {

        if (usePermanent) {
            if(!hasRendererContext) {
                hasRendererContext = true;
            } else
                // for non threaded mode, we don't want to bind context each frame for speed reasons
                return;
        }

        SDL_GL_MakeCurrent(window, glcontext);
    }

    auto setAspectRatio(int mode, bool _integerScaling) -> void { // mode: 0: off, 1: TV, 2: Native
        if ((int)viewScreen.mode == mode && viewScreen.hasIntegerScaling == _integerScaling)
            return;

        wait();
        viewScreen.mode = (ViewScreen::Mode)mode;
        viewScreen.hasIntegerScaling = _integerScaling;

        viewScreen.update(viewport);
#ifdef DRV_FREETYPE
        screenText.ftUpdateCoords();
#endif
        GL3::updateFrameSize();
        updateHistory = true;
    }

    auto getAspectRatio() -> int {
        return (int)viewScreen.mode;
    }

    auto getRotation() -> Rotation { return settings.rotation; }

    auto setRotation(Rotation rotation) -> void {
        if (settings.rotation == rotation)
            return;
        wait();
        settings.rotation = rotation;
        makeCurrent();
        GL3::updateRotation();
        clearCurrent();
        updateRTS = true;
        updateHistory = true;
        frameCount = 0;
    }

    auto setIntegerScalingDimension( unsigned _w, unsigned _h, bool _ds) -> void {
        viewScreen.scaling.width = _w;
        viewScreen.scaling.height = _h;
        viewScreen.scaling.doubleSize = _ds;
    }

    auto getIntegerScalingDimension(unsigned& _w, unsigned& _h) -> void {
        _w = viewScreen.scaling.width >> 1;
        _h = viewScreen.scaling.height >> 1;
    }

    auto getViewport() -> Viewport& { return viewport; }

    auto setVRR(bool state, float speed = 0.0) -> void {
        wait();
        settings.vrr = state;
        if (speed != 0.0)
            settings.vrrSpeed = speed;
        useVRR = state;
        updateVRR();
    }

    auto setBFI(unsigned frames, unsigned darkFrames) -> void {
        wait();
        subFrame = 1;
        totalFrames = frames + 1;
        settings.bfiFrames = frames;
        settings.darkFrames = darkFrames > frames ? frames : darkFrames;
        settings.lightFrames = frames - settings.darkFrames;
        updateVRR();
    }

    auto waitRenderThread() -> void { if (threadEnabled) wait(); }

    auto hasVRR() -> bool { return settings.vrr; }

    auto getAppData() -> AppData* { return &appData; }

    auto getShaderNativeVertexCode(std::string& slang, std::string& out) -> bool {
        return GLUtility::translate(version, slang, out, false);
    }

    auto getShaderNativeFragmentCode(std::string& slang, std::string& out) -> bool {
        return GLUtility::translate(version, slang, out, true);
    }

    auto setProgressAnimation(uint8_t* _data, unsigned _width, unsigned _height) -> void {
        wait();
        makeCurrent();
        progress.setIcon(_data, _width, _height);
        clearCurrent();
    }

    auto clearCurrent() -> void {
        SDL_GL_MakeCurrent(window, nullptr);
        hasRendererContext = false;
    }

#ifdef DRV_FREETYPE
    auto showScreenText(const std::string text, unsigned duration, bool warn = false) -> void {
        screenText.ftUpdateMessage(text, duration, warn);
    }

    auto setScreenTextDescription(ScreenTextDescription& desc) -> void {
        screenText.ftSetScreenTextDescription(desc);
    }

    auto freeFont() -> void {
        wait();
        screenText.ftUnload();
    }
#endif

    auto freeContext() -> void {
        clearCurrent();
    }

    auto setScreenshotCallback(ScreenshotCallback callback) -> void {
        GL3::setScreenshotCallback(callback);
    }

    auto canHardSync() -> bool { return true; }

    SDLGL() {

    }

    ~SDLGL() {
        GL3::stopShaderBuilding();
        wait();
        RenderThread::enable(false);
        term();
    }
};

}
