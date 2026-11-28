
#include "../window/sdlWindow.h"
#include "manager.h"
#include "../audio/manager.h"

VideoManager::Pacing VideoManager::pacing;

// Vulkan (M-Pacing1, SPEC "Decisions M-Pacing"): setting `pacing` 0 = Auto, 1 = Lowest latency.
// Auto: host refresh an integer multiple of the emulated rate within 0.5 % -> video master
// (each picture presented n times, every present waits for the previous one, audio follows
// via DRC). Otherwise, with absolute present target times (M-Pacing2): grid -- each picture
// waits for its slot of a steady grid at the emulated rate and is presented with that target time
// (the grid clocks the emulation, audio follows via DRC); with VRR the display follows the grid,
// without it (and a refresh above the emulated rate) the renderer blends (M-Pacing3): after a
// short grid probe (VRR detection) every vblank shows a phase-weighted mix of two pictures.
// No target times (X11 ...): audio master + FIFO. Lowest latency: audio master, IMMEDIATE (tearing, hint set by Mesa) or MAILBOX.
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
        if (p.fps > 0.0 && videoDriver->pacingCaps().absoluteTime) {
            p.state = Pacing::Grid;
            p.gridNs = (uint64_t)std::llround(1e9 / p.fps);
        }
        if (p.refresh > 0.0 && p.fps > 0.0) {
            unsigned n = (unsigned)std::lround(p.refresh / p.fps);
            if (n >= 1 && n <= 8 && std::abs(p.refresh / (n * p.fps) - 1.0) <= 0.005) {
                p.state = Pacing::VideoMaster;
                p.gridNs = 0;
                p.repeat = n;
                // the emulation runs at refresh / n instead of its nominal rate
                p.clock = p.refresh / (n * p.fps);
            }
        }
    }

    // depth 2: the present for the next vblank must be queued before the current one shows
    // (Hyprland composes right at vblank; measured with depth 1: every present took 2 periods)
    p.queueDepth = p.state == Pacing::VideoMaster ? 2 : 0;
    if (std::getenv("BADLINE_VK_DEBUG") && (p.state != pacing.state || p.repeat != pacing.repeat))
        fprintf(stderr, "badline pacing: state %d -> %d (repeat %u, refresh %.3f, stalled %d)\n",
            (int)pacing.state, (int)p.state, p.repeat, p.refresh, (int)p.stalled);
    pacing = p;
    uint64_t refreshNs = p.refresh > 0.0 ? (uint64_t)std::llround(1e9 / p.refresh) : 0;
    videoDriver->setContentFps(p.fps * p.clock);
    videoDriver->setPacing(p.repeat, p.queueDepth, presentMode, p.gridNs, refreshNs);

    if (audioManager) {
        // unthrottled (warp) keeps the audio buffer setup: no PulseAudio rebuild per warp toggle
        bool displayClock = p.state == Pacing::VideoMaster || p.state == Pacing::Grid;
        audioManager->setVideoMaster(displayClock, p.clock, p.state != Pacing::Unthrottled);
    }
}

auto VideoManager::pacingStateName() -> std::string {
    switch (pacing.state) {
        case Pacing::VideoMaster: return "video_master";
        case Pacing::Grid: {
            auto caps = videoDriver->pacingCaps();
            return caps.blend ? "blend" : caps.cadence ? "grid_vrr" : "grid";
        }
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
    // no renderer (headless, -no-driver, failed init): nothing to pace against
    pacing = {};
    if (audioManager) {
        audioManager->setVideoMaster(false, 1.0, true);
    }
}
