#pragma once

// Overlay widget toolkit: a painter that turns rectangles and text into quads
// (logical pixel grid, integer scaled) and simple retained widgets.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "font.h"
#include "../../driver/driver.h"

struct Json;

namespace OverlayUi {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static auto hex(uint32_t rgb, float a = 1.0f) -> Color {
        return { ((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a };
    }
};

// mockup C, based on the Pepto palette
namespace Theme {
    const Color Dim = Color::hex(0x000000, 0.55f);
    const Color Panel = Color::hex(0x14103c, 0.94f);
    const Color Sidebar = Color::hex(0x352879);       // blue
    const Color Selected = Color::hex(0x6c5eb5);      // light blue
    const Color SelectedIdle = Color::hex(0x352879);
    const Color Text = Color::hex(0xb8b0ff);
    const Color TextBright = Color::hex(0xffffff);
    const Color TextDim = Color::hex(0x8a80d0);
    const Color Path = Color::hex(0x7f7fff);
    const Color Accent = Color::hex(0x9ae29b);        // light green
    const Color Warn = Color::hex(0xb8c76f);          // yellow
    const Color KeyBg = Color::hex(0xb8b0ff);
    const Color KeyText = Color::hex(0x000000);
    const Color Track = Color::hex(0x352879);
}

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    auto contains(int px, int py) const -> bool { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Painter {
    Painter(const OverlayFont& font, DRIVER::OverlayFrame& frame, int scale, int offsetX, int offsetY, float alpha)
        : font(font), frame(frame), scale(scale), offsetX(offsetX), offsetY(offsetY), alpha(alpha) {}

    auto rect(int x, int y, int w, int h, Color c) -> void;
    // window pixels, unscaled (dim layer over the whole window)
    auto rawRect(int x, int y, int w, int h, Color c) -> void;
    // returns the advance; size 1 = 8 px, 2 = 16 px
    auto text(int x, int y, const std::string& s, Color c, OverlayFont::Face face = OverlayFont::Pro, int size = 1) -> int;
    // clipped to maxWidth with "..."
    auto textClip(int x, int y, const std::string& s, int maxWidth, Color c, OverlayFont::Face face = OverlayFont::Pro) -> void;
    auto textRight(int right, int y, const std::string& s, Color c, OverlayFont::Face face = OverlayFont::Pro) -> void;

    const OverlayFont& font;

private:
    auto quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) -> void;
    DRIVER::OverlayFrame& frame;
    int scale, offsetX, offsetY;
    float alpha;
};

enum class Nav { Up, Down, Left, Right, Ok, Back, PageUp, PageDown, Home, End, Backspace, Delete,
    // media browser: Tab / Shift+Tab (pad LB/RB), Shift+Return (pad X), Ctrl+B (pad Y), Ctrl+H, Ctrl+E,
    // parent folder (pad B; falls back to Back when not consumed)
    NextTab, PrevTab, Insert, Bookmark, AllFiles, Eject, Parent };

struct Hint { std::string key, action; };

struct Widget {
    virtual ~Widget() = default;

    std::string label;
    Rect rect; // logical, set by the page layout
    bool expands = false; // takes the remaining height of the page

    virtual auto kind() const -> std::string = 0;
    virtual auto focusable() const -> bool { return true; }
    virtual auto preferredHeight() const -> int { return 12; }
    virtual auto draw(Painter& p, bool focused) -> void = 0;
    // keyboard/pad navigation; true = consumed
    virtual auto nav(Nav n) -> bool { return false; }
    virtual auto text(const std::string& s) -> bool { return false; }
    // logical coordinates; clicks = 1 single, 2 double
    virtual auto click(int x, int y, int clicks) -> bool { return nav(Nav::Ok); }
    virtual auto wheel(int dy) -> bool { return false; }
    virtual auto wheelAt(int x, int y, int dy) -> bool { return wheel(dy); }
    virtual auto rightClick(int x, int y) -> bool { return false; }
    virtual auto editing() const -> bool { return false; }
    // receives typed text while focused without being in an edit mode (live filter)
    virtual auto acceptsText() const -> bool { return false; }
    // hints() is the complete key hint bar (no generic move/back entries)
    virtual auto ownsHints() const -> bool { return false; }
    virtual auto describe(Json& out) const -> void {}
    // UI thread, every overlay tick while the page is shown: true = draw again (async results)
    virtual auto poll() -> bool { return false; }
    // MCP state: wait (bounded) for pending async results before describing
    virtual auto settle() -> void {}
    virtual auto value() const -> std::string { return ""; }
    virtual auto hints() const -> std::vector<Hint> { return {}; }

