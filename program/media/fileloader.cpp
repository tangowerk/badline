
#include "../view/message.h"
#include "fileloader.h"
#include "autoloader.h"
#include "recentFiles.h"
#include "../tools/filepool.h"
#include "../states/states.h"
#include "../cmd/cmd.h"
#include "../firmware/manager.h"
#include "../thread/emuThread.h"
#include "../tools/diskGuesser.h"
#include "../view/status.h"
#include "../audio/manager.h"
#include "recentFiles.h"
#include "../helper/fileHelper.h"
#include "../helper/miscHelper.h"

Fileloader* fileloader = nullptr;

Fileloader::Fileloader() {}

Fileloader::~Fileloader() {
    for (auto recentFile : recentFiles)
        delete recentFile;
}

auto Fileloader::eject(Emulator::Interface* emulator, Emulator::Interface::Media* media) -> void {
    auto mediaGroup = media->group;
    auto useExpansion = emulator->getExpansion();
    auto insertedExpansion = emulator == activeEmulator && useExpansion && useExpansion->mediaGroup == mediaGroup;
    bool mediaIsRam = media->type == Emulator::Interface::Media::MemoryType::RAM || media->type == Emulator::Interface::Media::MemoryType::SRAM;

    if (!mediaGroup->isExpansion() || (insertedExpansion && (mediaIsRam || (media->parent && (mediaGroup->selected == media->parent))))) {
        emulator->ejectMedium(media);
        filePool->assign( _ident(emulator, media->name), nullptr);
        if (!cmd->noGui)
            States::getInstance( emulator )->updateImage( nullptr, media );
    } else {
        if (!cmd->noGui)
            States::getInstance(emulator)->forcePowerNextLoad = true;
    }

    media->guid = (uintptr_t)nullptr;


    filePool->assign( _ident(emulator, media->name + "store"), nullptr);
    filePool->unloadOrphaned();

    auto fSetting = FileSetting::getInstance(emulator, _underscore(media->name) );
    fSetting->init(false);


}

auto Fileloader::insertFile( Emulator::Interface* emulator, Emulator::Interface::Media* media, std::string filePath, unsigned flags, unsigned selection ) -> bool {

    auto settings = Program::getSettings( emulator );

    GUIKIT::File* file = filePool->get(filePath);
    if (!file)
        return false;

    if ((flags & DONT_REMEMBER_FOLDER) == 0) {
        auto folderPath = GUIKIT::File::buildRelativePath(file->getPath());
        settings->set<std::string>(_underscoreEx(media->group->name) + "_folder_auto", folderPath);
    }

    if (!file->exists() || !file->getSize()) {
        FileHelper::errorOpen(file, nullptr, appMessage);
        return false;
    }

    auto& items = file->scanArchive();

    emuThread->lock(true);
    insertImage( emulator, media, file, findSuitableItem(items, media->group) );
    emuThread->unlock();

    return true;
}

