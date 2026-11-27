#include "font.h"

#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace {

const char* FaceFiles[2] = { "C64_Pro-STYLE121.ttf", "C64_Pro_Mono-STYLE121.ttf" };

// ASCII, Latin-1 and a few symbols used by the UI (arrows, blocks)
auto charset() -> std::vector<uint32_t> {
    std::vector<uint32_t> out;
    for (uint32_t c = 32; c < 127; c++) out.push_back(c);
    for (uint32_t c = 160; c < 256; c++) out.push_back(c);
    for (uint32_t c : {0x2190u, 0x2191u, 0x2192u, 0x2193u, 0x2022u, 0x2026u, 0x2500u, 0x2502u, 0x2588u, 0x2591u, 0x2592u, 0x25cfu, 0x25cbu, 0x2713u})
        out.push_back(c);
    return out;
}

const int Cell = 10; // 8 px glyph + padding against bleeding

}

auto OverlayFont::decode(const std::string& s, size_t& pos) -> uint32_t {
    uint8_t c = (uint8_t)s[pos++];
    if (c < 0x80) return c;
    int extra = (c >= 0xf0) ? 3 : (c >= 0xe0) ? 2 : (c >= 0xc0) ? 1 : 0;
    uint32_t cp = c & (0x3f >> extra);
    if (!extra) return '?';
    while (extra-- && pos < s.size() && ((uint8_t)s[pos] & 0xc0) == 0x80)
        cp = (cp << 6) | ((uint8_t)s[pos++] & 0x3f);
    return cp;
}

auto OverlayFont::load(const std::string& folder, std::string& error) -> bool {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        error = "freetype init failed";
        return false;
    }

    auto codes = charset();
    unsigned perRow = 512 / Cell;
    // cell 0 is the white block
    unsigned cells = 1 + 2 * (unsigned)codes.size();
    atlasWidth = 512;
    atlasHeight = 16;
    while (atlasHeight < ((cells + perRow - 1) / perRow) * Cell) atlasHeight <<= 1;
    auto data = std::make_shared<std::vector<uint8_t>>(atlasWidth * atlasHeight, 0);

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            (*data)[(1 + y) * atlasWidth + 1 + x] = 255;
    whiteU = 3.0f / atlasWidth;
    whiteV = 3.0f / atlasHeight;

    unsigned cell = 1;
    bool ok = true;
    for (int f = 0; f < 2 && ok; f++) {
        FT_Face face;
        std::string path = folder + FaceFiles[f];
        if (FT_New_Face(ft, path.c_str(), 0, &face)) {
            error = "cannot load font " + path;
            ok = false;
            break;
        }
        FT_Set_Pixel_Sizes(face, 0, Size);

        for (auto cp : codes) {
            unsigned index = FT_Get_Char_Index(face, cp);
            if (!index && cp != ' ')
                continue;
            if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO))
                continue;
            auto slot = face->glyph;
            Glyph g;
            g.advance = (int)(slot->advance.x >> 6);
            if (cp != ' ') {
                g.left = slot->bitmap_left;
                g.top = slot->bitmap_top;
                g.width = std::min<int>(slot->bitmap.width, Cell - 1);
                g.height = std::min<int>(slot->bitmap.rows, Cell - 1);
            }
            unsigned cx = (cell % perRow) * Cell, cy = (cell / perRow) * Cell;
            cell++;
            for (int y = 0; y < g.height; y++) {
                const uint8_t* row = slot->bitmap.buffer + y * slot->bitmap.pitch;
                for (int x = 0; x < g.width; x++)
                    if (row[x >> 3] & (0x80 >> (x & 7)))
                        (*data)[(cy + y) * atlasWidth + cx + x] = 255;
            }
            g.u0 = (float)cx / atlasWidth;
            g.v0 = (float)cy / atlasHeight;
            g.u1 = (float)(cx + g.width) / atlasWidth;
            g.v1 = (float)(cy + g.height) / atlasHeight;
            glyphs[f][cp] = g;
        }
        FT_Done_Face(face);

        // C64 Pro only has the PETSCII arrows (left, up): mirror them for right and down
        auto mirror = [&](uint32_t from, uint32_t to, bool horizontal) {
            auto src = glyphs[f].find(from);
            if (src == glyphs[f].end() || glyphs[f].count(to))
                return;
            Glyph g = src->second;
            if (horizontal) std::swap(g.u0, g.u1);
            else std::swap(g.v0, g.v1);
            glyphs[f][to] = g;
        };
        mirror(0x2190, 0x2192, true);
        mirror(0x2191, 0x2193, false);

        auto q = glyphs[f].find('?');
        if (q != glyphs[f].end())
            fallback[f] = q->second;
    }
    FT_Done_FreeType(ft);

    if (!ok)
        return false;
    atlas = data;
    return true;
}

auto OverlayFont::glyph(Face face, uint32_t codepoint) const -> const Glyph& {
    auto it = glyphs[face].find(codepoint);
    return it == glyphs[face].end() ? fallback[face] : it->second;
}

auto OverlayFont::textWidth(Face face, const std::string& s) const -> int {
    int w = 0;
    for (size_t pos = 0; pos < s.size();)
        w += glyph(face, decode(s, pos)).advance;
    return w;
}

auto OverlayFont::fit(Face face, const std::string& s, int maxWidth) const -> size_t {
    int w = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = pos;
        int a = glyph(face, decode(s, next)).advance;
        if (w + a > maxWidth)
            break;
        w += a;
        pos = next;
    }
    return pos;
}