    std::function<void()> onChange;
protected:
    auto changed() -> void { if (onChange) onChange(); }
    // focused row background + label column
    auto drawRow(Painter& p, bool focused) -> void;
    static const int LabelWidth = 168;
};

struct Label : Widget {
    enum class Style { Normal, Title, Subtitle, Dim, Warn };
    Label(std::string text, Style style = Style::Normal) : style(style) { label = std::move(text); }
    Style style;
    auto kind() const -> std::string override { return "label"; }
    auto focusable() const -> bool override { return false; }
    auto preferredHeight() const -> int override;
    auto draw(Painter& p, bool focused) -> void override;
    auto value() const -> std::string override { return label; }
};

struct Button : Widget {
    Button(std::string text, std::function<void()> action) : action(std::move(action)) { label = std::move(text); }
    std::function<void()> action;
    auto kind() const -> std::string override { return "button"; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto hints() const -> std::vector<Hint> override { return {{"RETURN", "select"}}; }
};

struct Toggle : Widget {
    Toggle(std::string text, bool state) : state(state) { label = std::move(text); }
    bool state;
    auto kind() const -> std::string override { return "toggle"; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto value() const -> std::string override { return state ? "on" : "off"; }
    auto hints() const -> std::vector<Hint> override { return {{"RETURN", "toggle"}}; }
};

struct Slider : Widget {
    Slider(std::string text, int min, int max, int step, int value, std::string unit = "")
        : min(min), max(max), step(step), current(value), unit(std::move(unit)) { label = std::move(text); }
    int min, max, step, current;
    std::string unit;
    auto kind() const -> std::string override { return "slider"; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto click(int x, int y, int clicks) -> bool override;
    auto wheel(int dy) -> bool override;
    auto value() const -> std::string override { return std::to_string(current) + unit; }
    auto hints() const -> std::vector<Hint> override { return {{"\u2190\u2192", "change"}}; }
    auto set(int v) -> void;
private:
    auto track() const -> Rect;
};

struct Choice : Widget {
    Choice(std::string text, std::vector<std::string> options, unsigned index = 0)
        : options(std::move(options)), index(index) { label = std::move(text); }
    std::vector<std::string> options;
    unsigned index;
    auto kind() const -> std::string override { return "choice"; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto click(int x, int y, int clicks) -> bool override;
    auto wheel(int dy) -> bool override;
    auto value() const -> std::string override { return index < options.size() ? options[index] : ""; }
    auto hints() const -> std::vector<Hint> override { return {{"\u2190\u2192", "change"}}; }
};

struct TextInput : Widget {
    TextInput(std::string text, std::string content, size_t maxLength = 64)
        : content(std::move(content)), maxLength(maxLength) { label = std::move(text); }
    std::string content;
    size_t maxLength;
    bool active = false;
    auto kind() const -> std::string override { return "text"; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto text(const std::string& s) -> bool override;
    auto editing() const -> bool override { return active; }
    auto value() const -> std::string override { return content; }
    auto hints() const -> std::vector<Hint> override;
private:
    std::string before; // restored by Esc
};

struct ListView : Widget {
    struct Column { std::string title; int width; bool right = false; bool accent = false; };
    ListView(std::vector<Column> columns) : columns(std::move(columns)) { expands = true; }
    std::vector<Column> columns; // width 0 = remaining space
    std::vector<std::vector<std::string>> rows;
    int selected = 0;
    int top = 0;
    std::function<void(int row)> onActivate;

    auto kind() const -> std::string override { return "list"; }
    auto preferredHeight() const -> int override { return 12 + 12 * 4; }
    auto draw(Painter& p, bool focused) -> void override;
    auto nav(Nav n) -> bool override;
    auto click(int x, int y, int clicks) -> bool override;
    auto wheel(int dy) -> bool override;
    auto value() const -> std::string override;
    auto hints() const -> std::vector<Hint> override { return {{"RETURN", "select"}}; }
    auto visibleRows() const -> int;
    auto setRows(std::vector<std::vector<std::string>> r) -> void;
private:
    auto ensureVisible() -> void;
    static const int RowHeight = 12;
};

// modal yes/no style question
struct Dialog {
    std::string title, message;
    std::vector<std::string> buttons;
    int selected = 0;
    std::function<void(int button)> onResult; // -1 = cancelled
    Rect rect;
    auto buttonRect(int i) const -> Rect;
    auto layout(int screenWidth, int screenHeight, const OverlayFont& font) -> void;
    auto draw(Painter& p) -> void;
};

}
