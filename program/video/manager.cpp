#include <cmath>
#include <cstring>
#include "manager.h"
#include "../../guikit/api.h"
#include "../../emulation/libc64/interface.h"
#include "../thread/emuThread.h"
#include "sync.cpp"

unsigned VideoManager::takeScreenShots = 0;

std::vector<VideoManager*> videoManagers;

auto VideoManager::getInstance( Emulator::Interface* emulator ) -> VideoManager* {
    for (auto videoManager : videoManagers) {
        if (videoManager->emulator == emulator)
            return videoManager;
    }
    return nullptr;
}

VideoManager::VideoManager(Emulator::Interface* emulator) {
    this->emulator = emulator;
    this->settings = Program::getSettings(emulator);
    setPalette(&emulator->palettes[0]);
}

auto VideoManager::setPalette(Emulator::Interface::Palette* palette) -> void {
    this->palette = palette;
    unsigned count = std::min<unsigned>(palette->paletteColors.size(), 16);
    for (unsigned c = 0; c < count; c++) {
        auto& col = palette->paletteColors[c];
        colorTable[c] = 0xff000000 | (col.r << 16) | (col.g << 8) | col.b;
        lumaTable[c] = 0.299f * col.r + 0.587f * col.g + 0.114f * col.b;
    }
    updatePicture();
}

auto VideoManager::reloadSettings() -> void {
    auto& p = picture;
    p.saturation = settings->get<unsigned>("video_saturation", 100u, {0u, 200u});
    p.contrast = settings->get<unsigned>("video_contrast", 100u, {0u, 200u});
    p.brightness = settings->get<unsigned>("video_brightness", 100u, {0u, 200u});
    p.gamma = settings->get<unsigned>("video_gamma", 100u, {30u, 280u});
    p.palPass = settings->get<bool>("video_pal_pass", true);
    p.phaseErrorPal = settings->get<float>("video_pal_phase_error", 22.5f, {-45.0f, 45.0f});
    p.phaseErrorNtsc = settings->get<float>("video_ntsc_phase_error", 0.0f, {-45.0f, 45.0f});
    p.hanover = settings->get<int>("video_pal_hanover", 0, {-50, 50});
    p.lumaRise = settings->get<float>("video_luma_rise", 2.0f, {1.0f, 4.0f});
    p.lumaFall = settings->get<float>("video_luma_fall", 1.2f, {1.0f, 4.0f});
    p.chromaTaps = settings->get<unsigned>("video_chroma_taps", 0u, {0u, 21u});
    updatePicture();
}

auto VideoManager::savePicture() -> void {
    auto& p = picture;
    settings->set<unsigned>("video_saturation", p.saturation);
    settings->set<unsigned>("video_contrast", p.contrast);
    settings->set<unsigned>("video_brightness", p.brightness);
    settings->set<unsigned>("video_gamma", p.gamma);
    settings->set<bool>("video_pal_pass", p.palPass);
    settings->set<float>("video_pal_phase_error", p.phaseErrorPal);
    settings->set<float>("video_ntsc_phase_error", p.phaseErrorNtsc);
    settings->set<int>("video_pal_hanover", p.hanover);
    settings->set<float>("video_luma_rise", p.lumaRise);
    settings->set<float>("video_luma_fall", p.lumaFall);
    settings->set<unsigned>("video_chroma_taps", p.chromaTaps);
    reloadSettings();
}

auto VideoManager::updatePicture() -> void {
    if (!videoDriver)
        return;
    auto& p = picture;
    DRIVER::Picture d;
    std::memcpy(d.palette, colorTable, sizeof d.palette);
    for (auto& c : d.palette)
        c &= 0xffffff;
    d.saturation = (float)p.saturation / 100.0f;
    d.contrast = (float)p.contrast / 100.0f;
    d.brightness = (float)p.brightness - 100.0f;
    d.gamma = (float)p.gamma / 100.0f;
    d.palPass = p.palPass;
    d.ntsc = emulator->getRegionEncoding() == Emulator::Interface::Region::Ntsc;
    d.firstLineOdd = cropTopParity == 1;
    d.phaseError = d.ntsc ? p.phaseErrorNtsc : p.phaseErrorPal;
    d.hanover = (float)p.hanover;
    d.lumaRise = p.lumaRise;
    d.lumaFall = p.lumaFall;
    d.chromaTaps = (int)p.chromaTaps / 2;
    // chroma bandwidth (U/V 1.3 MHz, I 1.5 MHz, Q 0.5 MHz) relative to the pixel clock
    double pixelClock;
    switch (emulator->getSubRegion()) {
        case Emulator::Interface::SubRegion::Pal_B: pixelClock = 7.881984e6; break;
        case Emulator::Interface::SubRegion::Pal_N: pixelClock = 8.187e6; break;
        default: pixelClock = 8.181818e6; break;
    }
    d.cutoffU = (float)((d.ntsc ? 1.5e6 : 1.3e6) / pixelClock);
    d.cutoffV = (float)((d.ntsc ? 0.5e6 : 1.3e6) / pixelClock);
    videoDriver->setPicture(d);
}