auto Fileloader::autoload(Emulator::Interface* emulator, Emulator::Interface::Media* media, unsigned selection, bool trapped, bool forceOverrideSpeeder) -> void {
    auto mediaGroup = media->group;
    auto settings = Program::getSettings( emulator );

    autoloader->set(emulator, media, trapped, selection);

    if (mediaGroup->isDrive()) {
        autoloader->activateDrive( emulator, mediaGroup, 1 );
    }

    if (mediaGroup->isExpansion()) {
        settings->set<unsigned>("expansion", mediaGroup->expansion->id);
    }

    if (!mediaGroup->isExpansion())
        MiscHelper::removeExpansion(emulator, true);

    program->power( emulator );

    if (statusHandler && mediaGroup->isExpansion() && activeEmulator->isExpansionUnsupported())
        statusHandler->setMessage("This cartridge may not be supported.", true);

    bool forceStandardKernal = false;
    if (media->group->isTape()) {
        forceStandardKernal = settings->get<bool>("autostart_tape_standard_kernal", false);
    }

    auto useExpansion = emulator->getExpansion();

    if (!dynamic_cast<LIBC64::Interface*>(emulator) )
        trapped = false;
    else if (trapped && (useExpansion && !useExpansion->isEmpty() && !useExpansion->isFastloader() && !useExpansion->isRS232() && !useExpansion->isTurboCart()))
        trapped = false;

    bool trapsWithSpeeder = trapped && mediaGroup->isDisk() && settings->get<bool>("autostart_speeder_traps", false);
    uint8_t options = (uint8_t)trapped;
    if (forceOverrideSpeeder) options |= 0x80;
    else if (trapped) {
        if (!trapsWithSpeeder)  options |= 0x80;
        else                    options |= 2;
    }

    if (forceStandardKernal || trapped || forceOverrideSpeeder) {
        auto fManager = FirmwareManager::getInstance( emulator );
        if (fManager->getStoreLevelInUse() > 0)
            fManager->insertDefault( trapsWithSpeeder );
    }

    if (mediaGroup->selected)
        emulator->selectListing(mediaGroup->selected, selection, "", options);
    else
        emulator->selectListing(media, selection, "", options);

    if (audioManager)
        audioManager->drive.reset(mediaGroup, true);



    if (mediaGroup->isDrive()) {
        program->initAutoWarp(mediaGroup);
        settings->set<int>("swap_pos", -1, false);
    }
}

auto Fileloader::updateFileSetting(FileSetting* fSetting, GUIKIT::File* file, GUIKIT::File::Item* item ) -> void {
    auto path = GUIKIT::File::buildRelativePath(file->getFile());
    std::string fName = file->getFileName( item );
    if (file->isArchived())
        fName = file->getFileName( ) + " - " + fName;

    fSetting->setPath(path, !cmd->autoload);
    fSetting->setFile(fName, !cmd->autoload);
    fSetting->setId(item->id, !cmd->autoload);
}

auto Fileloader::insertImage(Emulator::Interface* emulator, Emulator::Interface::Media* media, GUIKIT::File* file, GUIKIT::File::Item* item, int options) -> void {
    bool fromState = options & 1;
    bool dontUpdateSelected = options & 2;
    if (!item)
        return;
    auto mediaGroup = media->group;

    auto settings = Program::getSettings( emulator );

    auto fSetting = FileSetting::getInstance( emulator, _underscore(media->name ) );

    unsigned size = file->archiveDataSize(item->id);

    auto data = mediaGroup->isHardDisk() && !file->isArchived() ? nullptr : file->archiveData(item->id);

    auto useExpansion = emulator->getExpansion();

    auto insertedExpansion = emulator == activeEmulator && useExpansion && useExpansion->mediaGroup == mediaGroup;

    bool mediaIsRam = media->type == Emulator::Interface::Media::MemoryType::RAM || media->type == Emulator::Interface::Media::MemoryType::SRAM;

    bool updateGenericFileList = !media->parent && (mediaGroup->isProgram() || !mediaIsRam);

    if (!mediaGroup->isExpansion() || (insertedExpansion && (mediaIsRam || (media->parent && (mediaGroup->selected == media->parent))))) {
        emulator->ejectMedium(media);
        updateFileSetting(fSetting, file, item);
        media->guid = uintptr_t(file);

        emulator->insertMedium(media, data, size);
        emulator->writeProtect(media, fSetting->writeProtect);
        if (!mediaGroup->isProgram())
            filePool->assign(_ident(emulator, media->name), file);

        if (mediaGroup->isHardDisk() && mediaGroup->expansion->pcbs.size())
            settings->set<unsigned>(_underscore(media->name) + "_pcb", 0);

        if (!cmd->noGui)
            States::getInstance(emulator)->updateImage(fSetting, media);
    } else {
        auto ext = GUIKIT::String::getExtension(file->getFile(), "bin");
        GUIKIT::String::toLowerCase(ext);
        if (ext != "crt")
            updateGenericFileList = false;

        if (mediaGroup->expansion->pcbs.size())
            settings->set<unsigned>( _underscore(media->name) + "_pcb", 0);

        updateFileSetting(fSetting, file, item);

        if (!cmd->noGui)
            States::getInstance(emulator)->forcePowerNextLoad = true;
    }

    auto recentFile = getRecentFile(emulator);

    if (!cmd->autoload) {
        RecentFiles::FileIdent fI{GUIKIT::File::buildRelativePath(file->getFile()), fSetting->file, item->id};
        recentFile->add(mediaGroup, media->parent != nullptr, fI, updateGenericFileList);
    }

    emulator->getListing(media);


    if (!dontUpdateSelected && mediaGroup->selected && !media->parent ) {
        mediaGroup->selected = media;
        settings->set<unsigned>(_underscore(mediaGroup->name) + "_selected", media->id);
    }

    if (!mediaGroup->isProgram())
        filePool->assign(_ident(emulator, media->name + "store"), file);

    filePool->unloadOrphaned();

    if (!cmd->noGui) {
        if (!fromState && mediaGroup->isDrive() && fSetting)
            FileHelper::updateSaveIdent( emulator, fSetting );
    }
}

