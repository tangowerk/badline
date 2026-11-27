#include "ui.h"

#include <algorithm>

namespace OverlayUi {

// ------------------------------------------------------------ painter

auto Painter::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) -> void {
    float a = c.a * alpha;
    const float v[6][4] = {
        {x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
        {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1},
    };
    auto& out = frame.vertices;
    for (auto& p : v) {
        out.insert(out.end(), {p[0], p[1], p[2], p[3], c.r, c.g, c.b, a});
    }
}

auto Painter::rawRect(int x, int y, int w, int h, Color c) -> void {
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), font.whiteU, font.whiteV, font.whiteU, font.whiteV, c);
}

auto Painter::rect(int x, int y, int w, int h, Color c) -> void {
    if (w <= 0 || h <= 0)
        return;
    rawRect(offsetX + x * scale, offsetY + y * scale, w * scale, h * scale, c);
}

auto Painter::text(int x, int y, const std::string& s, Color c, OverlayFont::Face face, int size) -> int {
    int pen = x;
    int baseline = y + OverlayFont::Ascent * size;
    int unit = scale * size;
    for (size_t pos = 0; pos < s.size();) {
        auto& g = font.glyph(face, OverlayFont::decode(s, pos));
        if (g.width && g.height) {
            float gx = (float)(offsetX + pen * scale + g.left * unit);
            float gy = (float)(offsetY + baseline * scale - g.top * unit);
            quad(gx, gy, gx + g.width * unit, gy + g.height * unit, g.u0, g.v0, g.u1, g.v1, c);
        }
        pen += g.advance * size;
    }
    return pen - x;
}

auto Painter::textClip(int x, int y, const std::string& s, int maxWidth, Color c, OverlayFont::Face face) -> void {
    if (font.textWidth(face, s) <= maxWidth) {
        text(x, y, s, c, face);
        return;
    }
    const std::string dots = "...";
    int room = maxWidth - font.textWidth(face, dots);
    if (room <= 0)
        return;
    size_t n = font.fit(face, s, room);
    int w = text(x, y, s.substr(0, n), c, face);
    text(x + w, y, dots, c, face);
}

auto Painter::textRight(int right, int y, const std::string& s, Color c, OverlayFont::Face face) -> void {
    text(right - font.textWidth(face, s), y, s, c, face);
}

// ------------------------------------------------------------ widgets

auto Widget::drawRow(Painter& p, bool focused) -> void {
    if (focused)
        p.rect(rect.x, rect.y, rect.w, rect.h, Theme::Selected);
    p.textClip(rect.x + 4, rect.y + 2, label, LabelWidth - 8, focused ? Theme::TextBright : Theme::Text);
}

auto Label::preferredHeight() const -> int {
    switch (style) {
        case Style::Title: return 20;
        case Style::Subtitle: return 12;
        default: return 10;
    }
}

auto Label::draw(Painter& p, bool) -> void {
    switch (style) {
        case Style::Title: p.text(rect.x, rect.y, label, Theme::TextBright, OverlayFont::Pro, 2); break;
        case Style::Subtitle: p.textClip(rect.x, rect.y, label, rect.w, Theme::Path); break;
        case Style::Dim: p.textClip(rect.x, rect.y, label, rect.w, Theme::TextDim); break;
        case Style::Warn: p.textClip(rect.x, rect.y, label, rect.w, Theme::Warn); break;
        default: p.textClip(rect.x, rect.y, label, rect.w, Theme::Text); break;
    }
}

auto Button::draw(Painter& p, bool focused) -> void {
    int w = p.font.textWidth(OverlayFont::Pro, label) + 16;
    p.rect(rect.x, rect.y, w, rect.h, focused ? Theme::Selected : Theme::Sidebar);
    p.text(rect.x + 8, rect.y + 2, label, focused ? Theme::TextBright : Theme::Text);
}

auto Button::nav(Nav n) -> bool {
    if (n != Nav::Ok)
        return false;
    if (action)
        action();
    return true;
}

auto Toggle::draw(Painter& p, bool focused) -> void {
    drawRow(p, focused);
    int x = rect.x + LabelWidth;
    p.rect(x, rect.y + 2, 8, 8, focused ? Theme::TextBright : Theme::Text);
    p.rect(x + 1, rect.y + 3, 6, 6, focused ? Theme::Selected : Theme::Panel);
    if (state)
        p.rect(x + 2, rect.y + 4, 4, 4, Theme::Accent);
    p.text(x + 14, rect.y + 2, state ? "On" : "Off", state ? Theme::Accent : (focused ? Theme::TextBright : Theme::TextDim));
}

