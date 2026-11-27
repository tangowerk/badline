#pragma once

// Glyph atlas for the overlay: C64 Pro (proportional) and C64 Pro Mono, rasterized once
// at their native 8 px grid (1-bit, pixel exact) and scaled by integer factors on screen.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct OverlayFont {
    enum Face { Pro = 0, Mono = 1 };
    static const int Size = 8; // logical pixels per text line (cell height)
    static const int Ascent = 7;

    struct Glyph {
        int advance = 0;
        int left = 0, top = 0, width = 0, height = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };

    auto load(const std::string& folder, std::string& error) -> bool;
    auto loaded() const -> bool { return atlas != nullptr; }

    auto glyph(Face face, uint32_t codepoint) const -> const Glyph&;
    auto textWidth(Face face, const std::string& utf8) const -> int;
    // longest prefix (in bytes) that fits into maxWidth
    auto fit(Face face, const std::string& utf8, int maxWidth) const -> size_t;

    static auto decode(const std::string& utf8, size_t& pos) -> uint32_t;

    std::shared_ptr<std::vector<uint8_t>> atlas;
    unsigned atlasWidth = 0;
    unsigned atlasHeight = 0;
    float whiteU = 0, whiteV = 0; // a fully set texel for solid rectangles

private:
    std::unordered_map<uint32_t, Glyph> glyphs[2];
    Glyph fallback[2];
};
