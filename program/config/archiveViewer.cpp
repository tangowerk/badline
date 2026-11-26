
#include "archiveViewer.h"
#include "../program.h"
#include "../tools/filepool.h"
#include "../../data/icons.h"
#include "../view/view.h"
#include "../helper/fileHelper.h"
#include "../helper/miscHelper.h"

ArchiveViewer* archiveViewer = nullptr;

auto ArchiveViewer::build() -> void {
    imgFolderOpen.loadPng((uint8_t*)Icons::folderOpen, sizeof(Icons::folderOpen) );
    imgFolderClosed.loadPng((uint8_t*)Icons::folderClosed, sizeof(Icons::folderClosed) );
    imgDocument.loadPng((uint8_t*)Icons::document, sizeof(Icons::document) );

    layout.append(tv, {~0u, ~0u});
    layout.setMargin(10);

    append(layout);
	translate();

    onClose = [this]() {
        filePool->unloadOrphaned();
        setVisible(false);
        view->setFocused(100);
    };

    onMove = [&]() {
        if (fullScreen()) return;
        GUIKIT::Geometry geometry = this->geometry();
        globalSettings->set<int>("screen_archiveviewer_x", geometry.x);
        globalSettings->set<int>("screen_archiveviewer_y", geometry.y);
    };

    onSize = [&](GUIKIT::Window::SIZE_MODE sizeMode) {
        if (fullScreen()) return;
        GUIKIT::Geometry geometry = this->geometry();
        globalSettings->set<unsigned>("screen_archiveviewer_width", geometry.width);
        globalSettings->set<unsigned>("screen_archiveviewer_height", geometry.height);
    };

    builded = true;
}

auto ArchiveViewer::setView(GUIKIT::File* file, std::vector<GUIKIT::File::Item>& items, bool multiSelection) -> void {
    this->filesSelected = 0;

    if (!builded) {
        build();
    }

    tv.onActivate = [this, file, multiSelection]() {
        auto item = tv.selected();
        if (!item) return;
        auto fileItem = (GUIKIT::File::Item*)item->userData();
        if (fileItem->isDirectory) return;

        if (item->text() == "-") return;

        if (!multiSelection)
            setVisible(false);
        else
            item->setText("-");

        if (file && onCallback)
            onCallback(file, fileItem);

        filesSelected++;
    };

    unsigned fileCount = 0;
    unsigned sufFileCount = 0;
    GUIKIT::File::Item* useFile = nullptr;
    std::string fileSuffix;

    tv.reset();
    for(auto& item : itemList) delete item;
    itemList.clear();

    for( auto& item : items ) {
        if(item.parent) continue;

        std::function<GUIKIT::TreeViewItem* (GUIKIT::File::Item&)> addItem;

        addItem = [&](GUIKIT::File::Item& item) -> GUIKIT::TreeViewItem* {
            auto tvi = new GUIKIT::TreeViewItem;
            itemList.push_back(tvi);
            std::string txt = item.info.name;
            if (!item.isDirectory) {
                txt += " [" + GUIKIT::File::SizeFormated( item.info.size ) + "]";
            }
            tvi->setText( txt );
            tvi->setUserData( (uintptr_t)&item );

            for(auto child : item.childs) {
                GUIKIT::TreeViewItem* citem = addItem(*child);
                tvi->append(*citem);
            }
            if (item.isDirectory) {
                tvi->setImage(imgFolderClosed);
                tvi->setImageSelected(imgFolderOpen);
            } else {
                tvi->setImage(imgDocument);
                fileCount++;

                if (!suffixList.empty()) {
                    fileSuffix = GUIKIT::String::getExtension(item.info.name, "exe");
                    GUIKIT::String::toLowerCase( fileSuffix );

                    if (GUIKIT::Vector::find(suffixList, fileSuffix)) {
                        sufFileCount++;
                        if (!useFile)
                            useFile = &item;
                    }
                } else if (!useFile)
                    useFile = &item;
            }
            return tvi;
        };

        GUIKIT::TreeViewItem* tvi = addItem(item);
        tv.append(*tvi);
    }

    if (useFile && ( ( suffixList.empty() && (fileCount == 1) ) || ( !suffixList.empty() && (sufFileCount == 1) ) ) ) {
        setVisible(false);
        if (onCallback)
            onCallback( file, useFile);
        return;
    }

    if (fileCount == 0) {
        setVisible(false);
        if (onCallback)
            onCallback( file, nullptr);
        return;
    }

    if (!geometryInitialized) {
        MiscHelper::applyGeometry(this, globalSettings,"screen_archiveviewer", {100, 100, 400, 350});
        geometryInitialized = true;
    }

    mtimer.setInterval(30);

    mtimer.onFinished = [this]() {
        mtimer.setEnabled(false);
        setVisible();
        setFocused(100);
    };
    mtimer.setEnabled();
}

auto ArchiveViewer::translate() -> void {
	setTitle( trans->getA("archive_selector") );
}

auto ArchiveViewer::preferSuffixes(std::vector<Emulator::Interface::MediaGroup*> groups) -> void {
    suffixList.clear();

    if (!groups.empty()) {
        for (auto group : groups) {
            for (auto& suffix : group->suffix)
                suffixList.push_back(&suffix);
        }
    }
}