auto Toggle::nav(Nav n) -> bool {
    if (n == Nav::Ok || (n == Nav::Left && state) || (n == Nav::Right && !state)) {
        state = !state;
        changed();
        return true;
    }
    // Left/Right on the edge are not consumed (focus may leave the content)
    return n == Nav::Right;
}

auto Slider::track() const -> Rect {
    int x = rect.x + LabelWidth;
    int w = std::max(32, std::min(160, rect.w - LabelWidth - 56));
    return { x, rect.y + 4, w, 4 };
}

auto Slider::set(int v) -> void {
    v = std::clamp(v, min, max);
    if (step > 1)
        v = min + ((v - min + step / 2) / step) * step;
    if (v != current) {
        current = std::clamp(v, min, max);
        changed();
    }
}

auto Slider::draw(Painter& p, bool focused) -> void {
    drawRow(p, focused);
    auto t = track();
    p.rect(t.x, t.y, t.w, t.h, focused ? Theme::Sidebar : Theme::Track);
    int fill = max > min ? (t.w * (current - min)) / (max - min) : 0;
    p.rect(t.x, t.y, fill, t.h, focused ? Theme::TextBright : Theme::Text);
    p.rect(t.x + fill - 1, t.y - 2, 3, t.h + 4, focused ? Theme::TextBright : Theme::Text);
    p.text(t.x + t.w + 8, rect.y + 2, value(), focused ? Theme::TextBright : Theme::Text, OverlayFont::Mono);
}

auto Slider::nav(Nav n) -> bool {
    int big = std::max(step, (max - min) / 10);
    switch (n) {
        case Nav::Left: set(current - step); return true;
        case Nav::Right: set(current + step); return true;
        case Nav::PageUp: set(current + big); return true;
        case Nav::PageDown: set(current - big); return true;
        case Nav::Home: set(min); return true;
        case Nav::End: set(max); return true;
        default: return false;
    }
}

auto Slider::click(int x, int, int) -> bool {
    auto t = track();
    if (x < t.x - 4 || x > t.x + t.w + 4)
        return true;
    set(min + ((x - t.x) * (max - min) + t.w / 2) / std::max(1, t.w));
    return true;
}

auto Slider::wheel(int dy) -> bool {
    set(current + (dy > 0 ? step : -step));
    return true;
}

auto Choice::draw(Painter& p, bool focused) -> void {
    drawRow(p, focused);
    int x = rect.x + LabelWidth;
    auto col = focused ? Theme::TextBright : Theme::Text;
    auto arrows = focused ? Theme::TextBright : Theme::TextDim;
    p.text(x, rect.y + 2, "←", index > 0 ? arrows : Theme::Sidebar);
    int w = p.text(x + 12, rect.y + 2, value(), col);
    p.text(x + 12 + w + 4, rect.y + 2, "→", index + 1 < options.size() ? arrows : Theme::Sidebar);
}

auto Choice::nav(Nav n) -> bool {
    if (options.empty())
        return false;
    if (n == Nav::Left || n == Nav::Right || n == Nav::Ok) {
        unsigned before = index;
        if (n == Nav::Left && index > 0) index--;
        else if (n == Nav::Right && index + 1 < options.size()) index++;
        else if (n == Nav::Ok) index = (index + 1) % options.size();
        if (before != index)
            changed();
        // the leftmost option lets Left leave the content
        return !(n == Nav::Left && before == 0);
    }
    return false;
}

auto Choice::click(int x, int, int) -> bool {
    int mid = rect.x + LabelWidth + 6;
    return nav(x <= mid ? Nav::Left : (x < rect.x + LabelWidth ? Nav::Ok : Nav::Right)) || true;
}

auto Choice::wheel(int dy) -> bool {
    nav(dy > 0 ? Nav::Right : Nav::Left);
    return true;
}

