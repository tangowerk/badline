
#include "../view/message.h"
#include "miscHelper.h"

#include "../../emulation/libc64/interface.h"
#include "../program.h"
#include "../tools/chronos.h"
#include "../view/status.h"
#include "../tools/filesetting.h"
#include "../tools/filepool.h"
#include "../states/states.h"
#include "fileHelper.h"
#include "settingsHelper.h"

auto MiscHelper::addFileFonts() -> void {
    for(auto emulator : emulators) {
        if ( dynamic_cast<LIBC64::Interface*>(emulator)) {
            // listings as C64 screen codes (overlay draws them with the C64 Pro glyphs)
            ((LIBC64::Interface*) emulator)->convertPetsciiToScreencode(true);
            ((LIBC64::Interface*) emulator)->loadWithColumn( Program::getSettings(emulator)->get<bool>("autostart_load_with_column") );

        }
    }
}

auto MiscHelper::libraryMissing(std::string plugin) -> void {
    static unsigned ts = 0;
    if (!program->initialized)
        return;

    unsigned tsTemp = Chronos::getTimestampInMilliseconds();
    if (tsTemp - ts < 1000)
        return;

    if (plugin == "CAPS") {
        static int informed = 0;
        if (appMessage && (informed < 20) ) {
            appMessage->error("This image requires the SPS plugin at least version 4.2, available here: http://www.softpres.org");
            ts = Chronos::getTimestampInMilliseconds();
            informed++;
        }
    }
}

auto MiscHelper::setExpansionSelection( Emulator::Interface* emulator ) -> void {
    auto settings = Program::getSettings( emulator );

    for( auto& mediaGroup : emulator->mediaGroups ) {

        if ( mediaGroup.selected ) {

            auto mediaId = settings->get<unsigned>( _underscore( mediaGroup.name ) + "_selected", mediaGroup.media[0].id );

            auto media = emulator->getMedia( mediaGroup, mediaId );

            if (media && !media->parent)
                mediaGroup.selected = media;
        }

        if (mediaGroup.isHardDisk()) {
            for (auto& media : mediaGroup.media) {
                auto pcbId = settings->get<unsigned>(_underscore(media.name) + "_pcb", mediaGroup.expansion->pcbs[0].id);

                auto pcbLayout = emulator->getPCB(*mediaGroup.expansion, pcbId);

                media.pcbLayout = pcbLayout ? pcbLayout : &mediaGroup.expansion->pcbs[0];
            }
        }
    }

    for ( auto& expansion : emulator->expansions ) {

        if (!expansion.mediaGroup || (expansion.pcbs.size() == 0) )
            continue;

        for(auto& media : expansion.mediaGroup->media) {

            if (!media.pcbLayout || media.parent) {
                continue;
            }

            auto pcbId = settings->get<unsigned>( _underscore( media.name ) + "_pcb", expansion.pcbs[0].id );

            auto pcbLayout = emulator->getPCB( expansion, pcbId );

            media.pcbLayout = pcbLayout ? pcbLayout : &expansion.pcbs[0];
        }
    }
}

auto MiscHelper::removeExpansion( Emulator::Interface* emulator, bool bootableOnly ) -> void {
    if (bootableOnly && !emulator->isExpansionBootable())
        return;

    auto expansion = emulator->getExpansion();

    if (!expansion || expansion->isEmpty())
        return;

    Program::getSettings( emulator )->set<unsigned>("expansion", 0);

}

auto MiscHelper::prepareSocket(Emulator::Interface::Media* media, Emulator::Interface* emulator, std::string address) -> void {
    std::string port;

    auto parts = GUIKIT::String::split( address, ':' );

    if (parts.size() > 1) {

        port = parts[ parts.size() - 1 ];

        address = parts[0];
    }

    emulator->prepareSocket( media, address, port );
}

auto MiscHelper::toggle2Mhz() -> void {
    if (dynamic_cast<LIBC64::Interface*>(activeEmulator)) {
        bool state = dynamic_cast<LIBC64::Interface*>(activeEmulator)->toggle2Mhz();
        if (statusHandler)
            statusHandler->setMessage((state ? "2 MHz Turbo enabled" : "Turbo disabled"), false, true);
    }
}

auto MiscHelper::resetRunAhead() -> void {

    auto settings = Program::getSettings( activeEmulator );

    if ( settings->get<bool>( "runahead_disable", true) ) {

        settings->set<unsigned>( "runahead", 0);

        activeEmulator->runAhead( 0 );


    }
}

auto MiscHelper::setRunAhead(Emulator::Interface* emulator) -> void {

    auto settings = Program::getSettings( emulator );

    emulator->runAhead( settings->get<unsigned>( "runahead", 0, {0u, 10u}) );

    emulator->runAheadPerformance( settings->get<bool>( "runahead_performance", false) );

    emulator->runAheadPreventJit( settings->get<bool>( "runahead_prevent_jit", true ) );
}

auto MiscHelper::setRewind(Emulator::Interface* emulator) -> void {
    auto settings = Program::getSettings( emulator );

    bool rewindEnable = settings->get<bool>("rewind_enable", false);
    unsigned rewindStep = settings->get<unsigned>("rewind_step", 1, {1, 60});
    unsigned rewindBuffer = settings->get<unsigned>("rewind_buffer", 100, {10, 500});

    emulator->configRewind(rewindEnable ? rewindStep : 0, rewindBuffer);
}

auto MiscHelper::setJit(Emulator::Interface* emulator) -> void {

    auto settings = Program::getSettings(emulator);

    emulator->setInputSampling( settings->get<unsigned>("input_sampling", 2, {0, 2}) );

    auto manager = InputManager::getManager(emulator);

    manager->jit.rescanDelay = settings->get<unsigned>("input_jit_delay", 5, {1, 10});
}

auto MiscHelper::getDevice( Emulator::Interface* emulator, Emulator::Interface::Connector* connector ) -> Emulator::Interface::Device* {
    unsigned defaultDevice = 0;

    for(auto& device : emulator->devices) {
        if (device.isJoypad() && connector->isPort2()) {
            defaultDevice = device.id;
            break;
        }
    }

    auto deviceId = Program::getSettings(emulator)->get<unsigned>( _underscore(connector->name), defaultDevice);

    return emulator->getDevice( deviceId );
}