auto Fileloader::preselectPath( GUIKIT::Settings* settings, std::string& groupName ) -> std::string {
    std::string baseFolderIdent = _underscoreEx(groupName) + "_folder_auto";
    auto path = settings->get<std::string>( baseFolderIdent, "" );
    return GUIKIT::File::resolveRelativePath(path);
}

auto Fileloader::loadSettings(Emulator::Interface* emulator) -> void {

    for(auto& group : emulator->mediaGroups) {
        for(auto& media : group.media) {
            auto fSetting = FileSetting::getInstance(emulator, _underscore(media.name) );
            fSetting->update();
        }
    }

    // swapper
    for (unsigned i = 1; i < SWAPPER_SLOTS; i++) {
        auto fSetting = FileSetting::getInstance( emulator, "swapper_" + std::to_string( i ) );
        fSetting->update();
    }
}

auto Fileloader::getSwapPos(Emulator::Interface* emulator) -> unsigned {
    auto settings = Program::getSettings( emulator );
    Emulator::Interface::Media* media = autoloader->getLatestDrive(emulator);
    if (media->group->isDisk()) {
        auto mediaId = settings->get<unsigned>("access_floppy", 0u, {0u, 3u});
        auto media2 = emulator->getEnabledDisk(mediaId);
        if (media2)
            media = media2;
    }

    auto fSetting = FileSetting::getInstance( activeEmulator, _underscore(media->name ) );
    if (fSetting->path.empty())
        return 0;

    DiskGuesser diskGuesser(fSetting->path);
    return diskGuesser.getDiskPos(fSetting->id);
}

auto Fileloader::getSwapMedia(Emulator::Interface* emulator, int swapPos, FileSetting* fSetting) -> Emulator::Interface::Media* {
    auto settings = Program::getSettings( emulator );
    Emulator::Interface::Media* media = nullptr;

    if (fSetting->path.empty() || (swapPos == 0) ) {
        media = autoloader->getLatestDrive(emulator);
    } else {
        std::string fileSuffix = GUIKIT::String::getExtension(fSetting->file, "exe");
        GUIKIT::String::toLowerCase( fileSuffix );

        auto driveGroups = emulator->getDriveMediaGroups();
        for (auto group: driveGroups) {
            for (auto& suffix: group->suffix) {
                if (suffix == fileSuffix) {
                    media = &group->media[0];
                    break;
                }
            }
            if (media)
                break;
        }
    }

    if (media && media->group->isDisk()) {
        auto mediaId = settings->get<unsigned>("access_floppy", 0u, {0u, 3u});
        auto media2 = emulator->getEnabledDisk(mediaId);
        if (media2)
            media = media2;
    }

    return media;
}

