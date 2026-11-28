
#pragma once

#include "../../emulation/interface.h"
#include "../../guikit/api.h"

struct MiscHelper {

    static auto libraryMissing(std::string plugin) -> void;
    static auto setExpansionSelection( Emulator::Interface* emulator ) -> void;
    static auto removeExpansion( Emulator::Interface* emulator, bool bootableOnly = false ) -> void;
    static auto prepareSocket(Emulator::Interface::Media* media, Emulator::Interface* emulator, std::string address) -> void;
    static auto toggle2Mhz() -> void;

    static auto addFileFonts() -> void;

    static auto resetRunAhead() -> void;
    static auto setRunAhead(Emulator::Interface* emulator) -> void;
    static auto setRewind(Emulator::Interface* emulator) -> void;
    static auto setJit(Emulator::Interface* emulator) -> void;
    static auto getDevice( Emulator::Interface* emulator, Emulator::Interface::Connector* connector ) -> Emulator::Interface::Device*;

};

