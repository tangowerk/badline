#pragma once

// Video page of the overlay (M-VK3c, mockups/video.html): the active slang preset with its load
// status, and the preset browser (sources: system collection, downloaded collection, any folder)
// that opens as a sub page over the Video page.

#include <functional>
#include <string>
#include <vector>

#include "ui.h"

// "Preset" row + status line; RETURN opens the browser, DEL removes the preset
struct PresetRow : OverlayUi::Widget {
    PresetRow() { label = "Preset"; }
    std::function<void()> openBrowser;
    auto kind() const -> std::string override { return "preset"; }
    auto preferredHeight() const -> int override { return 24; }
    auto draw(OverlayUi::Painter& p, bool focused) -> void override;
    auto nav(OverlayUi::Nav n) -> bool override;
    auto value() const -> std::string override;
    auto hints() const -> std::vector<OverlayUi::Hint> override;
    auto describe(Json& out) const -> void override;
    auto poll() -> bool override;
    auto settle() -> void override;
private:
    std::string lastSeen;
};

struct ShaderBrowser : OverlayUi::Widget {
    enum class Source { System, Downloaded, Folder };

    ShaderBrowser();
    ~ShaderBrowser() override;

    std::function<void()> closeBrowser; // back to the Video page
    const OverlayFont* font = nullptr;

    // shown: start in the source/folder of the active preset
    auto refresh() -> void;

    auto kind() const -> std::string override { return "shader_browser"; }
    auto preferredHeight() const -> int override { return 120; }
    auto draw(OverlayUi::Painter& p, bool focused) -> void override;
    auto nav(OverlayUi::Nav n) -> bool override;
    auto text(const std::string& s) -> bool override;
    auto click(int x, int y, int clicks) -> bool override;
    auto wheelAt(int x, int y, int dy) -> bool override;
    auto acceptsText() const -> bool override { return true; }
    auto ownsHints() const -> bool override { return true; }
    auto value() const -> std::string override;
    auto hints() const -> std::vector<OverlayUi::Hint> override;
    auto describe(Json& out) const -> void override;
    auto poll() -> bool override;
    auto settle() -> void override;

private:
    struct Entry {
        std::string name;
        enum class Kind { None, Parent, Dir, Preset, Download } kind;
    };

    auto root() const -> std::string;   // "" = source not available
    auto setSource(Source s) -> void;
    auto load() -> void;                 // list `dir` with the filter
    auto activate() -> void;             // RETURN on the selected entry
    auto parent() -> bool;
    auto location() const -> std::string;
    auto statusLine() const -> std::string;
    auto layoutParts() const -> void;
    auto visibleRows() const -> int;
    auto startDownload() -> void;

    Source source = Source::System;
    std::string dir;                     // absolute, ends without '/'
    std::string filter;
    std::vector<Entry> entries;
    int selected = 0, top = 0;
    std::string seenState;               // SlangPresets state shown last (poll)

    mutable OverlayUi::Rect tabBar, pathRect, listRect, statusRect;
    mutable std::vector<OverlayUi::Rect> tabRects;
};