auto TextInput::draw(Painter& p, bool focused) -> void {
    drawRow(p, focused);
    int x = rect.x + LabelWidth;
    int w = std::max(48, rect.w - LabelWidth - 4);
    p.rect(x, rect.y + 1, w, rect.h - 2, active ? Theme::Panel : (focused ? Theme::Sidebar : Theme::Track));
    std::string shown = content;
    int room = w - 12;
    // keep the end (cursor) visible
    while (!shown.empty() && p.font.textWidth(OverlayFont::Mono, shown) > room) {
        size_t pos = 0;
        OverlayFont::decode(shown, pos);
        shown.erase(0, pos);
    }
    int tw = p.text(x + 3, rect.y + 2, shown, active || focused ? Theme::TextBright : Theme::Text, OverlayFont::Mono);
    if (active)
        p.rect(x + 3 + tw, rect.y + 2, 6, 8, Theme::Text);
}

auto TextInput::nav(Nav n) -> bool {
    if (!active) {
        if (n == Nav::Ok) {
            active = true;
            before = content;
            return true;
        }
        return false;
    }
    switch (n) {
        case Nav::Ok:
            active = false;
            if (content != before)
                changed();
            return true;
        case Nav::Back:
            active = false;
            content = before;
            return true;
        case Nav::Backspace:
            if (!content.empty()) {
                // drop the last UTF-8 sequence
                size_t i = content.size() - 1;
                while (i > 0 && ((uint8_t)content[i] & 0xc0) == 0x80) i--;
                content.erase(i);
            }
            return true;
        case Nav::Delete:
            content.clear();
            return true;
        default:
            return true; // the field keeps the focus while editing
    }
}

auto TextInput::text(const std::string& s) -> bool {
    if (!active)
        return false;
    for (size_t pos = 0; pos < s.size();) {
        size_t start = pos;
        uint32_t cp = OverlayFont::decode(s, pos);
        if (cp < 32 || content.size() + (pos - start) > maxLength)
            continue;
        content += s.substr(start, pos - start);
    }
    return true;
}

auto TextInput::hints() const -> std::vector<Hint> {
    if (active)
        return {{"RETURN", "apply"}, {"ESC", "cancel"}};
    return {{"RETURN", "edit"}};
}

auto ListView::visibleRows() const -> int {
    return std::max(1, (rect.h - RowHeight) / RowHeight);
}

auto ListView::setRows(std::vector<std::vector<std::string>> r) -> void {
    rows = std::move(r);
    selected = std::clamp(selected, 0, std::max(0, (int)rows.size() - 1));
    ensureVisible();
}

auto ListView::ensureVisible() -> void {
    int vis = visibleRows();
    if (selected < top) top = selected;
    if (selected >= top + vis) top = selected - vis + 1;
    top = std::clamp(top, 0, std::max(0, (int)rows.size() - vis));
}

auto ListView::draw(Painter& p, bool focused) -> void {
    int scrollbar = (int)rows.size() > visibleRows() ? 6 : 0;
    int inner = rect.w - scrollbar;
    // column x positions; width 0 = remaining
    int fixed = 0;
    for (auto& c : columns) fixed += c.width;
    int flexible = std::max(16, inner - fixed);

    auto columnX = [&](size_t i, int& x, int& w) {
        x = rect.x;
        for (size_t k = 0; k < i; k++) x += columns[k].width ? columns[k].width : flexible;
        w = columns[i].width ? columns[i].width : flexible;
    };

    for (size_t i = 0; i < columns.size(); i++) {
        int x, w;
        columnX(i, x, w);
        if (columns[i].right) p.textRight(x + w - 4, rect.y + 2, columns[i].title, Theme::Path);
        else p.textClip(x + 4, rect.y + 2, columns[i].title, w - 8, Theme::Path);
    }

    ensureVisible();
    int vis = visibleRows();
    for (int r = 0; r < vis && top + r < (int)rows.size(); r++) {
        int row = top + r;
        int y = rect.y + RowHeight * (r + 1);
        bool sel = row == selected;
        if (sel)
            p.rect(rect.x, y, inner, RowHeight, focused ? Theme::Selected : Theme::SelectedIdle);
        auto& cells = rows[row];
        for (size_t i = 0; i < columns.size() && i < cells.size(); i++) {
            int x, w;
            columnX(i, x, w);
            auto col = sel && focused ? Theme::TextBright : (columns[i].accent ? Theme::Accent : Theme::Text);
            if (columns[i].right) p.textRight(x + w - 4, y + 2, cells[i], col, OverlayFont::Mono);
            else p.textClip(x + 4, y + 2, cells[i], w - 8, col);
        }
    }

    if (rows.empty())
        p.text(rect.x + 4, rect.y + RowHeight + 2, "(empty)", Theme::TextDim);

    if (scrollbar) {
        int trackY = rect.y + RowHeight, trackH = vis * RowHeight;
        p.rect(rect.x + rect.w - 4, trackY, 3, trackH, Theme::Track);
        int thumbH = std::max(6, trackH * vis / (int)rows.size());
        int thumbY = trackY + (trackH - thumbH) * top / std::max(1, (int)rows.size() - vis);
        p.rect(rect.x + rect.w - 4, thumbY, 3, thumbH, Theme::Text);
    }
}

