
#include "../window/sdlWindow.h"
#include "manager.h"
#include "../audio/manager.h"

#define VIDEO_SKEW 0.0015

VideoManager::Pacing VideoManager::pacing;

// Vulkan (M-Pacing1, SPEC "Decisions M-Pacing"): setting `pacing` 0 = Auto, 1 = Lowest latency.
// Auto: host refresh an integer multiple of the emulated rate within 0.5 % -> video master
// (each picture presented n times, every present waits for the previous one, audio follows
// via DRC); otherwise audio master + FIFO (VRR/blending follow in M-Pacing2).
// Lowest latency: audio master, IMMEDIATE (tearing) or MAILBOX.
auto VideoManager::setPacing() -> void {
    Pacing p;
    p.mode = globalSettings->get<unsigned>("pacing", 0u, {0u, 1u});
    p.refresh = sdlWindow ? sdlWindow->refreshRate() : 0.0;
    p.fps = audioManager ? audioManager->inputFPS : 0.0;
    bool fast = program->warp.mode != Program::Warp::Off || program->isMaximumSpeed();

    int presentMode = DRIVER::PRESENT_FIFO;
    if (fast) {
        p.state = Pacing::Unthrottled;
        presentMode = DRIVER::PRESENT_MAILBOX;
    } else if (videoDriver->pacingCaps().stalled) {
        // nothing is displayed (hidden window): waiting for the display would stall the
        // emulation; audio keeps the time until presents are shown again
        p.state = Pacing::AudioMaster;
        p.stalled = true;
    } else if (p.mode == 1) {
        p.state = Pacing::LowestLatency;
        presentMode = DRIVER::PRESENT_IMMEDIATE;
    } else {
        p.state = Pacing::AudioMaster;
        if (p.refresh > 0.0 && p.fps > 0.0) {
            unsigned n = (unsigned)std::lround(p.refresh / p.fps);
            if (n >= 1 && n <= 8 && std::abs(p.refresh / (n * p.fps) - 1.0) <= 0.005) {
                p.state = Pacing::VideoMaster;
                p.repeat = n;
                // the emulation runs at refresh / n instead of its nominal rate
                p.clock = p.refresh / (n * p.fps);
            }
        }
    }

    // depth 2: the present for the next vblank must be queued before the current one shows
    // (Hyprland composes right at vblank; measured with depth 1: every present took 2 periods)
    p.queueDepth = p.state == Pacing::VideoMaster ? 2 : 0;
    pacing = p;
    videoDriver->setPacing(p.repeat, p.queueDepth, presentMode);
    setFrameRender(1);

    if (audioManager) {
        audioManager->allowDrc = false;
        // unthrottled (warp) keeps the audio buffer setup: no PulseAudio rebuild per warp toggle
        audioManager->setVideoMaster(p.state == Pacing::VideoMaster, p.clock, p.state != Pacing::Unthrottled);
    }
}

auto VideoManager::pacingStateName() -> std::string {
    switch (pacing.state) {
        case Pacing::VideoMaster: return "video_master";
        case Pacing::AudioMaster: return "audio_master";
        case Pacing::LowestLatency: return "lowest_latency";
        case Pacing::Unthrottled: return "unthrottled";
        default: return "none";
    }
}

auto VideoManager::setSynchronize() -> void {
    if (!activeEmulator)
        return;

    if (videoDriver->supportsPacing())
        return setPacing();
    pacing = {};
    // e.g. after a runtime switch Vulkan -> GL: back to Denise's audio handling
    if (audioManager)
        audioManager->setVideoMaster(false, 1.0, true);

    bool vsync = false;
    bool vrr = false;
    unsigned tr = 0;

    if (program->warp.mode == Program::Warp::Off) {
        vsync = globalSettings->get<bool>("video_sync", true);
        vrr = globalSettings->get<bool>("vrr_sync", false);

        auto _settings = Program::getSettings( activeEmulator );
        tr = _settings->get<unsigned>("threaded_renderer", 0);
    }

    bool threadedRenderer = tr == 1;
    bool adaptive = tr == 2;

    unsigned frameRenderEach = 1;
    float skew = 0.0;

    if (audioDriver->hasSynchronized()) {
        if (vsync && adaptive) {
            float monitorFrequency = (sdlWindow ? sdlWindow->refreshRate() : 0.0f);
            if (monitorFrequency <= 0.0f)
                monitorFrequency = audioManager->inputFPS; // unknown (headless): no skew
            vrr = false;
            float ratio = (float)audioManager->inputFPS / monitorFrequency;
            float intpart;
            float fractpart = std::modf (ratio, &intpart);

            if ((uint8_t)intpart <= 1) {
                skew = std::abs(1.0 - ratio );
                if ((skew > VIDEO_SKEW) /*&& ((float) audioManager->inputFPS > monitorFrequency)*/)
                    threadedRenderer = true;
            } else {
                if (fractpart > VIDEO_SKEW)
                    threadedRenderer = true;
                else
                    frameRenderEach = (uint8_t)intpart;
            }
        }
    } else if (!threadedRenderer) {
        vrr = false;

        if (adaptive)
            threadedRenderer = true;
        else
            vsync = false;
    }

    if (program->warp.mode == Program::Warp::Off) {
        // don't change when toggling warp mode
        if (videoDriver->hasThreaded() != threadedRenderer)
            videoDriver->setThreaded( threadedRenderer );
    }

    if (videoDriver->hasSynchronized() != vsync)
        videoDriver->synchronize( vsync );

    setFrameRender( frameRenderEach );

    if (audioManager) {
        videoDriver->setVRR( vrr, (float)audioManager->inputFPS );
        audioManager->allowDrc = (vsync || vrr) && !threadedRenderer && (frameRenderEach == 1) && (skew <= VIDEO_SKEW);
        audioManager->setRateControl();
    }
}

auto VideoManager::setHardSync() -> void {
    videoDriver->hardSync( globalSettings->get<bool>("hardsync", false) );
}

#undef VIDEO_SKEW
