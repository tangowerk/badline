// Drag & drop tiles: one tile per connected drive, split into INSERT | START.
// SDL names the dragged files only on release, so the tiles cannot depend on the file type:
// disks go to the drive of the tile, tape/cartridge/PRG to their port from any tile.

#include "overlay.h"

#include <algorithm>

#include "../program.h"
#include "../thread/emuThread.h"
#include "../media/autoloader.h"
#include "../view/message.h"
#include "../mcp/json.h"
#include "../mcp/server.h"

using namespace OverlayUi;

namespace {

const int TileMaxWidth = 184;
const int TileGap = 12;
const int RowGap = 14;
const int HeaderHeight = 12;
const int HalfHeight = 64;
const int HalfGap = 6;

// 8x8 icons, drawn at 2x
const uint8_t DiskIcon[8] = { 0xff, 0x81, 0x99, 0xa5, 0xa5, 0x99, 0x89, 0xff };
const uint8_t PlayIcon[8] = { 0xc0, 0xf0, 0xfc, 0xff, 0xff, 0xfc, 0xf0, 0xc0 };

auto icon(Painter& p, int x, int y, const uint8_t* bits, Color c) -> void {
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (bits[row] & (0x80 >> col))
                p.rect(x + col * 2, y + row * 2, 2, 2, c);
}

auto drawHalf(Painter& p, const Rect& r, bool hot, const uint8_t* bits, const std::string& label, const std::string& sub) -> void {
    p.rect(r.x, r.y, r.w, r.h, hot ? Theme::TextBright : Theme::Selected);
    p.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, hot ? Theme::Selected : Color::hex(0x352879, 0.92f));
    icon(p, r.x + (r.w - 16) / 2, r.y + 8, bits, hot ? Theme::TextBright : Theme::Text);
    int lw = p.font.textWidth(OverlayFont::Pro, label);
    p.text(r.x + (r.w - lw) / 2, r.y + 32, label, hot ? Theme::TextBright : Theme::Text);
    int sw = p.font.textWidth(OverlayFont::Pro, sub);
    if (sw <= r.w - 4)
        p.text(r.x + (r.w - sw) / 2, r.y + 46, sub, hot ? Color::hex(0xe0dcff) : Theme::TextDim);
    else
        p.textClip(r.x + 2, r.y + 46, sub, r.w - 4, hot ? Color::hex(0xe0dcff) : Theme::TextDim);
}

}

auto Overlay::dropTiles() const -> std::vector<DropTile> {
    std::vector<DropTile> out;
    auto emu = activeEmulator;
    auto disks = emu ? emu->getDiskMediaGroup() : nullptr;
    unsigned drives = 1;
    if (disks) {
        drives = emu->getModelValue(emu->getModelIdOfEnabledDrives(disks));
        drives = std::clamp<unsigned>(drives, 1, std::min<unsigned>(4, disks->media.size()));
    }

    int cols = drives <= 2 ? (int)drives : 2;
    int rows = ((int)drives + cols - 1) / cols;
    int tileW = std::min(TileMaxWidth, (logicalWidth - 32 - (cols - 1) * TileGap) / cols);
    int tileH = HeaderHeight + HalfHeight;
    int gridW = cols * tileW + (cols - 1) * TileGap;
    int gridH = rows * tileH + (rows - 1) * RowGap;
    int x0 = (logicalWidth - gridW) / 2;
    int y0 = std::max(8, (logicalHeight - gridH) / 2 - 12);
    int halfW = (tileW - HalfGap) / 2;

    for (unsigned d = 0; d < drives; d++) {
        DropTile t;
        t.drive = d;
        if (disks) {
            auto media = emu->getDisk(d);
            if (media && media->guid)
                t.image = program->getFileNameFromMedia(media);
        }
        int x = x0 + (int)(d % cols) * (tileW + TileGap);
        int y = y0 + (int)(d / cols) * (tileH + RowGap);
        t.tile = { x, y, tileW, tileH };
        t.insert = { x, y + HeaderHeight, halfW, HalfHeight };
        t.start = { x + tileW - halfW, y + HeaderHeight, halfW, HalfHeight };
        out.push_back(t);
    }
    return out;
}

auto Overlay::dragBegin() -> void {
    dragActive = true;
    dragHover = -1;
    dirty = true;
}

auto Overlay::dragMove(int x, int y) -> void {
    if (!dragActive)
        return;
    int hover = -1;
    auto tiles = dropTiles();
    for (int i = 0; i < (int)tiles.size(); i++) {
        if (tiles[i].insert.contains(x, y)) hover = i * 2;
        else if (tiles[i].start.contains(x, y)) hover = i * 2 + 1;
    }
    if (hover != dragHover) {
        dragHover = hover;
        dirty = true;
    }
}

