#include "video.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>

#include "../mcp/json.h"
#include "../video/slang.h"

using namespace OverlayUi;

namespace {

const int RowHeight = 12;
const Color Error = Color::hex(0xff7777);
const Color Folder = Color::hex(0xb8c76f);

// collection download (overlay "Download" entry): outlives the browser page, never freed (the
// thread may still run at exit)
struct Download {
    std::mutex mutex;
    bool running = false;
    bool finished = false; // since the last poll
    std::string message;
};
Download& download = *new Download;

auto isDir(const std::string& path) -> bool {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

auto fileName(const std::string& path) -> std::string {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

auto lower(std::string s) -> std::string {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// path below root? (root itself counts)
auto inside(const std::string& path, const std::string& root) -> bool {
    return !root.empty() && (path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/'));
}

// one line describing the load state, also used to detect changes
auto stateKey(const SlangPresets::State& s) -> std::string {
    return s.path + "|" + s.status + "|" + (s.error ? "e" : "") + (s.loading ? "l" + s.requested : "");
}

}

// ------------------------------------------------------------ preset row

auto PresetRow::value() const -> std::string {
    auto s = SlangPresets::state();
    if (s.loading)
        return "loading " + fileName(s.requested);
    return s.path.empty() ? "None" : fileName(s.path);
}

auto PresetRow::draw(Painter& p, bool focused) -> void {
    auto s = SlangPresets::state();
    Rect row{rect.x, rect.y, rect.w, 12};
    if (focused)
        p.rect(row.x, row.y, row.w, row.h, Theme::Selected);
    p.textClip(rect.x + 4, rect.y + 2, label, LabelWidth - 8, focused ? Theme::TextBright : Theme::Text);
    std::string name = s.path.empty() ? "None" : fileName(s.path);
    if (!SlangPresets::supported())
        name = "Vulkan renderer only";
    p.textClip(rect.x + LabelWidth, rect.y + 2, name, rect.w - LabelWidth - 4,
        focused ? Theme::TextBright : (s.path.empty() ? Theme::TextDim : Theme::TextBright), OverlayFont::Mono);

    std::string status;
    Color col = Theme::Accent;
    if (s.loading) {
        status = "loading " + fileName(s.requested) + " ...";
        col = Theme::Warn;
    } else if (s.error) {
        status = "error: " + s.status + " - plain picture shown";
        col = Error;
    } else
        status = s.status;
    if (!status.empty())
        p.textClip(rect.x + LabelWidth, rect.y + 14, status, rect.w - LabelWidth - 4, col, OverlayFont::Mono);
}

auto PresetRow::nav(Nav n) -> bool {
    if (!SlangPresets::supported())
        return false;
    if (n == Nav::Ok || n == Nav::Right) {
        if (openBrowser)
            openBrowser();
        return true;
    }
    if (n == Nav::Delete) {
        SlangPresets::load("");
        return true;
    }
    return false;
}

auto PresetRow::hints() const -> std::vector<Hint> {
    if (!SlangPresets::supported())
        return {};
    return {{"RETURN", "browse"}, {"DEL", "no preset"}};
}

auto PresetRow::describe(Json& out) const -> void {
    auto s = SlangPresets::state();
    out.set("path", s.path);
    out.set("status", s.status);
    out.set("error", s.error);
    out.set("loading", s.loading);
    out.set("supported", SlangPresets::supported());
}

auto PresetRow::poll() -> bool {
    auto key = stateKey(SlangPresets::state());
    if (key == lastSeen)
        return false;
    lastSeen = key;
    return true;
}

auto PresetRow::settle() -> void {
    SlangPresets::wait();
}

// ------------------------------------------------------------ browser

ShaderBrowser::ShaderBrowser() {
    expands = true;
}

ShaderBrowser::~ShaderBrowser() = default;

auto ShaderBrowser::root() const -> std::string {
    switch (source) {
        case Source::System: return SlangPresets::systemDir();
        case Source::Downloaded: return SlangPresets::downloadedDir();
        case Source::Folder: return "/";
    }
    return "";
}

auto ShaderBrowser::refresh() -> void {
    auto active = SlangPresets::state().path;
    auto sys = SlangPresets::systemDir(), dl = SlangPresets::downloadedDir();
    filter.clear();
    if (inside(active, sys))
        source = Source::System;
    else if (inside(active, dl))
        source = Source::Downloaded;
    else if (!active.empty())
        source = Source::Folder;
    else
        source = !sys.empty() ? Source::System : (!dl.empty() ? Source::Downloaded : Source::System);
    auto r = root();
    dir = r;
    if (!active.empty() && inside(active, r == "/" ? "" : r))
        dir = active.substr(0, active.rfind('/'));
    else if (source == Source::Folder && !active.empty())
        dir = active.substr(0, active.rfind('/'));
    if (dir.empty() && source == Source::Folder)
        dir = "/";
    load();
    // select the active preset
    auto name = fileName(active);
    for (int i = 0; i < (int)entries.size(); i++)
        if (entries[i].kind == Entry::Kind::Preset && entries[i].name == name && dir + "/" + name == active)
            selected = i;
}

auto ShaderBrowser::setSource(Source s) -> void {
    source = s;
    filter.clear();
    dir = root();
    if (s == Source::Folder) {
        const char* home = std::getenv("HOME");
        dir = home && isDir(home) ? home : "/";
    }
    if (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    load();
}

auto ShaderBrowser::load() -> void {
    entries.clear();
    entries.push_back({"(no preset)", Entry::Kind::None});
    auto r = root();
    if (r.empty()) {
        if (source == Source::Downloaded) {
            std::lock_guard<std::mutex> lock(download.mutex);
            if (!download.running)
                entries.push_back({"Download " + SlangPresets::downloadInfo(), Entry::Kind::Download});
        }
        selected = std::clamp(selected, 0, (int)entries.size() - 1);
        return;
    }
    if (dir != r && dir != "/")
        entries.push_back({"..", Entry::Kind::Parent});
    std::vector<Entry> dirs, presets;
    auto f = lower(filter);
    if (DIR* d = ::opendir(dir.c_str())) {
        while (auto e = ::readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == ".." || name[0] == '.')
                continue;
            if (!f.empty() && lower(name).find(f) == std::string::npos)
                continue;
            std::string full = (dir == "/" ? "" : dir) + "/" + name;
            bool folder = e->d_type == DT_DIR || ((e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) && isDir(full));
            if (folder)
                dirs.push_back({name, Entry::Kind::Dir});
            else if (name.size() > 7 && lower(name.substr(name.size() - 7)) == ".slangp")
                presets.push_back({name, Entry::Kind::Preset});
        }
        ::closedir(d);
    }
    auto byName = [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(presets.begin(), presets.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), presets.begin(), presets.end());
    selected = std::clamp(selected, 0, (int)entries.size() - 1);
}

auto ShaderBrowser::parent() -> bool {
    auto r = root();
    if (r.empty() || dir == r || dir == "/")
        return false;
    auto child = fileName(dir);
    dir = dir.substr(0, dir.rfind('/'));
    if (dir.empty())
        dir = "/";
    filter.clear();
    load();
    for (int i = 0; i < (int)entries.size(); i++)
        if (entries[i].kind == Entry::Kind::Dir && entries[i].name == child)
            selected = i;
    return true;
}

auto ShaderBrowser::activate() -> void {
    if (selected < 0 || selected >= (int)entries.size())
        return;
    auto& e = entries[selected];
    switch (e.kind) {
        case Entry::Kind::None:
            SlangPresets::load("");
            break;
        case Entry::Kind::Parent:
            parent();
            break;
        case Entry::Kind::Dir:
            dir = (dir == "/" ? "" : dir) + "/" + e.name;
            filter.clear();
            selected = 0;
            top = 0;
            load();
            // first preset or folder below "(no preset)" / ".."
            for (int i = 0; i < (int)entries.size(); i++)
                if (entries[i].kind == Entry::Kind::Dir || entries[i].kind == Entry::Kind::Preset) {
                    selected = i;
                    break;
                }
            break;
        case Entry::Kind::Preset:
            SlangPresets::load((dir == "/" ? "" : dir) + "/" + e.name);
            break;
        case Entry::Kind::Download:
            startDownload();
            break;
    }
}

auto ShaderBrowser::startDownload() -> void {
    {
        std::lock_guard<std::mutex> lock(download.mutex);
        if (download.running)
            return;
        download.running = true;
        download.message = "downloading ...";
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.kind == Entry::Kind::Download; }), entries.end());
    selected = std::clamp(selected, 0, (int)entries.size() - 1);
    std::thread([]() {
        std::string error;
        bool ok = SlangPresets::download([](const std::string& line) {
            std::lock_guard<std::mutex> lock(download.mutex);
            download.message = line;
        }, error);
        std::lock_guard<std::mutex> lock(download.mutex);
        download.running = false;
        download.finished = true;
        download.message = ok ? "" : "download failed: " + error;
    }).detach();
}

auto ShaderBrowser::location() const -> std::string {
    auto r = root();
    if (r.empty())
        return source == Source::System ? "not installed (Arch package libretro-shaders-slang)"
                                        : "not downloaded (" + SlangPresets::downloadTarget() + ")";
    if (source == Source::Folder)
        return dir;
    std::string rel = dir.size() > r.size() ? dir.substr(r.size() + 1) + "/" : "";
    return (source == Source::System ? "shaders_slang/" : "slang-shaders/") + rel;
}

auto ShaderBrowser::statusLine() const -> std::string {
    {
        std::lock_guard<std::mutex> lock(download.mutex);
        if (download.running || !download.message.empty())
            return download.message;
    }
    auto s = SlangPresets::state();
    if (s.loading)
        return "loading " + fileName(s.requested) + " ...";
    if (s.error)
        return "error: " + s.status;
    if (s.path.empty())
        return "no preset active";
    return fileName(s.path) + " active, " + s.status;
}

auto ShaderBrowser::layoutParts() const -> void {
    tabBar = {rect.x, rect.y, rect.w, 12};
    pathRect = {rect.x, rect.y + 15, rect.w, 11};
    int listY = rect.y + 27;
    int listH = std::max(RowHeight, (rect.y + rect.h - 14 - listY) / RowHeight * RowHeight);
    listRect = {rect.x, listY, rect.w, listH};
    statusRect = {rect.x, rect.y + rect.h - 11, rect.w, 11};
    tabRects.clear();
    if (!font)
        return;
    int x = tabBar.x;
    for (auto t : {"System", "Downloaded", "Folder"}) {
        int w = font->textWidth(OverlayFont::Pro, t) + 16;
        tabRects.push_back({x, tabBar.y, w, 11});
        x += w + 2;
    }
}

auto ShaderBrowser::visibleRows() const -> int {
    layoutParts();
    return std::max(1, listRect.h / RowHeight);
}

auto ShaderBrowser::draw(Painter& p, bool focused) -> void {
    layoutParts();
    const char* titles[] = {"System", "Downloaded", "Folder"};
    for (int i = 0; i < 3 && i < (int)tabRects.size(); i++) {
        auto& r = tabRects[i];
        bool sel = i == (int)source;
        if (sel) p.rect(r.x, r.y, r.w, r.h, Theme::Selected);
        p.text(r.x + 8, r.y + 2, titles[i], sel ? Theme::TextBright : Theme::TextDim);
    }
    p.rect(tabBar.x, tabBar.y + 11, tabBar.w, 1, Theme::Selected);

    std::string flt = filter.empty() ? "" : "filter: " + filter + "_";
    int fw = flt.empty() ? 0 : p.font.textWidth(OverlayFont::Pro, flt);
    if (!flt.empty()) p.textRight(pathRect.x + pathRect.w, pathRect.y + 1, flt, Theme::Warn);
    p.textClip(pathRect.x, pathRect.y + 1, location(), pathRect.w - fw - 8, root().empty() ? Theme::TextDim : Theme::Path);

    auto active = SlangPresets::state().path;
    int vis = visibleRows();
    int count = (int)entries.size();
    if (selected < top) top = selected;
    if (selected >= top + vis) top = selected - vis + 1;
    top = std::clamp(top, 0, std::max(0, count - vis));
    bool scrollbar = count > vis;
    int inner = listRect.w - (scrollbar ? 6 : 0);
    for (int r = 0; r < vis && top + r < count; r++) {
        auto& e = entries[top + r];
        int y = listRect.y + r * RowHeight;
        bool s = top + r == selected;
        bool hot = s && focused;
        if (s) p.rect(listRect.x, y, inner, RowHeight, hot ? Theme::Selected : Theme::SelectedIdle);
        bool folder = e.kind == Entry::Kind::Dir || e.kind == Entry::Kind::Parent;
        bool isActive = e.kind == Entry::Kind::Preset && (dir == "/" ? "" : dir) + "/" + e.name == active;
        Color col = hot ? Theme::TextBright : (folder ? Folder : (e.kind == Entry::Kind::Preset ? Theme::Text : Theme::Accent));
        std::string text = e.name + (e.kind == Entry::Kind::Dir ? "/" : "");
        int typeW = isActive ? 56 : 0;
        p.textClip(listRect.x + 2, y + 1, text, inner - typeW - 6, col, OverlayFont::Mono);
        if (isActive)
            p.textRight(listRect.x + inner - 2, y + 1, "active", hot ? Theme::TextBright : Theme::Accent);
    }
    if (scrollbar) {
        int trackH = vis * RowHeight;
        p.rect(listRect.x + listRect.w - 4, listRect.y, 3, trackH, Theme::Track);
        int thumbH = std::max(6, trackH * vis / count);
        int thumbY = listRect.y + (trackH - thumbH) * top / std::max(1, count - vis);
        p.rect(listRect.x + listRect.w - 4, thumbY, 3, thumbH, Theme::Text);
    }

    auto s = SlangPresets::state();
    auto line = statusLine();
    Color col = s.error && line.rfind("error", 0) == 0 ? Error : (s.loading ? Theme::Warn : Theme::Accent);
    if (line.rfind("download failed", 0) == 0) col = Error;
    p.rect(statusRect.x, statusRect.y - 2, statusRect.w, 1, Theme::Sidebar);
    p.textClip(statusRect.x + 2, statusRect.y + 1, line, statusRect.w - 4, col, OverlayFont::Mono);
}

auto ShaderBrowser::nav(Nav n) -> bool {
    int count = (int)entries.size();
    int vis = visibleRows();
    switch (n) {
        case Nav::Up: selected = std::max(0, selected - 1); return true;
        case Nav::Down: selected = std::min(count - 1, selected + 1); return true;
        case Nav::PageUp: selected = std::max(0, selected - vis); return true;
        case Nav::PageDown: selected = std::min(count - 1, selected + vis); return true;
        case Nav::Home: selected = 0; return true;
        case Nav::End: selected = count - 1; return true;
        case Nav::Ok: case Nav::Insert: activate(); return true;
        case Nav::Right:
            if (selected < count && entries[selected].kind == Entry::Kind::Dir)
                activate();
            return true;
        case Nav::Left:
            parent();
            return true;
        case Nav::NextTab: setSource((Source)(((int)source + 1) % 3)); return true;
        case Nav::PrevTab: setSource((Source)(((int)source + 2) % 3)); return true;
        case Nav::Delete: SlangPresets::load(""); return true;
        case Nav::Backspace:
            if (!filter.empty()) {
                filter.pop_back();
                load();
            } else
                parent();
            return true;
        case Nav::Parent:
            if (parent())
                return true;
            [[fallthrough]];
        case Nav::Back:
            if (!filter.empty()) {
                filter.clear();
                load();
            } else if (closeBrowser)
                closeBrowser();
            return true;
        default:
            return false;
    }
}

auto ShaderBrowser::text(const std::string& s) -> bool {
    if (s.empty() || root().empty())
        return false;
    filter += s;
    load();
    // first match below "(no preset)" / ".."
    selected = 0;
    for (int i = 0; i < (int)entries.size(); i++)
        if (entries[i].kind == Entry::Kind::Dir || entries[i].kind == Entry::Kind::Preset) {
            selected = i;
            break;
        }
    return true;
}

auto ShaderBrowser::click(int x, int y, int clicks) -> bool {
    layoutParts();
    for (int i = 0; i < (int)tabRects.size(); i++)
        if (tabRects[i].contains(x, y)) {
            if (clicks > 0)
                setSource((Source)i);
            return true;
        }
    if (listRect.contains(x, y)) {
        int row = top + (y - listRect.y) / RowHeight;
        if (row < (int)entries.size()) {
            selected = row;
            if (clicks >= 2)
                activate();
        }
        return true;
    }
    return clicks > 0;
}

auto ShaderBrowser::wheelAt(int x, int y, int dy) -> bool {
    int count = (int)entries.size();
    top = std::clamp(top - dy * 3, 0, std::max(0, count - visibleRows()));
    selected = std::clamp(selected, top, std::min(count - 1, top + visibleRows() - 1));
    return true;
}

auto ShaderBrowser::value() const -> std::string {
    return selected >= 0 && selected < (int)entries.size() ? entries[selected].name : "";
}

auto ShaderBrowser::hints() const -> std::vector<Hint> {
    std::vector<Hint> h;
    if (selected >= 0 && selected < (int)entries.size()) {
        switch (entries[selected].kind) {
            case Entry::Kind::Dir: case Entry::Kind::Parent: h.push_back({"RETURN", "open"}); break;
            case Entry::Kind::Download: h.push_back({"RETURN", "download"}); break;
            default: h.push_back({"RETURN", "load"}); break;
        }
    }
    h.push_back({"TAB", "source"});
    if (!root().empty())
        h.push_back({"type", "filter"});
    h.push_back({"BACKSPACE", "parent"});
    h.push_back({"DEL", "no preset"});
    h.push_back({"ESC", filter.empty() ? "back" : "clear filter"});
    return h;
}

auto ShaderBrowser::describe(Json& out) const -> void {
    const char* names[] = {"system", "downloaded", "folder"};
    out.set("source", std::string(names[(int)source]));
    out.set("root", root());
    out.set("dir", dir);
    out.set("location", location());
    out.set("filter", filter);
    Json list = Json::array();
    const char* kinds[] = {"none", "parent", "dir", "preset", "download"};
    for (auto& e : entries) {
        Json j = Json::object();
        j.set("name", e.name);
        j.set("kind", std::string(kinds[(int)e.kind]));
        list.push(j);
    }
    out.set("entries", list);
    out.set("selected", (int64_t)selected);
    out.set("status", statusLine());
    std::lock_guard<std::mutex> lock(download.mutex);
    out.set("downloading", download.running);
}

auto ShaderBrowser::poll() -> bool {
    std::string key = stateKey(SlangPresets::state());
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(download.mutex);
        key += "|" + download.message + (download.running ? "d" : "");
        finished = download.finished;
        download.finished = false;
    }
    if (finished) {
        if (source == Source::Downloaded) {
            dir = root();
            load();
        }
    }
    if (key == seenState && !finished)
        return false;
    seenState = key;
    return true;
}

auto ShaderBrowser::settle() -> void {
    SlangPresets::wait();
}
