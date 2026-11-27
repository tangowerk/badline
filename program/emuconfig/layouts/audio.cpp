
#include "audio.h"
#include "../config.h"
#include "../../../data/icons.h"
#include "../../thread/emuThread.h"
#include "../../view/view.h"
#include "../../view/status.h"
#include "../../audio/manager.h"
#include "../../helper/fileHelper.h"
#include "../../helper/miscHelper.h"
#include "system.h"

#define mes this->tabWindow->message
#define _settings this->tabWindow->settings

namespace EmuConfigView {

AudioDriveLayout::TapeSelection::TapeSelection() {
    append( label, {0u, 0u}, 10 );
    append( combo, {0u, 0u} );
    append( spacer, {~0u, 0u} );
    append( reload, {0u, 0u} );

    setAlignment( 0.5 );
}

AudioDriveLayout::FloppySelection::FloppySelection() {
    append( label, {0u, 0u}, 10 );
    append( combo, {0u, 0u}, 10 );
    append( labelExt, {0u, 0u}, 10 );
    append( comboExt, {0u, 0u} );
    append( spacer, {~0u, 0u} );
    append( reload, {0u, 0u} );

    setAlignment( 0.5 );
}

AudioDriveLayout::AudioDriveLayout(Emulator::Interface* emulator) :
floppyVolume("%", SliderLayout::ACTIVATOR),
floppyVolumeExt("%"),
tapeVolume("%", SliderLayout::ACTIVATOR),
tapeNoiseVolume("%", SliderLayout::ACTIVATOR) {
    append( floppyVolume, {~0u, 0u}, 10 );
    append( floppyVolumeExt, {~0u, 0u}, 10 );
    append( floppySelection, {~0u, 0u}, 10 );
    auto tapeMediaGroup = emulator->getTapeMediaGroup();

    if(tapeMediaGroup) {
        append(tapeVolume, {~0u, 0u}, 10);
        append(tapeSelection, {~0u, 0u}, 10);
        append(tapeNoiseVolume, {~0u, 0u});
    }

    setPadding(10);
    setFont(GUIKIT::Font::system("bold"));

    floppyVolume.slider.setLength( 301 );
    floppyVolume.updateValueWidth( "300 %" );

    floppyVolumeExt.slider.setLength( 301 );
    floppyVolumeExt.updateValueWidth( "300 %" );

    if(tapeMediaGroup) {
        tapeVolume.slider.setLength(301);
        tapeVolume.updateValueWidth("300 %");
        tapeNoiseVolume.slider.setLength(301);
        tapeNoiseVolume.updateValueWidth("300 %");
    }
}

VolumeControlLayout::Info::Info() {
    append(label, {0u, 0u}, 5);
    append(value, {50u, 0u});
}

VolumeControlLayout::VolumeControlLayout() {
    append( volumeSlider, {30, ~0u}, 5 );
    append( info, {0u, 0u});
    volumeSlider.setLength(101);
}

AudioModelLayout::ControlLayout::ControlLayout() {
    append(label, {0u, 0u}, 7);
    append(firstAll, {0u, 0u}, 5);
    append(secondAll, {0u, 0u});
    append(spacer, {~0u, 0u} );
}

AudioLayout::AudioLayout(TabWindow* tabWindow) {
    
    this->tabWindow = tabWindow;
    this->emulator = tabWindow->emulator;

    setMargin(10);

    driveLayout = new AudioDriveLayout(this->emulator);
    moduleList.setHeaderText( { "" } );
    moduleList.setHeaderVisible( false );
    
    processorImage.loadPng((uint8_t*)Icons::processor, sizeof(Icons::processor));
    driveImage.loadPng((uint8_t*)Icons::drive, sizeof(Icons::drive));

    moduleList.append( {"SID"} );
    moduleList.setImage(0, 0, processorImage);

    moduleList.append( {"Drive"} );
    moduleList.setImage(moduleList.rowCount() - 1, 0, driveImage);

    moduleList.setSelection(0);
    moduleFrame.append( moduleList, { GUIKIT::Font::scale(140), GUIKIT::Font::scale(dynamic_cast<LIBC64::Interface*>(emulator) ? 120 : 100)}, 15 );
    moduleFrame.append( volumeLayout, {0u, ~0u} );
    moduleFrame.setPadding(10);
    moduleFrame.setFont( GUIKIT::Font::system("bold") );
    
    moduleList.onChange = [this]() {

        if (!moduleList.selected())
            return;

        moduleSwitch.setSelection( moduleList.selection() );
    };
    
    append( moduleFrame, {0u, ~0u}, 10 );

    std::vector<unsigned> dim;
    if (dynamic_cast<LIBC64::Interface*>(emulator))
        dim = {5, 1, 1, 1, 2, 4, 4, 4, 4, 4, 4, 4, 4};
    else
        dim = {1,1};

    settingsLayout.build(tabWindow, emulator,
        {Emulator::Interface::Model::Purpose::SoundChip,
         Emulator::Interface::Model::Purpose::AudioSettings,
         Emulator::Interface::Model::Purpose::AudioResampler}, dim, dynamic_cast<LIBC64::Interface*>(emulator) ? 5 : 8);

    settingsLayout.setEvents();
    
    append( moduleSwitch, {~0u, ~0u} );

    int layPos = 0;
    moduleSwitch.setLayout( layPos++, settingsLayout, {~0u, ~0u} );
    moduleSwitch.setLayout( layPos++, *driveLayout, {~0u, ~0u} );
    
    driveLayout->floppyVolume.active.onToggle = [this](bool checked) {

        _settings->set<bool>( "audio_floppy", checked );

        if (emulator == activeEmulator) {
            emuThread->lock();
            audioManager->setDriveSounds();
            emuThread->unlock();
        }
    };

    driveLayout->floppyVolume.slider.onChange = [this](unsigned position) {
        _settings->set<unsigned>( "audio_floppy_volume", position );

        driveLayout->floppyVolume.value.setText( std::to_string( position ) + " %" );

        if (activeEmulator) {
            emuThread->lock();
            audioManager->drive.setVolume(activeEmulator, activeEmulator->getDiskMediaGroup(), (float) position * 0.01, false);
            emuThread->unlock();
        }
    };

    driveLayout->floppyVolumeExt.slider.onChange = [this](unsigned position) {
        _settings->set<unsigned>( "audio_floppy_volume_external", position );

        driveLayout->floppyVolumeExt.value.setText( std::to_string( position ) + " %" );

        if (activeEmulator) {
            emuThread->lock();
            audioManager->drive.setVolume(activeEmulator, activeEmulator->getDiskMediaGroup(), (float) position * 0.01, true);
            emuThread->unlock();
        }
    };

    driveLayout->floppySelection.combo.onChange = [this]() {

        std::string folder = driveLayout->floppySelection.combo.text();

        _settings->set<std::string>( audioManager->drive.getFloppyFolderIdent(emulator, false), folder );

        emuThread->lock();
        audioManager->drive.unload( emulator, emulator->getDiskMediaGroup(), false );
        if (emulator == activeEmulator)
            audioManager->setDriveSounds( false );
        emuThread->unlock();
    };

    driveLayout->floppySelection.comboExt.onChange = [this]() {

        std::string folder = driveLayout->floppySelection.comboExt.text();

        _settings->set<std::string>( audioManager->drive.getFloppyFolderIdent(emulator, true), folder );

        emuThread->lock();
        audioManager->drive.unload( emulator, emulator->getDiskMediaGroup(), true );
        if (emulator == activeEmulator)
            audioManager->setDriveSounds( false );
        emuThread->unlock();
    };

    driveLayout->floppySelection.reload.onActivate = [this]() {
        updateFloppyProfileList();

        driveLayout->floppySelection.combo.setSelectionByText( audioManager->drive.getFloppyFolder(emulator, false) );
        driveLayout->floppySelection.comboExt.setSelectionByText( audioManager->drive.getFloppyFolder(emulator, true) );

        // in case if content doesn't match setting anymore
        _settings->set<std::string>(audioManager->drive.getFloppyFolderIdent(emulator, false), driveLayout->floppySelection.combo.text() );
        _settings->set<std::string>(audioManager->drive.getFloppyFolderIdent(emulator, true), driveLayout->floppySelection.comboExt.text() );

        emuThread->lock();
        audioManager->drive.unload( emulator, emulator->getDiskMediaGroup(), false );
        audioManager->drive.unload( emulator, emulator->getDiskMediaGroup(), true );
        if (emulator == activeEmulator)
            audioManager->setDriveSounds( false );
        emuThread->unlock();
    };

    driveLayout->tapeVolume.active.onToggle = [this](bool checked) {

        _settings->set<bool>( "audio_tape", checked );

        if (emulator == activeEmulator) {
            emuThread->lock();
            audioManager->setDriveSounds();
            emuThread->unlock();
        }
    };

    driveLayout->tapeVolume.slider.onChange = [this](unsigned position) {

        _settings->set<unsigned>( "audio_tape_volume", position );

        driveLayout->tapeVolume.value.setText( std::to_string( position ) + " %" );

        if (activeEmulator) {
            emuThread->lock();
            audioManager->drive.setVolume(activeEmulator, activeEmulator->getTapeMediaGroup(), (float)position * 0.01, false);
            emuThread->unlock();
        }
    };

    driveLayout->tapeNoiseVolume.active.onToggle = [this](bool checked) {

        _settings->set<bool>( "audio_tape_noise", checked );

        if (emulator == activeEmulator) {
            emuThread->lock();
            audioManager->setTapeNoise();
            emuThread->unlock();
        }
    };

    driveLayout->tapeNoiseVolume.slider.onChange = [this](unsigned position) {

        _settings->set<unsigned>( "audio_tape_noise_volume", position );

        driveLayout->tapeNoiseVolume.value.setText( std::to_string( position ) + " %" );

        if (emulator == activeEmulator) {
            emuThread->lock();
            audioManager->setTapeNoise();
            emuThread->unlock();
        }
    };

    driveLayout->tapeSelection.combo.onChange = [this]() {

        std::string folder = driveLayout->tapeSelection.combo.text();

        _settings->set<std::string>( "audio_tape_folder", folder );

        emuThread->lock();
        audioManager->drive.unload( emulator, emulator->getTapeMediaGroup(), false );
        if (emulator == activeEmulator)
            audioManager->setDriveSounds( false );
        emuThread->unlock();
    };

    driveLayout->tapeSelection.reload.onActivate = [this]() {
        updateTapeProfileList();

        driveLayout->tapeSelection.combo.setSelectionByText( _settings->get<std::string>("audio_tape_folder", "") );

        // in case if content doesn't match setting anymore
        _settings->set<std::string>("audio_tape_folder", driveLayout->tapeSelection.combo.text() );

        emuThread->lock();
        audioManager->drive.unload( emulator, emulator->getTapeMediaGroup(), false );
        if (emulator == activeEmulator)
            audioManager->setDriveSounds( false );
        emuThread->unlock();
    };

    volumeLayout.volumeSlider.onChange = [this](unsigned position) {
        _settings->set<unsigned>("audio_volume", position);

        volumeLayout.info.value.setText( std::to_string(position) + "%");

        emuThread->lock();

        if (activeEmulator == emulator) {
            if (statusHandler)
                statusHandler->setVolumeSlider(position);

            audioManager->setVolume();
        }

        emuThread->unlock();
    };

    updateFloppyProfileList();

    updateTapeProfileList();

    loadSettings();
}

auto AudioLayout::updateVolumeSlider() -> void {
    unsigned volume = _settings->get<unsigned>("audio_volume", 100, {0, 100});
    volumeLayout.volumeSlider.setPosition(volume);
    volumeLayout.info.value.setText( std::to_string(volume) + "%");
}

auto AudioLayout::updateFloppyProfileList() -> void {

    driveLayout->floppySelection.combo.reset();
    driveLayout->floppySelection.comboExt.reset();

    auto baseFolder = program->soundFolder();

    auto list = GUIKIT::File::getFolderList(baseFolder + "floppy/" + emulator->ident);

    for (auto& info : list) {
        driveLayout->floppySelection.combo.append( info.name );
        driveLayout->floppySelection.comboExt.append( info.name );
    }
}

auto AudioLayout::updateTapeProfileList() -> void {

    driveLayout->tapeSelection.combo.reset();

    auto baseFolder = program->soundFolder();

    auto list = GUIKIT::File::getFolderList(baseFolder + "tape/" + emulator->ident);

    for (auto& info : list) {
        driveLayout->tapeSelection.combo.append( info.name );
    }
}

auto AudioLayout::translate() -> void {
    settingsLayout.translate();
    moduleFrame.setText(trans->get("selection"));

    if (dynamic_cast<LIBC64::Interface*>(this->emulator)) {
        settingsLayout.controlLayout.label.setText(trans->getA("all"));
        settingsLayout.controlLayout.firstAll.setText("8580");
        settingsLayout.controlLayout.secondAll.setText("6581");

        auto& lines = settingsLayout.lines;
        std::vector<GUIKIT::Layout*> layouts = {&settingsLayout.controlLayout,
            lines[5]->blocks[0], lines[6]->blocks[0], lines[7]->blocks[0], lines[8]->blocks[0],
            lines[9]->blocks[0], lines[10]->blocks[0], lines[11]->blocks[0], lines[12]->blocks[0]};

        alignChildWidth( layouts );
    }
    
    driveLayout->setText( trans->get("Drive Noise") );
    driveLayout->floppyVolume.active.setText( trans->get("Floppy") );
    driveLayout->floppyVolumeExt.name.setText( trans->get(dynamic_cast<LIBC64::Interface*>(emulator) ? "3.5-inch" : "external") );
    driveLayout->floppySelection.label.setText( trans->get("Floppy Profile") );
    driveLayout->floppySelection.labelExt.setText( dynamic_cast<LIBC64::Interface*>(emulator)
        ? trans->get("Floppy 3.5-inch Profile") : trans->get("External Floppy Profile") );
    driveLayout->floppySelection.reload.setText( trans->get("Reload") );
    driveLayout->floppySelection.reload.setTooltip( trans->get("reload samples tooltip") );
    driveLayout->tapeVolume.active.setText( trans->get("Tape") );
    driveLayout->tapeSelection.label.setText( trans->get("Tape Profile") );
    driveLayout->tapeSelection.reload.setText( trans->get("Reload") );
    driveLayout->tapeSelection.reload.setTooltip( trans->get("reload samples tooltip") );
    driveLayout->tapeNoiseVolume.active.setText( trans->get("Tape Noise") );

    int layPos = 0;
    moduleList.setText(layPos++, 0, trans->get("Chip"));
    moduleList.setText(layPos++, 0, trans->get("Drives"));
    

    volumeLayout.info.label.setText( trans->getA("volume") );

    unsigned neededWidth = driveLayout->floppySelection.label.minimumSize().width;
    neededWidth = std::max(neededWidth, driveLayout->tapeSelection.label.minimumSize().width);
    neededWidth = SliderLayout::scale({&driveLayout->floppyVolume, &driveLayout->floppyVolumeExt, &driveLayout->tapeVolume, &driveLayout->tapeNoiseVolume}, "300 %", neededWidth);

    driveLayout->floppySelection.children[ 0 ].size.width = neededWidth;
    driveLayout->tapeSelection.children[ 0 ].size.width = neededWidth;
}

auto AudioLayout::loadSettings() -> void {
    settingsLayout.updateWidgets();

    driveLayout->floppyVolume.active.setChecked( _settings->get<bool>( "audio_floppy", false) );

    unsigned floppyVolume = _settings->get<unsigned>("audio_floppy_volume", 200u, {0u, 300u});

    driveLayout->floppyVolume.slider.setPosition( floppyVolume );

    driveLayout->floppyVolume.value.setText(std::to_string(floppyVolume) + " %");

    unsigned floppyVolumeExt = _settings->get<unsigned>("audio_floppy_volume_external", 200u, {0u, 300u});

    driveLayout->floppyVolumeExt.slider.setPosition( floppyVolumeExt );

    driveLayout->floppyVolumeExt.value.setText(std::to_string(floppyVolumeExt) + " %");

    std::string folder = audioManager->drive.getFloppyFolder(emulator, false);;

    driveLayout->floppySelection.combo.setSelectionByText( folder );

    folder = driveLayout->floppySelection.combo.text();

    _settings->set<std::string>(audioManager->drive.getFloppyFolderIdent(emulator, false), folder);

    folder = audioManager->drive.getFloppyFolder(emulator, true);

    driveLayout->floppySelection.comboExt.setSelectionByText( folder );

    folder = driveLayout->floppySelection.comboExt.text();

    _settings->set<std::string>(audioManager->drive.getFloppyFolderIdent(emulator, true), folder);

    driveLayout->tapeVolume.active.setChecked( _settings->get<bool>( "audio_tape", false) );

    unsigned tapeVolume = _settings->get<unsigned>("audio_tape_volume", 100u, {0u, 300u});

    driveLayout->tapeVolume.slider.setPosition( tapeVolume );

    driveLayout->tapeVolume.value.setText(std::to_string(tapeVolume) + " %");

    folder = _settings->get<std::string>("audio_tape_folder", "");

    driveLayout->tapeSelection.combo.setSelectionByText( folder );

    folder = driveLayout->tapeSelection.combo.text();

    _settings->set<std::string>("audio_tape_folder", folder);

    driveLayout->tapeNoiseVolume.active.setChecked( _settings->get<bool>( "audio_tape_noise", false) );

    unsigned tapeNoiseVolume = _settings->get<unsigned>("audio_tape_noise_volume", 100u, {0u, 300u});

    driveLayout->tapeNoiseVolume.slider.setPosition( tapeNoiseVolume );

    driveLayout->tapeNoiseVolume.value.setText(std::to_string(tapeNoiseVolume) + " %");

    updateVolumeSlider();

}

auto AudioModelLayout::lineWillAppend( unsigned pos ) -> void {
    if ((lines.size() != 5) || !dynamic_cast<LIBC64::Interface*>(this->emulator))
        return;

    update( *lines[lines.size() - 1], 10 );

    append(controlLayout, {~0u, 0u});

    controlLayout.firstAll.onToggle = [this](bool checked) {
        if (!checked)
            return;

        std::vector blocks = {
            getBlock(LIBC64::Interface::ModelIdSid ), getBlock(LIBC64::Interface::ModelIdSid2),
            getBlock(LIBC64::Interface::ModelIdSid3), getBlock(LIBC64::Interface::ModelIdSid4),
            getBlock(LIBC64::Interface::ModelIdSid5), getBlock(LIBC64::Interface::ModelIdSid6),
            getBlock(LIBC64::Interface::ModelIdSid7), getBlock(LIBC64::Interface::ModelIdSid8),
        };

        for (auto block : blocks) {
            if (block)
                block->options[0]->activate();
        }

        controlLayout.secondAll.setChecked( false );
    };

    controlLayout.secondAll.onToggle = [this](bool checked) {
        if (!checked)
            return;

        std::vector blocks = {
            getBlock(LIBC64::Interface::ModelIdSid ), getBlock(LIBC64::Interface::ModelIdSid2),
            getBlock(LIBC64::Interface::ModelIdSid3), getBlock(LIBC64::Interface::ModelIdSid4),
            getBlock(LIBC64::Interface::ModelIdSid5), getBlock(LIBC64::Interface::ModelIdSid6),
            getBlock(LIBC64::Interface::ModelIdSid7), getBlock(LIBC64::Interface::ModelIdSid8),
        };

        for (auto block : blocks) {
            if (block)
                block->options[1]->activate();
        }

        controlLayout.firstAll.setChecked( false );
    };
}

auto AudioModelLayout::updated( Line::Block* block, Emulator::Interface::Model* model ) -> void {
    if (dynamic_cast<LIBC64::Interface*> (this->emulator)) {
        switch(model->id) {
            case LIBC64::Interface::ModelIdSidEngine:
                updateBiasVisibillity();
                break;
            case LIBC64::Interface::ModelIdSidMulti:
                updateExtraSidVisibillity( );
            case LIBC64::Interface::ModelIdSid1Left:
            case LIBC64::Interface::ModelIdSid1Right:
            case LIBC64::Interface::ModelIdSid2Left:
            case LIBC64::Interface::ModelIdSid2Right:
            case LIBC64::Interface::ModelIdSid3Left:
            case LIBC64::Interface::ModelIdSid3Right:
            case LIBC64::Interface::ModelIdSid4Left:
            case LIBC64::Interface::ModelIdSid4Right:
            case LIBC64::Interface::ModelIdSid5Left:
            case LIBC64::Interface::ModelIdSid5Right:
            case LIBC64::Interface::ModelIdSid6Left:
            case LIBC64::Interface::ModelIdSid6Right:
            case LIBC64::Interface::ModelIdSid7Left:
            case LIBC64::Interface::ModelIdSid7Right:
            case LIBC64::Interface::ModelIdSid8Left:
            case LIBC64::Interface::ModelIdSid8Right:
                if (activeEmulator)
                    audioManager->power();
                break;

            case LIBC64::Interface::ModelIdSid: {
                if (tabWindow->systemLayout)
                    tabWindow->systemLayout->systemModelLayout.updateWidget(LIBC64::Interface::ModelIdSid);
            } break;

            case LIBC64::Interface::ModelIdSidSampleFetch:
                audioManager->setResampler();
                break;
        }
    }
}

auto AudioModelLayout::updateVisibillity( ) -> void {
    updateExtraSidVisibillity();
    updateBiasVisibillity();
}

auto AudioModelLayout::updateBiasVisibillity() -> void {
    if (!dynamic_cast<LIBC64::Interface*>(this->emulator))
        return;

    int filter = emulator->getModelValue( LIBC64::Interface::ModelIdSidEngine );

    bool showBias = filter == 0 || filter == 1;
    bool showRange = filter == 1;
    bool showStrength = filter == 1;

    lines[0]->blocks[3]->setEnabled(showStrength);

    if (lines[1]->enabled() != showBias )
        lines[1]->setEnabled( showBias );

    if (lines[2]->enabled() != showRange )
        lines[2]->setEnabled( showRange );

    if (lines[3]->enabled() != showBias )
        lines[3]->setEnabled( showBias );
}

auto AudioModelLayout::updateExtraSidVisibillity( ) -> void {
    if (!dynamic_cast<LIBC64::Interface*>(this->emulator))
        return;

    static int activeSidsNow = -1;

    int activeSids = emulator->getModelValue( LIBC64::Interface::ModelIdSidMulti );

    if (controlLayout.firstAll.checked())
        controlLayout.firstAll.setChecked(false);

    if (controlLayout.secondAll.checked())
        controlLayout.secondAll.setChecked(false);

    if (activeSidsNow == activeSids)
        return;

    activeSidsNow = activeSids;

    for (unsigned i = 0; i < 8; i++) {

        if (i <= activeSids)
            lines[i + 5]->setEnabled(true);
        else
            lines[i + 5]->setEnabled(false);
    }

    if (activeSids == 0) {
        controlLayout.label.setEnabled( false );
        controlLayout.firstAll.setEnabled( false );
        controlLayout.secondAll.setEnabled( false );

        lines[5]->blocks[1]->setEnabled(false);
        lines[5]->blocks[2]->setEnabled(false);
        lines[5]->blocks[3]->setEnabled(false);
    } else
        controlLayout.setEnabled( true );
}

auto AudioModelLayout::getIdent( Emulator::Interface::Model* model, std::string& tooltip ) -> std::string {
    std::string name = model->name;

    if (dynamic_cast<LIBC64::Interface*>(emulator)) {
        switch (model->id) {
            case LIBC64::Interface::ModelIdSid:
            case LIBC64::Interface::ModelIdSid2:
            case LIBC64::Interface::ModelIdSid3:
            case LIBC64::Interface::ModelIdSid4:
            case LIBC64::Interface::ModelIdSid5:
            case LIBC64::Interface::ModelIdSid6:
            case LIBC64::Interface::ModelIdSid7:
            case LIBC64::Interface::ModelIdSid8:
                tooltip = "SID tooltip";
                break;

            case LIBC64::Interface::ModelIdSid1Adr:
            case LIBC64::Interface::ModelIdSid2Adr:
            case LIBC64::Interface::ModelIdSid3Adr:
            case LIBC64::Interface::ModelIdSid4Adr:
            case LIBC64::Interface::ModelIdSid5Adr:
            case LIBC64::Interface::ModelIdSid6Adr:
            case LIBC64::Interface::ModelIdSid7Adr:
            case LIBC64::Interface::ModelIdSid8Adr:
                name = "Address";
                tooltip = name + " tooltip";
                break;

            case LIBC64::Interface::ModelIdSid1Left:
            case LIBC64::Interface::ModelIdSid2Left:
            case LIBC64::Interface::ModelIdSid3Left:
            case LIBC64::Interface::ModelIdSid4Left:
            case LIBC64::Interface::ModelIdSid5Left:
            case LIBC64::Interface::ModelIdSid6Left:
            case LIBC64::Interface::ModelIdSid7Left:
            case LIBC64::Interface::ModelIdSid8Left:
                name = "Left Channel";
                tooltip = name + " tooltip";
                break;

            case LIBC64::Interface::ModelIdSid1Right:
            case LIBC64::Interface::ModelIdSid2Right:
            case LIBC64::Interface::ModelIdSid3Right:
            case LIBC64::Interface::ModelIdSid4Right:
            case LIBC64::Interface::ModelIdSid5Right:
            case LIBC64::Interface::ModelIdSid6Right:
            case LIBC64::Interface::ModelIdSid7Right:
            case LIBC64::Interface::ModelIdSid8Right:
                name = "Right Channel";
                tooltip = name + " tooltip";
                break;

            default:
                tooltip = name + " tooltip";
                break;
        }
    } else
        return ModelLayout::getIdent(model, tooltip);

    return name;
}

}