auto Overlay::dragEnd(const std::vector<std::string>& files) -> void {
    if (!dragActive)
        return;
    int hover = dragHover;
    auto tiles = dropTiles();
    dragActive = false;
    dragHover = -1;
    dirty = true;
    if (files.empty())
        return;

    auto say = [this](const std::string& s) {
        if (opened) notify(s);
        else if (appMessage) appMessage->warning(s);
    };
    if (!activeEmulator) {
        say("No emulator running");
        return;
    }
    bool stopped = mcpServer ? mcpServer->cpuStopped() : (emuThread && emuThread->debugging);
    if (stopped) {
        say("Not possible while the debugger has stopped the CPU");
        return;
    }

    // outside the tiles: Denise's drop (autostart_dragndrop decides about disks and tapes)
    Autoloader::Mode mode = Autoloader::Mode::DragnDrop;
    unsigned drive = 0;
    if (hover >= 0 && hover / 2 < (int)tiles.size()) {
        drive = tiles[hover / 2].drive;
        mode = (hover & 1) ? Autoloader::Mode::AutoStartWithSlot : Autoloader::Mode::OpenWithSlot;
    }

    bool locked = emuThread->lock();
    autoloader->init(files, mode, drive);
    autoloader->setEmulator(activeEmulator);
    autoloader->loadFiles();
    bool loaded = autoloader->hasLoaded();
    bool started = loaded && autoloader->hasStarted();
    if (locked)
        emuThread->unlock();

    if (!opened || !loaded)
        return;
    if (started) {
        close();
        return;
    }
    if (currentPage().refresh)
        currentPage().refresh(currentPage());
    std::string name = files[0].substr(files[0].find_last_of('/') + 1);
    say("Inserted " + name);
}

auto Overlay::dropHint() const -> std::string {
    if (dragHover < 0) {
        auto settings = activeEmulator ? Program::getSettings(activeEmulator) : nullptr;
        bool autostart = settings ? settings->get<bool>("autostart_dragndrop", true) : true;
        return autostart ? "Drop outside the tiles to autostart" : "Drop outside the tiles to insert";
    }
    std::string drive = std::to_string(8 + dragHover / 2);
    if (dragHover & 1)
        return "Disk → drive " + drive + ": reset + run · Tape/cartridge/PRG: start";
    return "Disk → drive " + drive + ": insert · Tape: insert · Cartridge/PRG: start";
}

auto Overlay::drawDrop(Painter& p) -> void {
    // also over an open menu: the tiles must stand out from its text
    p.rawRect(0, 0, pixelWidth, pixelHeight, Theme::Dim);

    auto tiles = dropTiles();
    int bottom = 0;
    for (int i = 0; i < (int)tiles.size(); i++) {
        auto& t = tiles[i];
        std::string head = "DRIVE " + std::to_string(8 + t.drive);
        int hw = p.text(t.tile.x, t.tile.y, head, Theme::TextBright);
        p.textClip(t.tile.x + hw + 8, t.tile.y, t.image.empty() ? "(empty)" : t.image,
            t.tile.w - hw - 8, Theme::TextDim, t.image.empty() ? OverlayFont::Pro : OverlayFont::Mono);
        drawHalf(p, t.insert, dragHover == i * 2, DiskIcon, "INSERT", "no reset");
        drawHalf(p, t.start, dragHover == i * 2 + 1, PlayIcon, "START", "reset + run");
        bottom = std::max(bottom, t.tile.y + t.tile.h);
    }

    auto hint = dropHint();
    int w = p.font.textWidth(OverlayFont::Pro, hint);
    if (w <= logicalWidth - 16)
        p.text((logicalWidth - w) / 2, bottom + 20, hint, Theme::Text);
    else
        p.textClip(8, bottom + 20, hint, logicalWidth - 16, Theme::Text);
}

auto Overlay::describeDrop(Json& out) const -> void {
    out = Json::object();
    out.set("active", dragActive);
    if (!dragActive)
        return;
    auto rect = [](const Rect& r) {
        return Json(std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h));
    };
    Json tiles = Json::array();
    for (auto& t : dropTiles()) {
        Json e = Json::object();
        e.set("drive", (int64_t)(8 + t.drive));
        e.set("image", t.image);
        e.set("insert", rect(t.insert));
        e.set("start", rect(t.start));
        tiles.push(e);
    }
    out.set("tiles", tiles);
    if (dragHover >= 0) {
        Json h = Json::object();
        h.set("drive", (int64_t)(8 + dragHover / 2));
        h.set("action", (dragHover & 1) ? "start" : "insert");
        out.set("hover", h);
    } else
        out.set("hover", Json());
    out.set("hint", dropHint());
}
