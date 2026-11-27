#include "media.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "../program.h"
#include "../thread/emuThread.h"
#include "../media/autoloader.h"
#include "../media/fileloader.h"
#include "../helper/miscHelper.h"
#include "../media/recentFiles.h"
#include "../tools/filepool.h"
#include "../tools/filesetting.h"
#include "../tools/shortcuts.h"
#include "../mcp/json.h"

using namespace OverlayUi;
namespace fs = std::filesystem;
using Media = Emulator::Interface::Media;
using MediaGroup = Emulator::Interface::MediaGroup;

namespace {

const int RowHeight = 10;
const int PreviewRow = 9;
const int FavWidth = 120;
const unsigned MaxRecent = 20;
const unsigned MaxBookmarks = 20;
const uint64_t MaxPreviewSize = 16u << 20;
const Color Missing = Color::hex(0x555555);
const Color Folder = Theme::Warn;

auto lower(std::string s) -> std::string {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

auto extension(const std::string& name) -> std::string {
    auto dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
        return "";
    return lower(name.substr(dot + 1));
}

auto upper(std::string s) -> std::string {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

auto isArchiveName(const std::string& name) -> bool {
    auto ext = extension(name);
    auto list = GUIKIT::File::suppportedCompressionExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

auto home() -> std::string {
    const char* h = getenv("HOME");
    return h && *h ? std::string(h) : std::string("/");
}

// "~/c64" instead of "/home/kc/c64"
auto pretty(const std::string& path) -> std::string {
    auto h = home();
    if (h.size() > 1 && path.compare(0, h.size(), h) == 0 && (path.size() == h.size() || path[h.size()] == '/'))
        return "~" + path.substr(h.size());
    return path;
}

auto sizeText(uint64_t bytes) -> std::string {
    char buf[16];
    if (bytes < 1024) snprintf(buf, sizeof buf, "%uB", (unsigned)bytes);
    else if (bytes < (1u << 20)) snprintf(buf, sizeof buf, "%uK", (unsigned)((bytes + 1023) >> 10));
    else snprintf(buf, sizeof buf, "%.1fM", bytes / 1048576.0);
    return buf;
}

auto utf8(uint32_t cp, std::string& out) -> void {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
    else { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
}

// screen code (upper case/graphics set) -> ASCII approximation
auto screenToAscii(uint8_t sc) -> char {
    sc &= 0x7f;
    if (sc == 0) return '@';
    if (sc <= 26) return (char)('A' + sc - 1);
    if (sc == 27) return '[';
    if (sc == 29) return ']';
    if (sc >= 32 && sc < 64) return (char)sc;
    return sc == 0x60 ? ' ' : '-';
}

auto mediaFile(Media* media) -> std::string {
    if (!media || !media->guid)
        return "";
    return program->getFileNameFromMedia(media);
}

}

auto MediaBrowser::List::clamp(int count, int visible) -> void {
    selected = std::clamp(selected, 0, std::max(0, count - 1));
    visible = std::max(1, visible);
    if (selected < top) top = selected;
    if (selected >= top + visible) top = selected - visible + 1;
    top = std::clamp(top, 0, std::max(0, count - visible));
}

MediaBrowser::MediaBrowser() {
    expands = true;
}

MediaBrowser::~MediaBrowser() = default;

// ------------------------------------------------------------ tabs and media

auto MediaBrowser::tabs() const -> std::vector<TabInfo> {
    std::vector<TabInfo> out = {{Tab::Autostart, "autostart", "Autostart"}, {Tab::Disk8, "disk8", "Disk 8"}};
    auto emu = activeEmulator;
    auto disks = emu ? emu->getDiskMediaGroup() : nullptr;
    if (disks && emu->getModelValue(emu->getModelIdOfEnabledDrives(disks)) >= 2)
        out.push_back({Tab::Disk9, "disk9", "Disk 9"});
    out.push_back({Tab::Cartridge, "cartridge", "Cartridge"});
    out.push_back({Tab::Tape, "tape", "Tape"});
    return out;
}

auto MediaBrowser::currentTab() const -> Tab {
    auto t = tabs();
    return t[std::clamp(tabIndex, 0, (int)t.size() - 1)].tab;
}

auto MediaBrowser::selectTab(int index) -> void {
    auto t = tabs();
    index = std::clamp(index, 0, (int)t.size() - 1);
    if (index == tabIndex)
        return;
    tabIndex = index;
    slotButton = 0;
    zone = Zone::Files;
    auto settings = activeEmulator ? Program::getSettings(activeEmulator) : nullptr;
    auto stored = settings ? settings->get<std::string>(folderKey(currentTab()), "") : "";
    openFolder(stored.empty() ? home() : stored);
}

auto MediaBrowser::switchTab(int delta) -> void {
    int count = (int)tabs().size();
    selectTab((tabIndex + delta + count) % count);
}

auto MediaBrowser::tabSuffixes(Tab tab) const -> std::vector<std::string> {
    std::vector<std::string> out;
    auto emu = activeEmulator;
    if (!emu)
        return out;
    auto add = [&out](MediaGroup* g) {
        if (g) for (auto& s : g->suffix)
            if (std::find(out.begin(), out.end(), s) == out.end())
                out.push_back(s);
    };
    switch (tab) {
        case Tab::Autostart:
            for (auto& g : emu->mediaGroups)
                if (!g.isHardDisk()) add(&g);
            break;
        case Tab::Disk8: case Tab::Disk9: add(emu->getDiskMediaGroup()); break;
        case Tab::Tape: add(emu->getTapeMediaGroup()); break;
        case Tab::Cartridge: out = {"crt", "bin"}; break;
    }
    return out;
}

// media group by file name (first match: disk, tape, program, cartridge)
auto MediaBrowser::groupFor(const std::string& name) const -> MediaGroup* {
    auto emu = activeEmulator;
    if (!emu)
        return nullptr;
    auto ext = extension(name);
    for (auto g : {emu->getDiskMediaGroup(), emu->getTapeMediaGroup(), emu->getPRGMediaGroup()})
        if (g && std::find(g->suffix.begin(), g->suffix.end(), ext) != g->suffix.end())
            return g;
    for (auto& g : emu->mediaGroups)
        if (g.isExpansion() && std::find(g.suffix.begin(), g.suffix.end(), ext) != g.suffix.end())
            return &g;
    return nullptr;
}

auto MediaBrowser::tabMedia(Tab tab) const -> Media* {
    auto emu = activeEmulator;
    if (!emu)
        return nullptr;
    switch (tab) {
        case Tab::Disk8: return emu->getDisk(0);
        case Tab::Disk9: return emu->getDisk(1);
        case Tab::Tape: return emu->getTape(0);
        case Tab::Cartridge: {
            auto exp = emu->getExpansion();
            if (exp && !exp->isEmpty() && exp->mediaGroup && !exp->mediaGroup->media.empty())
                return &exp->mediaGroup->media[0];
            return nullptr;
        }
        default: return nullptr;
    }
}

auto MediaBrowser::slotButtons() const -> std::vector<std::string> {
    auto tab = currentTab();
    if (tab == Tab::Autostart)
        return {};
    auto media = tabMedia(tab);
    if (!media || !media->guid)
        return {};
    if (tab == Tab::Cartridge)
        return {"Eject"};
    // the drive applies a changed write protection with a delay: show the setting
    bool wp = FileSetting::getInstance(activeEmulator, _underscore(media->name))->writeProtect;
    return {"Eject", std::string("Write protect: ") + (wp ? "on" : "off")};
}

auto MediaBrowser::pressSlotButton(int index) -> void {
    if (index == 0) eject();
    else if (index == 1) toggleWriteProtect();
}

// ------------------------------------------------------------ folders and archives

auto MediaBrowser::folderKey(Tab tab) const -> std::string {
    for (auto& t : tabs())
        if (t.tab == tab)
            return "overlay_media_folder_" + t.id;
    return "overlay_media_folder";
}

auto MediaBrowser::rememberFolder() -> void {
    if (activeEmulator && !folder.empty())
        Program::getSettings(activeEmulator)->set<std::string>(folderKey(currentTab()), folder);
}

auto MediaBrowser::openFolder(const std::string& path, const std::string& select) -> bool {
    std::error_code ec;
    fs::path p = path.empty() ? fs::path(home()) : fs::path(path);
    p = fs::weakly_canonical(p, ec);
    if (ec) p = fs::path(home());
    // the folder may be gone: use the nearest existing parent
    while (!fs::is_directory(p, ec)) {
        if (!p.has_parent_path() || p.parent_path() == p) {
            p = fs::path(home());
            break;
        }
        p = p.parent_path();
    }
    folder = p.string();
    archive.reset();
    archiveItems = nullptr;
    archivePath.clear();
    archiveDir = -1;
    filter.clear();
    files = {};
    entries.clear();
    if (zone == Zone::Preview) zone = Zone::Files;
    rescan(select);
    rememberFolder();
    return true;
}

auto MediaBrowser::openArchive(const std::string& path) -> bool {
    auto file = std::make_unique<GUIKIT::File>();
    file->setFile(path);
    file->setReadOnly();
    auto& items = file->scanArchive();
    if (items.empty()) {
        if (notify) notify("Cannot open archive " + fs::path(path).filename().string());
        return false;
    }
    archive = std::move(file);
    archiveItems = &items;
    archivePath = path;
    archiveDir = -1;
    filter.clear();
    files = {};
    entries.clear();
    if (zone == Zone::Preview) zone = Zone::Files;
    rescan();
    return true;
}

auto MediaBrowser::enterArchiveDir(int item, const std::string& select) -> void {
    archiveDir = item;
    filter.clear();
    files = {};
    entries.clear();
    if (zone == Zone::Preview) zone = Zone::Files;
    rescan(select);
}

auto MediaBrowser::goParent() -> bool {
    if (archive) {
        if (archiveDir >= 0 && archiveDir < (int)archiveItems->size()) {
            auto& it = (*archiveItems)[archiveDir];
            enterArchiveDir(it.parent ? (int)it.parent->id : -1, it.info.name);
            return true;
        }
        auto name = fs::path(archivePath).filename().string();
        archive.reset();
        archiveItems = nullptr;
        archivePath.clear();
        filter.clear();
        files = {};
    entries.clear();
    if (zone == Zone::Preview) zone = Zone::Files;
        rescan(name);
        return true;
    }
    fs::path p(folder);
    if (!p.has_parent_path() || p.parent_path() == p)
        return false;
    openFolder(p.parent_path().string(), p.filename().string());
    return true;
}

auto MediaBrowser::rescan(const std::string& select) -> void {
    std::string keep = select;
    if (keep.empty())
        if (auto e = selectedEntry()) keep = e->name;

    all.clear();
    auto suffixes = tabSuffixes(currentTab());
    auto matches = [&](const std::string& name) {
        return allFiles || std::find(suffixes.begin(), suffixes.end(), extension(name)) != suffixes.end();
    };

    if (archive || folder != "/") {
        Entry up;
        up.name = "..";
        up.type = "DIR";
        up.parent = up.dir = true;
        all.push_back(up);
    }

    if (archive) {
        for (auto& it : *archiveItems) {
            int parent = it.parent ? (int)it.parent->id : -1;
            if (parent != archiveDir)
                continue;
            Entry e;
            e.name = it.info.name;
            e.item = (int)it.id;
            e.size = it.info.size;
            if (it.isDirectory) {
                e.dir = true;
                e.type = "DIR";
            } else {
                if (e.name.empty() || !matches(e.name))
                    continue;
                e.type = upper(extension(e.name));
            }
            all.push_back(e);
        }
    } else {
        std::error_code ec;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            auto name = it->path().filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            std::error_code ec2;
            Entry e;
            e.name = name;
            if (it->is_directory(ec2)) {
                e.dir = true;
                e.type = "DIR";
            } else if (it->is_regular_file(ec2)) {
                e.size = it->file_size(ec2);
                e.type = upper(extension(name));
                if (isArchiveName(name))
                    e.archive = true;
                else if (!matches(name))
                    continue;
            } else
                continue;
            all.push_back(e);
        }
    }

    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        if (a.parent != b.parent) return a.parent;
        bool fa = a.dir || a.archive, fb = b.dir || b.archive;
        if (fa != fb) return fa;
        return lower(a.name) < lower(b.name);
    });
    applyFilter(keep);
}

auto MediaBrowser::applyFilter(const std::string& select) -> void {
    std::string keep = select;
    if (keep.empty())
        if (auto e = selectedEntry()) keep = e->name;
    entries.clear();
    auto f = lower(filter);
    for (auto& e : all)
        if (e.parent || f.empty() || lower(e.name).find(f) != std::string::npos)
            entries.push_back(e);

    files.selected = 0;
    // with a filter the first match is more useful than ".."
    if (!f.empty() && entries.size() > 1 && entries[0].parent)
        files.selected = 1;
    for (int i = 0; i < (int)entries.size(); i++)
        if (!keep.empty() && entries[i].name == keep && (f.empty() || !entries[i].parent)) {
            files.selected = i;
            break;
        }
    files.clamp((int)entries.size(), visibleFiles());
    updatePreview();
}

auto MediaBrowser::location() const -> std::string {
    if (!archive)
        return pretty(folder == "/" ? folder : folder + "/");
    std::string inner;
    if (archiveDir >= 0 && archiveDir < (int)archiveItems->size())
        inner = archive->getFileName(&(*archiveItems)[archiveDir]) + "/";
    return pretty(archivePath) + "/" + inner;
}

// ------------------------------------------------------------ favorites

auto MediaBrowser::loadFavorites() -> void {
    favorites.clear();
    favorites.push_back({"BOOKMARKS", "", -1, true});
    std::error_code ec;
    for (unsigned i = 0; i < MaxBookmarks; i++) {
        auto path = globalSettings->get<std::string>("overlay_bookmark_" + std::to_string(i), "");
        if (path.empty())
            continue;
        Favorite f;
        f.title = pretty(path);
        f.path = path;
        f.bookmark = true;
        f.missing = !fs::is_directory(path, ec);
        favorites.push_back(f);
    }
    favorites.push_back({"RECENT", "", -1, true});
    if (activeEmulator && fileloader) {
        auto recent = fileloader->getRecentFile(activeEmulator);
        if (recent) {
            unsigned n = 0;
            for (auto& r : recent->list(nullptr)) {
                if (r.path.empty())
                    continue;
                Favorite f;
                f.path = GUIKIT::File::resolveRelativePath(r.path);
                f.title = r.file.empty() ? fs::path(f.path).filename().string() : r.file;
                f.item = (int)r.id;
                f.missing = !fs::exists(f.path, ec);
                favorites.push_back(f);
                if (++n >= MaxRecent)
                    break;
            }
        }
    }
    favs.clamp((int)selectableFavorites().size(), 1 << 20);
}

auto MediaBrowser::saveBookmarks() -> void {
    unsigned i = 0;
    for (auto& f : favorites)
        if (f.bookmark && i < MaxBookmarks)
            globalSettings->set<std::string>("overlay_bookmark_" + std::to_string(i++), f.path);
    for (; i < MaxBookmarks; i++)
        globalSettings->remove("overlay_bookmark_" + std::to_string(i));
}

auto MediaBrowser::selectableFavorites() const -> std::vector<int> {
    std::vector<int> out;
    for (int i = 0; i < (int)favorites.size(); i++)
        if (!favorites[i].header)
            out.push_back(i);
    return out;
}

auto MediaBrowser::toggleBookmark() -> void {
    for (auto it = favorites.begin(); it != favorites.end(); ++it)
        if (it->bookmark && it->path == folder) {
            favorites.erase(it);
            saveBookmarks();
            loadFavorites();
            if (notify) notify("Bookmark removed: " + pretty(folder));
            return;
        }
    int count = 0;
    for (auto& f : favorites) count += f.bookmark;
    if (count >= (int)MaxBookmarks) {
        if (notify) notify("At most 20 bookmarks");
        return;
    }
    // append after the last bookmark
    int pos = 1;
    for (int i = 0; i < (int)favorites.size(); i++)
        if (favorites[i].bookmark) pos = i + 1;
    Favorite f;
    f.path = folder;
    f.bookmark = true;
    favorites.insert(favorites.begin() + pos, f);
    saveBookmarks();
    loadFavorites();
    if (notify) notify("Bookmarked " + pretty(folder));
}

auto MediaBrowser::removeFavorite() -> void {
    auto sel = selectableFavorites();
    if (favs.selected < 0 || favs.selected >= (int)sel.size())
        return;
    auto& f = favorites[sel[favs.selected]];
    if (f.bookmark) {
        favorites.erase(favorites.begin() + sel[favs.selected]);
        saveBookmarks();
        loadFavorites();
        if (notify) notify("Bookmark removed");
    } else if (notify)
        notify("Recent files cannot be removed");
}

auto MediaBrowser::activateFavorite(bool autostart) -> void {
    auto sel = selectableFavorites();
    if (favs.selected < 0 || favs.selected >= (int)sel.size())
        return;
    auto f = favorites[sel[favs.selected]];
    if (f.missing) {
        if (notify) notify(std::string(f.bookmark ? "Folder" : "File") + " not found: " + pretty(f.path));
        return;
    }
    if (f.bookmark) {
        openFolder(f.path);
        zone = Zone::Files;
        return;
    }
    load({f.path, f.title, f.item}, autostart, 0);
}

// ------------------------------------------------------------ preview

auto MediaBrowser::selectedEntry() const -> const Entry* {
    if (files.selected < 0 || files.selected >= (int)entries.size())
        return nullptr;
    return &entries[files.selected];
}

auto MediaBrowser::targetOf(const Entry& e) const -> Target {
    if (archive)
        return {archivePath, e.name, e.item};
    return {(fs::path(folder) / e.name).string(), e.name, -1};
}

auto MediaBrowser::updatePreview() -> void {
    auto e = selectedEntry();
    std::string key;
    if (e && !e->dir && !e->archive)
        key = targetOf(*e).path + "#" + std::to_string(e->item);
    if (key == previewKey)
        return;
    previewKey = key;
    preview.clear();
    previewInfo.clear();
    prev = {};
    if (!e || e->parent)
        return;
    if (e->dir) {
        previewInfo = "Folder";
        return;
    }
    if (e->archive) {
        previewInfo = e->type + " archive, " + sizeText(e->size) + " - RETURN opens it";
        return;
    }
    auto emu = activeEmulator;
    if (!emu)
        return;
    previewInfo = e->type + ", " + sizeText(e->size);
    if (e->size > MaxPreviewSize)
        return;

    auto group = groupFor(e->name);
    if (!group)
        return;

    auto build = [&](uint8_t* data, unsigned size) {
        std::vector<Emulator::Interface::Listing> listing;
        if (!data || !size)
            return listing;
        if (group->isDisk()) listing = emu->getDiskPreview(data, size, nullptr);
        else if (group->isTape()) listing = emu->getTapePreview(data, size, nullptr);
        else if (group->isProgram()) listing = emu->getProgramPreview(data, size);
        else if (group->isExpansion()) listing = emu->getExpansionPreview(data, size);
        return listing;
    };

    std::vector<Emulator::Interface::Listing> listing;
    if (archive && e->item >= 0) {
        listing = build(archive->archiveData(e->item), (unsigned)archive->archiveDataSize(e->item));
    } else {
        GUIKIT::File file;
        file.setFile(targetOf(*e).path);
        file.setReadOnly();
        auto& items = file.scanArchive();
        if (items.size() == 1)
            listing = build(file.archiveData(0), (unsigned)file.archiveDataSize(0));
    }

    for (auto& l : listing) {
        PreviewLine line;
        for (auto code : l.line) {
            utf8(0xee00 + (code & 0xff), line.text);
            line.plain += screenToAscii((uint8_t)code);
        }
        while (!line.plain.empty() && line.plain.back() == ' ') line.plain.pop_back();
        preview.push_back(line);
    }
}

// ------------------------------------------------------------ loading

auto MediaBrowser::activateEntry(bool autostart) -> void {
    auto e = selectedEntry();
    if (!e)
        return;
    if (e->parent) {
        goParent();
        return;
    }
    if (e->dir) {
        if (archive) enterArchiveDir(e->item);
        else openFolder((fs::path(folder) / e->name).string());
        return;
    }
    if (e->archive && !archive) {
        openArchive((fs::path(folder) / e->name).string());
        return;
    }
    load(targetOf(*e), autostart, 0);
}

auto MediaBrowser::load(const Target& t, bool autostart, unsigned selection) -> void {
    if (canAct && !canAct())
        return;
    auto emu = activeEmulator;
    auto tab = currentTab();
    auto group = groupFor(t.name);
    auto say = [this](const std::string& s) { if (notify) notify(s); };

    if (!group) {
        say("Unknown file type: " + t.name);
        return;
    }

    if (tab == Tab::Autostart) {
        // cartridges and programs cannot be inserted without starting them
        bool start = autostart || !group->isDrive();
        bool locked = emuThread->lock();
        autoloader->init({t.path}, start ? Autoloader::Mode::AutoStart : Autoloader::Mode::Open, selection);
        autoloader->setErrorLevel(1);
        autoloader->setEmulator(emu);
        if (t.item >= 0)
            autoloader->setArchiveId(t.item);
        autoloader->loadFiles();
        bool ok = autoloader->hasLoaded();
        if (locked)
            emuThread->unlock();
        if (!ok) {
            say("Cannot load " + t.name);
            return;
        }
        rememberFolder();
        loadFavorites();
        if (start) {
            if (closeOverlay) closeOverlay();
        } else
            say("Inserted " + t.name);
        return;
    }

    bool wanted = (tab == Tab::Disk8 || tab == Tab::Disk9) ? group->isDisk()
        : tab == Tab::Tape ? group->isTape() : group->isExpansion();
    if (!wanted) {
        say(std::string("Not a ") + (tab == Tab::Tape ? "tape image" : tab == Tab::Cartridge ? "cartridge" : "disk image") + ": " + t.name);
        return;
    }

    GUIKIT::File* file = filePool->get(t.path);
    if (!file || !file->exists() || !file->getSize()) {
        say("Cannot open " + t.path);
        return;
    }
    auto& items = file->scanArchive();
    GUIKIT::File::Item* item = nullptr;
    if (t.item >= 0 && t.item < (int)items.size())
        item = &items[t.item];
    else
        item = Fileloader::findSuitableItem(items, group);
    if (!item || !item->info.size) {
        say("Cannot open " + t.name);
        filePool->unloadOrphaned();
        return;
    }

    Media* media = nullptr;
    if (tab == Tab::Cartridge) {
        auto exp = emu->analyzeExpansion(file->archiveData(item->id), (unsigned)file->archiveDataSize(item->id), extension(item->info.name));
        if (!exp || !exp->mediaGroup || exp->mediaGroup->media.empty()) {
            say("Unsupported cartridge: " + t.name);
            filePool->unloadOrphaned();
            return;
        }
        media = &exp->mediaGroup->media[0];
        autostart = true; // a cartridge needs a power cycle
    } else
        media = tabMedia(tab);
    if (!media) {
        say("No slot for " + t.name);
        return;
    }
    if (tab == Tab::Disk9)
        autostart = false;

    bool locked = emuThread->lock();
    fileloader->insertImage(emu, media, file, item);
    if (media->group->isDrive()) {
        autoloader->setOnlyForFirstDrive(emu, media);
        Program::getSettings(emu)->set<int>("swap_pos", -1, false);
    }
    if (autostart) {
        auto settings = Program::getSettings(emu);
        bool trapped = media->group->isDisk() ? settings->get<bool>("use_disk_traps", false)
            : media->group->isTape() ? settings->get<bool>("use_tape_traps", false) : false;
        fileloader->autoload(emu, media, selection, trapped);
    }
    if (locked)
        emuThread->unlock();

    rememberFolder();
    loadFavorites();
    if (autostart) {
        if (closeOverlay) closeOverlay();
    } else
        say("Inserted " + t.name + " into " + (tab == Tab::Tape ? std::string("the tape drive") : "drive " + std::to_string(tab == Tab::Disk9 ? 9 : 8)));
}

auto MediaBrowser::eject() -> void {
    auto tab = currentTab();
    auto emu = activeEmulator;
    auto say = [this](const std::string& s) { if (notify) notify(s); };
    if (tab == Tab::Autostart) {
        say("Choose a slot tab to eject");
        return;
    }
    auto media = tabMedia(tab);
    if (!media || !media->guid) {
        say("Nothing inserted");
        return;
    }
    if (canAct && !canAct())
        return;
    bool locked = emuThread->lock();
    if (tab == Tab::Cartridge) {
        for (auto& m : media->group->media)
            if (m.guid)
                fileloader->eject(emu, &m);
        // unplug the cartridge too: an empty EasyFlash etc. would stay attached and crash the boot
        auto expansion = emu->getExpansion();
        if (expansion && expansion->mediaGroup == media->group)
            MiscHelper::removeExpansion(emu);
        program->power(emu);
    } else
        fileloader->eject(emu, media);
    if (locked)
        emuThread->unlock();
    slotButton = 0;
    if (zone == Zone::Slot && slotButtons().empty())
        zone = Zone::Files;
    say(tab == Tab::Cartridge ? "Cartridge ejected (hard reset)" : "Ejected");
}

auto MediaBrowser::toggleWriteProtect() -> void {
    auto emu = activeEmulator;
    auto media = tabMedia(currentTab());
    if (!emu || !media || !media->guid || currentTab() == Tab::Cartridge)
        return;
    auto fSetting = FileSetting::getInstance(emu, _underscore(media->name));
    bool state = !fSetting->writeProtect;
    bool locked = emuThread->lock();
    emu->writeProtect(media, state);
    if (locked)
        emuThread->unlock();
    // shared between image, savestates and the disk swapper (as in Denise)
    fSetting->setWriteProtect(state);
    if (notify) notify(state ? "Write protected" : "Writable");
}

auto MediaBrowser::refresh() -> void {
    if (folder.empty()) {
        auto settings = activeEmulator ? Program::getSettings(activeEmulator) : nullptr;
        auto stored = settings ? settings->get<std::string>(folderKey(currentTab()), "") : "";
        openFolder(stored.empty() ? home() : stored);
    } else if (!archive) {
        rescan();
    }
    tabIndex = std::clamp(tabIndex, 0, (int)tabs().size() - 1);
    if (zone == Zone::Slot && slotButtons().empty())
        zone = Zone::Files;
    loadFavorites();
    previewKey.clear();
    updatePreview();
}

// ------------------------------------------------------------ layout

auto MediaBrowser::layoutParts() const -> void {
    tabBar = { rect.x, rect.y, rect.w, 12 };
    slotRow = { rect.x, rect.y + 14, rect.w, 14 };
    int by = rect.y + 31;
    int bh = std::max(40, rect.y + rect.h - by);
    favRect = { rect.x, by, FavWidth, bh };
    int rx = rect.x + FavWidth + 8, rw = std::max(40, rect.w - FavWidth - 8);
    pathRect = { rx, by, rw, 11 };
    int rest = bh - 11;
    int fh = std::max(RowHeight * 2, (rest * 2 / 3) / RowHeight * RowHeight);
    fileRect = { rx, by + 11, rw, fh };
    prevRect = { rx, by + 11 + fh + 2, rw, std::max(PreviewRow, rest - fh - 2) };

    tabRects.clear();
    buttonRects.clear();
    if (!font)
        return;
    int x = tabBar.x;
    for (auto& t : tabs()) {
        int w = font->textWidth(OverlayFont::Pro, t.title) + 16;
        tabRects.push_back({ x, tabBar.y, w, 11 });
        x += w + 2;
    }
    auto buttons = slotButtons();
    buttonRects.resize(buttons.size());
    int right = slotRow.x + slotRow.w;
    for (int i = (int)buttons.size() - 1; i >= 0; i--) {
        int w = font->textWidth(OverlayFont::Pro, buttons[i]) + 12;
        right -= w;
        buttonRects[i] = { right, slotRow.y + 2, w, 11 };
        right -= 4;
    }
}

auto MediaBrowser::visibleFiles() const -> int {
    layoutParts();
    return std::max(1, fileRect.h / RowHeight);
}

auto MediaBrowser::visibleFavorites() const -> int {
    layoutParts();
    return std::max(1, (favRect.h - 4) / RowHeight);
}

auto MediaBrowser::visiblePreview() const -> int {
    layoutParts();
    return std::max(1, (prevRect.h - 4) / PreviewRow);
}

// ------------------------------------------------------------ drawing

auto MediaBrowser::draw(Painter& p, bool focused) -> void {
    layoutParts();
    auto tab = currentTab();
    auto t = tabs();

    // tab bar
    for (int i = 0; i < (int)t.size() && i < (int)tabRects.size(); i++) {
        auto& r = tabRects[i];
        bool sel = i == tabIndex;
        if (sel) p.rect(r.x, r.y, r.w, r.h, Theme::Selected);
        p.text(r.x + 8, r.y + 2, t[i].title, sel ? Theme::TextBright : Theme::TextDim);
    }
    p.rect(tabBar.x, tabBar.y + 11, tabBar.w, 1, Theme::Selected);

    // slot row
    auto buttons = slotButtons();
    auto rects = buttonRects;
    int right = rects.empty() ? slotRow.x + slotRow.w : rects[0].x - 4;
    std::string label, image;
    switch (tab) {
        case Tab::Autostart: image = "Disk → 8 · CRT → cartridge · TAP → tape · PRG → RAM"; break;
        case Tab::Disk8: label = "Drive 8:"; break;
        case Tab::Disk9: label = "Drive 9:"; break;
        case Tab::Cartridge: label = "Cartridge:"; break;
        case Tab::Tape: label = "Tape:"; break;
    }
    if (tab != Tab::Autostart) {
        image = mediaFile(tabMedia(tab));
        if (image.empty()) image = "-";
    }
    int lw = label.empty() ? -8 : p.text(slotRow.x, slotRow.y + 4, label, Theme::TextDim);
    p.textClip(slotRow.x + lw + 8, slotRow.y + 4, image, right - slotRow.x - lw - 12,
        tab == Tab::Autostart ? Theme::Text : Theme::TextBright, tab == Tab::Autostart ? OverlayFont::Pro : OverlayFont::Mono);
    for (int i = 0; i < (int)buttons.size(); i++) {
        auto& r = rects[i];
        bool f = focused && zone == Zone::Slot && i == slotButton;
        p.rect(r.x, r.y, r.w, r.h, f ? Theme::Selected : Theme::Sidebar);
        p.text(r.x + 6, r.y + 2, buttons[i], f ? Theme::TextBright : Theme::Text);
    }
    p.rect(slotRow.x, slotRow.y + slotRow.h + 1, slotRow.w, 1, Theme::Sidebar);

    // favorites
    p.rect(favRect.x, favRect.y, favRect.w, favRect.h, Color::hex(0x352879, 0.5f));
    {
        auto sel = selectableFavorites();
        int selIndex = favs.selected >= 0 && favs.selected < (int)sel.size() ? sel[favs.selected] : -1;
        int vis = visibleFavorites();
        if (selIndex >= 0) {
            if (selIndex < favs.top) favs.top = selIndex;
            if (selIndex >= favs.top + vis) favs.top = selIndex - vis + 1;
        }
        favs.top = std::clamp(favs.top, 0, std::max(0, (int)favorites.size() - vis));
        for (int r = 0; r < vis && favs.top + r < (int)favorites.size(); r++) {
            auto& f = favorites[favs.top + r];
            int y = favRect.y + 2 + r * RowHeight;
            if (f.header) {
                p.text(favRect.x + 6, y + 1, f.title, Theme::Accent);
                continue;
            }
            bool s = favs.top + r == selIndex && zone == Zone::Favorites;
            if (s) p.rect(favRect.x, y, favRect.w, RowHeight, focused ? Theme::Selected : Theme::SelectedIdle);
            auto col = f.missing ? Missing : (s && focused ? Theme::TextBright : Theme::Text);
            p.textClip(favRect.x + 6, y + 1, f.title, favRect.w - 10, col, OverlayFont::Mono);
        }
    }

    // path + filter
    std::string flt;
    if (!filter.empty()) flt = "filter: " + filter + "_";
    if (allFiles) flt += std::string(flt.empty() ? "" : "  ") + "all files";
    int fw = flt.empty() ? 0 : p.font.textWidth(OverlayFont::Pro, flt);
    if (!flt.empty()) p.textRight(pathRect.x + pathRect.w, pathRect.y + 1, flt, Theme::Warn);
    p.textClip(pathRect.x, pathRect.y + 1, location(), pathRect.w - fw - 8, Theme::Path);

    // file list
    {
        int vis = visibleFiles();
        files.clamp((int)entries.size(), vis);
        bool scrollbar = (int)entries.size() > vis;
        int inner = fileRect.w - (scrollbar ? 6 : 0);
        const int typeW = 32, sizeW = 44;
        for (int r = 0; r < vis && files.top + r < (int)entries.size(); r++) {
            auto& e = entries[files.top + r];
            int y = fileRect.y + r * RowHeight;
            bool s = files.top + r == files.selected;
            bool hot = s && focused && zone == Zone::Files;
            if (s) p.rect(fileRect.x, y, inner, RowHeight, hot ? Theme::Selected : Theme::SelectedIdle);
            bool folderLike = e.dir || e.archive;
            auto col = hot ? Theme::TextBright : (folderLike ? Folder : Theme::Text);
            p.textClip(fileRect.x + 2, y + 1, e.name + (e.dir && !e.parent ? "/" : ""), inner - typeW - sizeW - 6, col, OverlayFont::Mono);
            if (!e.dir)
                p.textRight(fileRect.x + inner - typeW - 4, y + 1, sizeText(e.size), hot ? Theme::TextBright : Theme::TextDim, OverlayFont::Mono);
            p.textRight(fileRect.x + inner - 2, y + 1, e.type, hot ? Theme::TextBright : (folderLike ? Folder : Theme::Accent), OverlayFont::Mono);
        }
        if (entries.empty() || (entries.size() == 1 && entries[0].parent))
            p.text(fileRect.x + 2, fileRect.y + RowHeight * (int)entries.size() + 1,
                filter.empty() ? "(no matching files - CTRL+H shows all)" : "(no match)", Theme::TextDim);
        if (scrollbar) {
            int trackH = vis * RowHeight;
            p.rect(fileRect.x + fileRect.w - 4, fileRect.y, 3, trackH, Theme::Track);
            int thumbH = std::max(6, trackH * vis / (int)entries.size());
            int thumbY = fileRect.y + (trackH - thumbH) * files.top / std::max(1, (int)entries.size() - vis);
            p.rect(fileRect.x + fileRect.w - 4, thumbY, 3, thumbH, Theme::Text);
        }
    }

    // preview
    p.rect(prevRect.x, prevRect.y, prevRect.w, 1, Theme::Sidebar);
    if (!preview.empty()) {
        int vis = visiblePreview();
        prev.clamp((int)preview.size(), vis);
        for (int r = 0; r < vis && prev.top + r < (int)preview.size(); r++) {
            int y = prevRect.y + 3 + r * PreviewRow;
            bool s = prev.top + r == prev.selected && zone == Zone::Preview;
            if (s) p.rect(prevRect.x, y, prevRect.w, PreviewRow, focused ? Theme::Selected : Theme::SelectedIdle);
            p.text(prevRect.x + 2, y + 1, preview[prev.top + r].text, s && focused ? Theme::TextBright : Theme::Text, OverlayFont::Mono);
        }
    } else if (!previewInfo.empty())
        p.textClip(prevRect.x + 2, prevRect.y + 4, previewInfo, prevRect.w - 4, Theme::TextDim);
}

// ------------------------------------------------------------ input

auto MediaBrowser::listNav(List& list, int count, int visible, Nav n) -> bool {
    int last = count - 1;
    switch (n) {
        case Nav::Up: if (list.selected <= 0) return false; list.selected--; break;
        case Nav::Down: if (list.selected >= last) return true; list.selected++; break;
        case Nav::PageUp: list.selected = std::max(0, list.selected - visible); break;
        case Nav::PageDown: list.selected = std::max(0, std::min(last, list.selected + visible)); break;
        case Nav::Home: list.selected = 0; break;
        case Nav::End: list.selected = std::max(0, last); break;
        default: return false;
    }
    list.clamp(count, visible);
    return true;
}

auto MediaBrowser::moveZone(int dir) -> bool {
    bool hasFavs = !selectableFavorites().empty();
    if (zone == Zone::Files) {
        if (dir < 0) {
            if (!hasFavs) return false;
            zone = Zone::Favorites;
        } else if (!preview.empty())
            zone = Zone::Preview;
        return true;
    }
    if (zone == Zone::Favorites) {
        if (dir < 0) return false;
        zone = Zone::Files;
        return true;
    }
    if (zone == Zone::Preview) {
        if (dir < 0) zone = Zone::Files;
        return true;
    }
    return false;
}

auto MediaBrowser::nav(Nav n) -> bool {
    switch (n) {
        case Nav::NextTab: switchTab(1); return true;
        case Nav::PrevTab: switchTab(-1); return true;
        case Nav::AllFiles:
            allFiles = !allFiles;
            rescan();
            if (notify) notify(allFiles ? "Showing all files" : "Showing matching files only");
            return true;
        case Nav::Bookmark: toggleBookmark(); return true;
        case Nav::Eject: eject(); return true;
        case Nav::Back:
            if (filter.empty()) return false;
            filter.clear();
            applyFilter();
            return true;
        case Nav::Backspace:
            if (!filter.empty()) {
                size_t i = filter.size() - 1;
                while (i > 0 && ((uint8_t)filter[i] & 0xc0) == 0x80) i--;
                filter.erase(i);
                applyFilter();
                return true;
            }
            if (zone != Zone::Slot) {
                zone = Zone::Files;
                goParent();
            }
            return true;
        case Nav::Parent:
            if (!filter.empty()) {
                filter.clear();
                applyFilter();
                return true;
            }
            if (zone == Zone::Files && goParent())
                return true;
            return false;
        default: break;
    }

    switch (zone) {
        case Zone::Slot: {
            int count = (int)slotButtons().size();
            if (!count) { zone = Zone::Files; return true; }
            slotButton = std::clamp(slotButton, 0, count - 1);
            switch (n) {
                case Nav::Left: if (slotButton == 0) return false; slotButton--; return true;
                case Nav::Right: slotButton = std::min(count - 1, slotButton + 1); return true;
                case Nav::Down: zone = Zone::Files; return true;
                case Nav::Up: return true;
                case Nav::Ok: case Nav::Insert: pressSlotButton(slotButton); return true;
                default: return false;
            }
        }
        case Zone::Files:
            switch (n) {
                case Nav::Up:
                    if (files.selected > 0) break;
                    if (!slotButtons().empty()) { zone = Zone::Slot; slotButton = 0; }
                    return true;
                case Nav::Left: return moveZone(-1);
                case Nav::Right: return moveZone(1);
                case Nav::Ok: activateEntry(true); return true;
                case Nav::Insert: activateEntry(false); return true;
                default: break;
            }
            if (listNav(files, (int)entries.size(), visibleFiles(), n)) {
                updatePreview();
                return true;
            }
            return n == Nav::Down;
        case Zone::Favorites: {
            int count = (int)selectableFavorites().size();
            switch (n) {
                case Nav::Left: return false;
                case Nav::Right: return moveZone(1);
                case Nav::Ok: activateFavorite(true); return true;
                case Nav::Insert: activateFavorite(false); return true;
                case Nav::Delete: removeFavorite(); return true;
                case Nav::Up: if (favs.selected <= 0) return true; break;
                default: break;
            }
            return listNav(favs, count, visibleFavorites(), n) || n == Nav::Down;
        }
        case Zone::Preview: {
            switch (n) {
                case Nav::Left: zone = Zone::Files; return true;
                case Nav::Right: return true;
                case Nav::Ok: case Nav::Insert:
                    if (auto e = selectedEntry()) load(targetOf(*e), n == Nav::Ok, (unsigned)prev.selected);
                    return true;
                case Nav::Up: if (prev.selected <= 0) return true; break;
                default: break;
            }
            return listNav(prev, (int)preview.size(), visiblePreview(), n) || n == Nav::Down;
        }
    }
    return false;
}

auto MediaBrowser::text(const std::string& s) -> bool {
    if (zone == Zone::Slot)
        return false;
    bool added = false;
    for (size_t pos = 0; pos < s.size();) {
        size_t start = pos;
        uint32_t cp = OverlayFont::decode(s, pos);
        if (cp < 32 || filter.size() + (pos - start) > 64)
            continue;
        filter += s.substr(start, pos - start);
        added = true;
    }
    if (added) {
        zone = Zone::Files;
        applyFilter();
    }
    return true;
}

auto MediaBrowser::click(int x, int y, int clicks) -> bool {
    layoutParts();
    for (int i = 0; i < (int)tabRects.size(); i++)
        if (tabRects[i].contains(x, y)) {
            selectTab(i);
            return true;
        }
    for (int i = 0; i < (int)buttonRects.size(); i++)
        if (buttonRects[i].contains(x, y)) {
            zone = Zone::Slot;
            slotButton = i;
            pressSlotButton(i);
            return true;
        }
    if (favRect.contains(x, y)) {
        int row = favs.top + (y - favRect.y - 2) / RowHeight;
        auto sel = selectableFavorites();
        auto it = std::find(sel.begin(), sel.end(), row);
        if (it == sel.end())
            return true;
        zone = Zone::Favorites;
        favs.selected = (int)(it - sel.begin());
        if (clicks >= 2) activateFavorite(true);
        return true;
    }
    if (fileRect.contains(x, y)) {
        int row = files.top + (y - fileRect.y) / RowHeight;
        if (row >= (int)entries.size())
            return true;
        zone = Zone::Files;
        files.selected = row;
        updatePreview();
        if (clicks >= 2) activateEntry(true);
        return true;
    }
    if (prevRect.contains(x, y) && !preview.empty()) {
        int row = prev.top + (y - prevRect.y - 3) / PreviewRow;
        if (row < 0 || row >= (int)preview.size())
            return true;
        zone = Zone::Preview;
        prev.selected = row;
        if (clicks >= 2)
            if (auto e = selectedEntry()) load(targetOf(*e), true, (unsigned)row);
        return true;
    }
    return true;
}

auto MediaBrowser::rightClick(int x, int y) -> bool {
    layoutParts();
    if (!fileRect.contains(x, y))
        return false;
    int row = files.top + (y - fileRect.y) / RowHeight;
    if (row >= (int)entries.size())
        return true;
    zone = Zone::Files;
    files.selected = row;
    updatePreview();
    activateEntry(false);
    return true;
}

auto MediaBrowser::wheelAt(int x, int y, int dy) -> bool {
    layoutParts();
    auto scroll = [dy](List& l, int count, int vis) {
        l.top = std::clamp(l.top - dy * 3, 0, std::max(0, count - vis));
        l.selected = std::clamp(l.selected, l.top, std::max(l.top, std::min(count - 1, l.top + vis - 1)));
    };
    if (fileRect.contains(x, y)) {
        scroll(files, (int)entries.size(), visibleFiles());
        updatePreview();
    } else if (prevRect.contains(x, y))
        scroll(prev, (int)preview.size(), visiblePreview());
    else if (favRect.contains(x, y))
        favs.top = std::clamp(favs.top - dy * 3, 0, std::max(0, (int)favorites.size() - visibleFavorites()));
    else if (tabBar.contains(x, y))
        switchTab(dy > 0 ? -1 : 1);
    return true;
}

// ------------------------------------------------------------ hints + MCP

auto MediaBrowser::hints() const -> std::vector<Hint> {
    auto tab = currentTab();
    std::string start = tab == Tab::Autostart ? "autostart" : tab == Tab::Disk9 ? "insert" : "insert + run";
    std::string back = filter.empty() ? "back" : "clear filter";
    switch (zone) {
        case Zone::Slot:
            return {{"RETURN", slotButton == 0 ? "eject" : "toggle"}, {"←→", "button"}, {"↓", "files"}, {"TAB", "slot"}, {"ESC", back}};
        case Zone::Favorites:
            return {{"RETURN", "open"}, {"DEL", "remove bookmark"}, {"→", "files"}, {"TAB", "slot"}, {"ESC", back}};
        case Zone::Preview: {
            std::string what = "load + run";
            if (prev.selected > 0 && prev.selected < (int)preview.size()) {
                auto& l = preview[prev.selected].plain;
                auto a = l.find('"'), b = l.rfind('"');
                if (a != std::string::npos && b > a)
                    what += " " + l.substr(a, b - a + 1);
            }
            return {{"RETURN", tab == Tab::Disk9 ? "insert" : what}, {"←", "files"}, {"^E", "eject"}, {"ESC", back}};
        }
        case Zone::Files: default: {
            auto e = selectedEntry();
            std::vector<Hint> out;
            if (e && (e->dir || e->archive)) out.push_back({"RETURN", "open"});
            else out.push_back({"RETURN", start});
            if (tab != Tab::Disk9 && tab != Tab::Cartridge) out.push_back({"SHIFT+RET", "insert"});
            out.push_back({"TAB", "slot"});
            out.push_back({"←→", "column"});
            out.push_back({"^B", "bookmark"});
            out.push_back({"^H", allFiles ? "matching files" : "all files"});
            out.push_back({"type", "filter"});
            out.push_back({"ESC", back});
            return out;
        }
    }
}

auto MediaBrowser::value() const -> std::string {
    auto e = selectedEntry();
    return location() + (e ? e->name : "");
}

auto MediaBrowser::describe(Json& out) const -> void {
    static const char* zones[] = {"slot", "favorites", "files", "preview"};
    layoutParts();
    out = Json::object();
    auto t = tabs();
    Json tabList = Json::array();
    for (int i = 0; i < (int)t.size(); i++) {
        tabList.push(t[i].id);
    }
    out.set("tabs", tabList);
    out.set("tab", t[std::clamp(tabIndex, 0, (int)t.size() - 1)].id);
    out.set("zone", zones[(int)zone]);
    out.set("folder", folder);
    out.set("location", location());
    out.set("archive", archivePath.empty() ? Json() : Json(archivePath));
    out.set("filter", filter);
    out.set("all_files", allFiles);

    Json slot = Json::object();
    auto media = tabMedia(currentTab());
    slot.set("image", mediaFile(media));
    if (media && media->guid && currentTab() != Tab::Cartridge && activeEmulator)
        slot.set("write_protect", FileSetting::getInstance(activeEmulator, _underscore(media->name))->writeProtect);
    Json buttons = Json::array();
    for (auto& b : slotButtons()) buttons.push(b);
    slot.set("buttons", buttons);
    if (zone == Zone::Slot) slot.set("focused_button", slotButton);
    out.set("slot", slot);

    Json list = Json::array();
    for (int i = 0; i < (int)entries.size() && i < 200; i++) {
        auto& e = entries[i];
        list.push(e.name + (e.dir && !e.parent ? "/" : "") + " [" + e.type + "]");
    }
    out.set("entries", list);
    auto e = selectedEntry();
    out.set("selected", e ? Json(e->name) : Json());

    Json favList = Json::array();
    auto sel = selectableFavorites();
    for (int i = 0; i < (int)favorites.size(); i++) {
        auto& f = favorites[i];
        if (f.header) continue;
        Json fe = Json::object();
        fe.set("kind", f.bookmark ? "bookmark" : "recent");
        fe.set("title", f.title);
        fe.set("path", f.path);
        if (f.missing) fe.set("missing", true);
        if (zone == Zone::Favorites && favs.selected < (int)sel.size() && sel[favs.selected] == i) fe.set("selected", true);
        favList.push(fe);
    }
    out.set("favorites", favList);

    Json pv = Json::array();
    for (auto& l : preview) pv.push(l.plain);
    out.set("preview", pv);
    if (!previewInfo.empty()) out.set("preview_info", previewInfo);
    if (zone == Zone::Preview) out.set("preview_selected", prev.selected);

    std::string rects;
    for (auto& r : tabRects) rects += (rects.empty() ? "" : ";") + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h);
    out.set("tab_rects", rects);
    auto rs = [](const Rect& r) { return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h); };
    out.set("file_rect", rs(fileRect));
    out.set("row_height", RowHeight);
    out.set("file_top", files.top);
}