auto ListView::nav(Nav n) -> bool {
    int last = (int)rows.size() - 1;
    int vis = visibleRows();
    switch (n) {
        case Nav::Up:
            if (selected <= 0) return false; // leave the list upwards
            selected--;
            break;
        case Nav::Down:
            if (selected >= last) return false;
            selected++;
            break;
        case Nav::PageUp: selected = std::max(0, selected - vis); break;
        case Nav::PageDown: selected = std::max(0, std::min(last, selected + vis)); break;
        case Nav::Home: selected = 0; break;
        case Nav::End: selected = std::max(0, last); break;
        case Nav::Ok:
            if (onActivate && selected <= last) onActivate(selected);
            return true;
        default:
            return false;
    }
    ensureVisible();
    changed();
    return true;
}

auto ListView::click(int, int y, int clicks) -> bool {
    int r = (y - rect.y) / RowHeight - 1;
    if (r < 0)
        return true;
    int row = top + r;
    if (row >= (int)rows.size())
        return true;
    if (row != selected) {
        selected = row;
        changed();
    }
    if (clicks >= 2 && onActivate)
        onActivate(row);
    return true;
}

auto ListView::wheel(int dy) -> bool {
    int vis = visibleRows();
    top = std::clamp(top - dy * 3, 0, std::max(0, (int)rows.size() - vis));
    // keep the selection inside the visible part
    selected = std::clamp(selected, top, std::max(top, std::min((int)rows.size() - 1, top + vis - 1)));
    return true;
}

auto ListView::value() const -> std::string {
    if (selected < 0 || selected >= (int)rows.size())
        return "";
    std::string out;
    for (auto& c : rows[selected])
        out += (out.empty() ? "" : " | ") + c;
    return out;
}

// ------------------------------------------------------------ dialog

auto Dialog::layout(int screenWidth, int screenHeight, const OverlayFont& font) -> void {
    int w = std::max(font.textWidth(OverlayFont::Pro, message), font.textWidth(OverlayFont::Pro, title) * 2) + 32;
    w = std::clamp(w, 200, std::max(200, screenWidth - 32));
    int h = 72;
    rect = { (screenWidth - w) / 2, (screenHeight - h) / 2, w, h };
}

auto Dialog::buttonRect(int i) const -> Rect {
    int bw = 64, gap = 8;
    int total = (int)buttons.size() * bw + ((int)buttons.size() - 1) * gap;
    int x = rect.x + (rect.w - total) / 2 + i * (bw + gap);
    return { x, rect.y + rect.h - 22, bw, 14 };
}

auto Dialog::draw(Painter& p) -> void {
    p.rect(rect.x - 2, rect.y - 2, rect.w + 4, rect.h + 4, Theme::Selected);
    p.rect(rect.x, rect.y, rect.w, rect.h, Color::hex(0x14103c));
    p.text(rect.x + 12, rect.y + 8, title, Theme::TextBright, OverlayFont::Pro, 2);
    p.textClip(rect.x + 12, rect.y + 30, message, rect.w - 24, Theme::Text);
    for (int i = 0; i < (int)buttons.size(); i++) {
        auto r = buttonRect(i);
        bool sel = i == selected;
        p.rect(r.x, r.y, r.w, r.h, sel ? Theme::Selected : Theme::Sidebar);
        int tw = p.font.textWidth(OverlayFont::Pro, buttons[i]);
        p.text(r.x + (r.w - tw) / 2, r.y + 3, buttons[i], sel ? Theme::TextBright : Theme::Text);
    }
}

}
