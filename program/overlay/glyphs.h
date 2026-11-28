#pragma once

// C API of crates/glyphs (overlay glyph atlas, M-VK2); kept in sync with crates/glyphs/src/lib.rs by hand.

#include <cstddef>
#include <cstdint>

extern "C" {

struct BglGlyph {
    uint32_t face;
    uint32_t codepoint;
    int32_t advance;
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
    uint32_t x; // top left texel in the atlas
    uint32_t y;
};

struct BglAtlas {
    uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    BglGlyph* glyphs;
    uint32_t glyph_count;
    uint32_t white_x; // fully set 4x4 block for solid rectangles
    uint32_t white_y;
};

// folder ends with '/'; 0 = ok, otherwise a message in error
int bgl_atlas_build(const char* folder, BglAtlas* out, char* error, size_t error_len);
void bgl_atlas_free(BglAtlas* atlas);

}
