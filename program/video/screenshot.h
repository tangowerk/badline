#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include "../../guikit/api.h"

// pending screenshot request, filled by the hotkey/MCP and consumed by the video driver callback
struct ScreenshotState {
    std::string path;
    GUIKIT::Image::Type type;
    bool withEffects = false;
    unsigned unscaled = 0;
    bool twoFrames = false;
    bool saveState = false;
    unsigned pause = 0;
    bool writePalette = false;
    unsigned gun = 0;
    bool animatedGif = false;
    std::vector<uint8_t*> buffer;
    unsigned bufferSize = 0;
    unsigned interval = 0;
    unsigned intervalPos = 0;
    std::mutex sharedMutex;

    auto clearBuffer() -> void {
        for (auto ptr : buffer)
            if (ptr)
                delete[] ptr;
        buffer.clear();
        bufferSize = 0;
    }
};
