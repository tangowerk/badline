
#pragma once

#include <thread>
#include <mutex>
#include <atomic>
#include "../../guikit/api.h"
#include "../../emulation/interface.h"

struct RecentFiles;
struct FileSetting;

#define SWAPPER_SLOTS 25

struct Fileloader {

    Fileloader();
    ~Fileloader();

    std::vector<RecentFiles*> recentFiles;



    static const unsigned DO_AUTOLOAD = 1;
    static const unsigned USE_TRAPS = 0x80;
    static const unsigned OVERRIDE_SPEEDER = 0x40;
    static const unsigned DONT_REMEMBER_FOLDER = 0x100;

    auto eject(Emulator::Interface* emulator, Emulator::Interface::Media* media) -> void;
    auto insertFile( Emulator::Interface* emulator, Emulator::Interface::Media* media, std::string filePath, unsigned flags = 0, unsigned selection = 0 ) -> bool;
    auto insertImage( Emulator::Interface* emulator, Emulator::Interface::Media* media, GUIKIT::File* file, GUIKIT::File::Item* item, int options = 0) -> void;
    auto preselectPath( GUIKIT::Settings* settings, std::string& groupName ) -> std::string;
    auto autoload(Emulator::Interface* emulator, Emulator::Interface::Media* media, unsigned selection, bool trapped, bool forceOverrideSpeeder = false) -> void;
    auto loadSettings(Emulator::Interface* emulator) -> void;
    auto insertSwapDisk(Emulator::Interface* emulator, unsigned swapPos) -> Emulator::Interface::Media*;
    auto getSwapPos(Emulator::Interface* emulator) -> unsigned;
    auto getSwapMedia(Emulator::Interface* emulator, int swapPos, FileSetting* fSetting) -> Emulator::Interface::Media*;
    auto updateFileSetting(FileSetting* fSetting, GUIKIT::File* file, GUIKIT::File::Item* item ) -> void;
    auto getRecentFile(Emulator::Interface* emulator) -> RecentFiles*;

    static auto findSuitableItem(std::vector<GUIKIT::File::Item>& items) -> GUIKIT::File::Item*;
    static auto findSuitableItem(std::vector<GUIKIT::File::Item>& items, Emulator::Interface::MediaGroup* group) -> GUIKIT::File::Item*;
    static auto findSuitableItem(std::vector<GUIKIT::File::Item>& items, std::vector<Emulator::Interface::MediaGroup*>& groups) -> GUIKIT::File::Item*;
};

extern Fileloader* fileloader;