auto VideoManager::toggleShaderTemporary() -> bool {
    suppressShaderByHotkey ^= 1;
    return suppressShaderByHotkey;
}

auto VideoManager::renderFrame(const uint8_t* src, unsigned width, unsigned height, unsigned srcPitch) -> void {
    bool suppressShader = suppressShaderByHotkey
        || (program->warp.disableShader && ((program->warp.mode == Program::Warp::Normal) || (program->warp.mode == Program::Warp::Aggressive)));

    uint8_t gpuOptions = 0;
    if (suppressShader) gpuOptions |= DRIVER::OPT_DisallowShader;

    // raster parity of the first picture line: PAL line alternation in the decode shader
    int parity = (int)(emulator->cropTop() & 1);
    if (parity != cropTopParity) {
        cropTopParity = parity;
        updatePicture();
    }

    if (unlikely(takeScreenShots)) {
        auto& screenshot = program->screenshot;

        if (!--screenshot.intervalPos) {
            screenshot.intervalPos = screenshot.interval;

            if (screenshot.unscaled) {
                takeScreenshot(screenshot.unscaled, src, width, height, srcPitch);
            } else {
                gpuOptions |= (uint8_t)DRIVER::OPT_TakeScreenshot;
                if (!screenshot.withEffects)
                    gpuOptions |= (uint8_t)DRIVER::OPT_DisallowShader | (uint8_t)DRIVER::OPT_DisallowFilter;
            }

            if (!--takeScreenShots && screenshot.pause)
                program->isPause = screenshot.pause;
        }
    }

    videoDriver->setIntegerScalingDimension(width << 1, height << 1, false);

    uint8_t* indices;
    unsigned pitch;
    if (!videoDriver->lockIndexed(indices, pitch, width, height, gpuOptions))
        return;
    const uint8_t* s = src;
    for (unsigned h = 0; h < height; h++) {
        std::memcpy(indices, s, width);
        indices += pitch;
        s += width + srcPitch;
    }

    if (audioManager->lumaInterference.enabled)
        sampleLuma(width, height, src, srcPitch);

    videoDriver->unlockAndRedraw();
}

// unscaled screenshot (setting screenshot_unscaled): indices with palette or plain palette RGB
auto VideoManager::takeScreenshot(unsigned unscaled, const uint8_t* src, unsigned width, unsigned height, unsigned pitch) -> void {
    if (unscaled > 1) {
        if (unscaled == 2)
            width = 320;
        else if (unscaled == 3)
            width = 384;

        src = activeEmulator->cropAlternatively(width, height, pitch);
        if (!src)
            return;
    }

    bool indexed = program->screenshot.writePalette;
    std::vector<uint8_t> dest((size_t)width * height * (indexed ? 1 : 3));
    uint8_t* d = dest.data();
    for (unsigned h = 0; h < height; h++) {
        for (unsigned w = 0; w < width; w++) {
            uint8_t i = src[w] & 15;
            if (indexed) {
                *d++ = i;
            } else {
                uint32_t c = colorTable[i];
                *d++ = (c >> 16) & 0xff;
                *d++ = (c >> 8) & 0xff;
                *d++ = c & 0xff;
            }
        }
        src += width + pitch;
    }
    program->takeScreenshot(dest.data(), width, height);
}

auto VideoManager::sampleLuma(unsigned width, unsigned height, const uint8_t* src, unsigned srcPitch) -> void {
    float lum;
    auto& ali = audioManager->lumaInterference;

    for (unsigned h = 0; h < height; h++) {
        lum = 0.0;
        for (unsigned w = 0; w < width; w++)
            lum += lumaTable[*src++ & 15];

        ali.avgLines[h] = lum / (static_cast<float>(width) * 3.0f);
        src += srcPitch;
    }

    lum = 0.0;
    for (unsigned h = 0; h < height; h++)
        lum += ali.avgLines[h];

    ali.avgFrame = lum / static_cast<float>(height);

    ali.avgLines[height] = 0.0;
    ali.lines = height + 1;
}
