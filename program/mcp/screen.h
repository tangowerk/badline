#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace LIBC64 { struct Interface; }

namespace McpScreen {
    // 25 lines of 40 characters, UTF-8, lines joined with '\n', trailing spaces kept
    auto readText(LIBC64::Interface* emu) -> std::string;
    // screen codes as hex, 25 lines
    auto readRaw(LIBC64::Interface* emu) -> std::string;
    // single screen code to UTF-8 for the given charset (lowercase = true for the lower/upper set)
    auto screenCodeToUtf8(uint8_t code, bool lowercase) -> const char*;
    // is the lowercase/uppercase ROM charset active
    auto lowercaseCharset(LIBC64::Interface* emu) -> bool;

    enum class Border { Visible, None, Full };
    // raw framebuffer with palette as PNG, base64 encoded
    auto screenshot(LIBC64::Interface* emu, const std::vector<uint32_t>& palette, Border border, unsigned scale,
        std::string& pngBase64, unsigned& width, unsigned& height, std::string& error) -> bool;
}
