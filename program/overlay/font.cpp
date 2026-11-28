#include "font.h"

#include "glyphs.h"

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

// rasterized by crates/glyphs (ttf-parser + pixel-exact rasterizer)
auto OverlayFont::load(const std::string& folder, std::string& error) -> bool {
    BglAtlas a = {};
    char err[512] = {0};
    if (bgl_atlas_build(folder.c_str(), &a, err, sizeof err) != 0) {
        error = err;
        return false;
    }
    atlasWidth = a.width;
    atlasHeight = a.height;
    auto data = std::make_shared<std::vector<uint8_t>>(a.pixels, a.pixels + (size_t)a.width * a.height);
    whiteU = (a.white_x + 2.0f) / atlasWidth;
    whiteV = (a.white_y + 2.0f) / atlasHeight;

    for (int f = 0; f < 2; f++)
        glyphs[f].clear();
    for (uint32_t i = 0; i < a.glyph_count; i++) {
        auto& src = a.glyphs[i];
        Glyph g;
        g.advance = src.advance;
        g.left = src.left;
        g.top = src.top;
        g.width = src.width;
        g.height = src.height;
        g.u0 = (float)src.x / atlasWidth;
        g.v0 = (float)src.y / atlasHeight;
        g.u1 = (float)(src.x + src.width) / atlasWidth;
        g.v1 = (float)(src.y + src.height) / atlasHeight;
        glyphs[src.face & 1][src.codepoint] = g;
    }
    bgl_atlas_free(&a);

    for (int f = 0; f < 2; f++) {
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