auto Fileloader::insertSwapDisk(Emulator::Interface* emulator, unsigned swapPos) -> Emulator::Interface::Media* {
    GUIKIT::File* file;
    Program::getSettings( emulator )->set<int>("swap_pos", swapPos, false);
    FileSetting* fSetting = FileSetting::getInstance( emulator, "swapper_" + std::to_string(swapPos) );
    Emulator::Interface::Media* media = getSwapMedia(emulator, swapPos, fSetting);
    if (!media)
        return nullptr;

    FileSetting fs;
    if (fSetting->path.empty() || (swapPos == 0) ) {
        fSetting = &fs;
        // auto create
        auto srcSetting = FileSetting::getInstance(emulator, _underscore(media->name) );

        if (srcSetting->path.empty() && (&media->group->media[0] != media) ) {
            srcSetting = FileSetting::getInstance(emulator, _underscore(media->group->media[0].name) );
            if (srcSetting->path.empty())
                return nullptr;
        }

        if (srcSetting->path.empty())
            return nullptr;

        DiskGuesser diskGuesser( srcSetting->path );
        auto result = diskGuesser.search( swapPos, srcSetting->id );

        if (!result.isEmpty()) {
            fSetting->file = result.fileName;
            fSetting->path = result.filePath;
            fSetting->id = result.archivePos;
            fSetting->writeProtect = false;
        }
    }

    file = filePool->get(GUIKIT::File::resolveRelativePath(fSetting->path));

    GUIKIT::File::Item item;
    item.id = fSetting->id;
    item.info.name = fSetting->file;

    if (!file || !file->exists() || !file->getSize() || (file->archiveData(fSetting->id) == nullptr)) {
        statusHandler->setMessage(GUIKIT::String::substitute("The file %path% couldn't be opened.", {{ "%path%", fSetting->file }}), true);
        return nullptr;
    }

    filePool->assign( _ident(emulator, "swapper_" + std::to_string(swapPos)), file);

    auto fSetting2 = FileSetting::getInstance( emulator, _underscore(media->name ) );
    fSetting2->setWriteProtect(fSetting->writeProtect);

    insertImage( emulator, media, file, &item );

    autoloader->setOnlyForFirstDrive(emulator, media);

    statusHandler->setMessage( GUIKIT::String::substitute("%file% was inserted in drive %drive%.", {{"%drive%", media->name},{"%file%", fSetting->file}}) );

    return media;
}

auto Fileloader::getRecentFile(Emulator::Interface* emulator) -> RecentFiles* {
    for (auto recentFile : recentFiles) {
        if (recentFile->emulator == emulator)
            return recentFile;
    }

    auto _ident = emulator->ident;
    GUIKIT::String::toLowerCase(_ident);

    std::string path = FileHelper::generatedFolder("") + _ident + "_recent.ini";

    auto recentFile = new RecentFiles(emulator, path);
    recentFiles.push_back(recentFile);
    return recentFile;
}

auto Fileloader::findSuitableItem(std::vector<GUIKIT::File::Item>& items, Emulator::Interface::MediaGroup* group) -> GUIKIT::File::Item* {
    std::vector<Emulator::Interface::MediaGroup*> groups;
    groups.push_back(group);
    return findSuitableItem(items, groups);
}

auto Fileloader::findSuitableItem(std::vector<GUIKIT::File::Item>& items) -> GUIKIT::File::Item* {
    std::vector<Emulator::Interface::MediaGroup*> groups;
    for (auto* emulator : emulators)
        GUIKIT::Vector::combine(groups, emulator->getMediaGroups());

    return findSuitableItem(items, groups);
}

auto Fileloader::findSuitableItem(std::vector<GUIKIT::File::Item>& items, std::vector<Emulator::Interface::MediaGroup*>& groups) -> GUIKIT::File::Item* {
    std::vector<std::string> suffixList;

    for(auto* group : groups)
        GUIKIT::Vector::combine(suffixList, group->suffix);

    if (items.empty())
        return nullptr;

    for (auto& item : items) {
        if (item.isDirectory)
            continue;

        auto& info = item.info;

        if (!info.size || !info.exists)
            continue;

        std::string fileSuffix = GUIKIT::String::getExtension(info.name, "exe");
        GUIKIT::String::toLowerCase( fileSuffix );

        if (GUIKIT::Vector::find(suffixList, fileSuffix))
            return &item;
    }

    return nullptr;
}
