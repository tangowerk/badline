#pragma once

// Media page of the overlay: tab bar (Autostart, Disk 8, Disk 9, Cartridge, Tape), slot row,
// favorites (bookmarks + recent files), file browser (folders and archives) and a
// directory preview of the selected image. One widget that fills the page.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "../../emulation/interface.h"
#include "../../guikit/api.h"

struct MediaBrowser : OverlayUi::Widget {
    enum class Tab { Autostart, Disk8, Disk9, Cartridge, Tape };
    enum class Zone { Slot, Favorites, Files, Preview };

    MediaBrowser();
    ~MediaBrowser() override;

    // set by the overlay
    std::function<void(const std::string&)> notify;
    std::function<void()> closeOverlay;
    std::function<bool()> canAct;
    const OverlayFont* font = nullptr; // for hit rects without drawing (headless/MCP)

    // page shown: tabs, slot and recent files may have changed
    auto refresh() -> void;

    auto kind() const -> std::string override { return "media"; }
    auto preferredHeight() const -> int override { return 120; }
    auto draw(OverlayUi::Painter& p, bool focused) -> void override;
    auto nav(OverlayUi::Nav n) -> bool override;
    auto text(const std::string& s) -> bool override;
    auto click(int x, int y, int clicks) -> bool override;
    auto rightClick(int x, int y) -> bool override;
    auto wheelAt(int x, int y, int dy) -> bool override;
    auto acceptsText() const -> bool override { return zone != Zone::Slot; }
    auto ownsHints() const -> bool override { return true; }
    auto value() const -> std::string override;
    auto hints() const -> std::vector<OverlayUi::Hint> override;
    auto describe(Json& out) const -> void override;

private:
    struct Entry {
        std::string name, type; // type: DIR, ZIP, D64, ...
        bool parent = false, dir = false, archive = false;
        uint64_t size = 0;
        int item = -1; // archive item id
    };
    struct Favorite {
        std::string title, path;
        int item = -1;
        bool header = false, bookmark = false, missing = false;
    };
    // a loadable file: plain file or an item inside an archive
    struct Target {
        std::string path, name;
        int item = -1;
    };
    struct PreviewLine {
        std::string text;  // UTF-8 with C64 Pro screen code glyphs
        std::string plain; // ASCII for hints and MCP
    };
    struct List {
        int selected = 0, top = 0;
        auto clamp(int count, int visible) -> void;
    };
    struct TabInfo { Tab tab; std::string id, title; };

    auto tabs() const -> std::vector<TabInfo>;
    auto currentTab() const -> Tab;
    auto switchTab(int delta) -> void;
    auto selectTab(int index) -> void;
    auto tabSuffixes(Tab tab) const -> std::vector<std::string>;
    auto groupFor(const std::string& name) const -> Emulator::Interface::MediaGroup*;
    auto tabMedia(Tab tab) const -> Emulator::Interface::Media*;

    auto openFolder(const std::string& path, const std::string& select = "") -> bool;
    auto openArchive(const std::string& path) -> bool;
    auto enterArchiveDir(int item, const std::string& select = "") -> void;
    auto goParent() -> bool;
    auto rescan(const std::string& select = "") -> void;
    auto applyFilter(const std::string& select = "") -> void;
    auto location() const -> std::string;
    auto folderKey(Tab tab) const -> std::string;
    auto rememberFolder() -> void;

    auto loadFavorites() -> void;
    auto saveBookmarks() -> void;
    auto toggleBookmark() -> void;
    auto removeFavorite() -> void;
    auto selectableFavorites() const -> std::vector<int>;

    auto selectedEntry() const -> const Entry*;
    auto targetOf(const Entry& e) const -> Target;
    auto updatePreview() -> void;
    auto activateEntry(bool autostart) -> void;
    auto activateFavorite(bool autostart) -> void;
    auto load(const Target& t, bool autostart, unsigned selection) -> void;
    auto eject() -> void;
    auto toggleWriteProtect() -> void;
    auto slotButtons() const -> std::vector<std::string>;
    auto pressSlotButton(int index) -> void;

    auto moveZone(int dir) -> bool;
    auto listNav(List& list, int count, int visible, OverlayUi::Nav n) -> bool;

    // layout (logical pixels), computed in layoutParts()
    auto layoutParts() const -> void;
    mutable OverlayUi::Rect tabBar, slotRow, favRect, pathRect, fileRect, prevRect;
    mutable std::vector<OverlayUi::Rect> tabRects, buttonRects;
    auto visibleFiles() const -> int;
    auto visibleFavorites() const -> int;
    auto visiblePreview() const -> int;

    int tabIndex = 0;
    Zone zone = Zone::Files;
    int slotButton = 0;
    bool allFiles = false;
    std::string filter;

    std::string folder;   // file system folder (also the folder of an open archive)
    std::string archivePath;
    std::unique_ptr<GUIKIT::File> archive;
    std::vector<GUIKIT::File::Item>* archiveItems = nullptr;
    int archiveDir = -1;  // item id of the current folder inside the archive, -1 = root

    std::vector<Entry> all, entries; // entries = filtered view
    List files, favs, prev;
    std::vector<Favorite> favorites;
    std::vector<PreviewLine> preview;
    std::string previewInfo; // one line for files without a directory
    std::string previewKey;
};
