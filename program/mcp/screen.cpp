#include "screen.h"
#include "json.h"
#include "../../emulation/libc64/interface.h"
#include "../../emulation/libc64/system/system.h"
#include "../../guikit/api.h"
#include <cstdio>
#include <cstring>

namespace McpScreen {

// Screen codes 0x40-0x7f of the uppercase/graphics ROM charset.
// Graphics without an exact Unicode match use the closest common
// block/box drawing character so terminals can render them.
static const char* upperGraphics[64] = {
    "─", "♠", "│", "─", "─", "─", "─", "│",   // 40-47
    "│", "╮", "╰", "╯", "└", "╲", "╱", "┌",   // 48-4f
    "┐", "●", "▁", "♥", "▏", "╭", "╳", "○",   // 50-57
    "♣", "▕", "♦", "┼", "▒", "│", "π", "◥",   // 58-5f
    " ", "▌", "▄", "▔", "▁", "▏", "▒", "▕",   // 60-67
    "▒", "◤", "▕", "├", "▗", "└", "┐", "▂",   // 68-6f
    "┌", "┴", "┬", "┤", "▎", "▍", "▐", "▔",   // 70-77
    "▀", "▃", "┘", "▖", "▝", "┘", "▘", "▚",   // 78-7f
};

static const char* ascii20[32] = {
    " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
};

static const char* upperLetters[32] = {
    "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "£", "]", "↑", "←",
};

static const char* lowerLetters[32] = {
    "@", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "[", "£", "]", "↑", "←",
};

auto screenCodeToUtf8(uint8_t code, bool lowercase) -> const char* {
    code &= 0x7f; // reverse characters are not marked

    if (code < 0x20)
        return lowercase ? lowerLetters[code] : upperLetters[code];
    if (code < 0x40)
        return ascii20[code - 0x20];

    if (lowercase) {
        if (code >= 0x41 && code <= 0x5a)
            return upperLetters[code - 0x40];
        switch (code) {
            case 0x5e: return "▒";
            case 0x5f: return "▧";
            case 0x7a: return "✓";
        }
    }
    return upperGraphics[code - 0x40];
}

static auto screenBase(LIBC64::System* system) -> uint16_t {
    return system->vicII->getVicBank() | ((system->vicII->getReg18() & 0xf0) << 6);
}

auto lowercaseCharset(LIBC64::Interface* emu) -> bool {
    return (emu->system->vicII->getReg18() & 2) != 0;
}

auto readText(LIBC64::Interface* emu) -> std::string {
    auto system = emu->system;
    uint8_t* ram = system->ram;
    uint16_t addr = screenBase(system);
    bool lower = lowercaseCharset(emu);
    std::string out;
    out.reserve(25 * 41 * 2);

    for (unsigned row = 0; row < 25; row++) {
        for (unsigned col = 0; col < 40; col++)
            out += screenCodeToUtf8(ram[(uint16_t)(addr++)], lower);
        if (row != 24)
            out += '\n';
    }
    return out;
}

auto readRaw(LIBC64::Interface* emu) -> std::string {
    auto system = emu->system;
    uint8_t* ram = system->ram;
    uint16_t addr = screenBase(system);
    std::string out;
    char buf[4];

    for (unsigned row = 0; row < 25; row++) {
        for (unsigned col = 0; col < 40; col++) {
            std::snprintf(buf, sizeof(buf), col ? " %02x" : "%02x", ram[(uint16_t)(addr++)]);
            out += buf;
        }
        if (row != 24)
            out += '\n';
    }
    return out;
}

auto screenshot(LIBC64::Interface* emu, const std::vector<uint32_t>& palette, Border border, unsigned scale,
    std::string& pngBase64, unsigned& width, unsigned& height, std::string& error) -> bool {

    auto crop = emu->system->crop;
    auto& latest = crop->latest;

    if (!latest.frame) {
        error = "No frame rendered yet";
        return false;
    }
    if (palette.size() < 16) {
        error = "No palette available";
        return false;
    }

    const uint8_t* frame = latest.frame;
    unsigned lineLength = latest.linePitch + latest.width;
    width = latest.width;
    height = latest.height;

    if (border != Border::Visible) {
        // back to the uncropped frame
        frame -= latest.left;
        frame -= latest.top * lineLength;
        unsigned fullWidth = crop->original.width;
        unsigned fullHeight = crop->original.height;

        if (border == Border::Full) {
            width = fullWidth;
            height = fullHeight;
        } else {
            auto c = emu->system->vicII->getCrop(320, 200);
            if (c.left + c.right >= fullWidth || c.top + c.bottom >= fullHeight) {
                error = "Unexpected frame geometry";
                return false;
            }
            frame += c.top * lineLength + c.left;
            width = fullWidth - c.left - c.right;
            height = fullHeight - c.top - c.bottom;
        }
    }

    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;

    unsigned outW = width * scale;
    unsigned outH = height * scale;
    std::vector<uint8_t> rgb((size_t)outW * outH * 3);
    uint8_t* dst = rgb.data();

    for (unsigned y = 0; y < height; y++) {
        const uint8_t* src = frame + (size_t)y * lineLength;
        uint8_t* lineStart = dst;
        for (unsigned x = 0; x < width; x++) {
            uint32_t color = palette[src[x] & 0xf];
            for (unsigned s = 0; s < scale; s++) {
                *dst++ = (color >> 16) & 0xff;
                *dst++ = (color >> 8) & 0xff;
                *dst++ = color & 0xff;
            }
        }
        for (unsigned s = 1; s < scale; s++) {
            std::memcpy(dst, lineStart, (size_t)outW * 3);
            dst += (size_t)outW * 3;
        }
    }

    GUIKIT::Image png;
    unsigned pngSize = 0;
    uint8_t* pngData = png.generatePng(rgb.data(), outW, outH, 3, pngSize);
    if (!pngData || !pngSize) {
        error = "PNG encoding failed";
        return false;
    }
    pngBase64 = Base64::encode(pngData, pngSize);
    delete[] pngData;
    width = outW;
    height = outH;
    return true;
}

}
