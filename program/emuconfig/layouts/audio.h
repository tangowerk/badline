
#pragma once

#include "../../../guikit/api.h"
#include "../../program.h"
#include "../../config/slider.h"
#include "model.h"

namespace EmuConfigView {

struct TabWindow;

struct AudioDriveLayout : GUIKIT::FramedVerticalLayout {
    struct TapeSelection : GUIKIT::HorizontalLayout {
        GUIKIT::Label label;
        GUIKIT::ComboButton combo;
        GUIKIT::Widget spacer;
        GUIKIT::Button reload;
        TapeSelection();
    } tapeSelection;

    struct FloppySelection : GUIKIT::HorizontalLayout {
        GUIKIT::Label label;
        GUIKIT::ComboButton combo;
        GUIKIT::Label labelExt;
        GUIKIT::ComboButton comboExt;
        GUIKIT::Widget spacer;
        GUIKIT::Button reload;
        FloppySelection();
    } floppySelection;

    SliderLayout floppyVolume;
    SliderLayout floppyVolumeExt;
    SliderLayout tapeVolume;
    SliderLayout tapeNoiseVolume;

    AudioDriveLayout(Emulator::Interface* emulator);
};

struct VolumeControlLayout : GUIKIT::HorizontalLayout {
    GUIKIT::VerticalSlider volumeSlider;

    struct Info : GUIKIT::VerticalLayout {
        GUIKIT::Label label;
        GUIKIT::Label value;

        Info();
    } info;

    VolumeControlLayout();
};

struct AudioModelLayout : ModelLayout {

    struct ControlLayout : GUIKIT::HorizontalLayout {
        GUIKIT::Label label;
        GUIKIT::CheckBox firstAll;
        GUIKIT::CheckBox secondAll;
        GUIKIT::Widget spacer;

        ControlLayout();
    } controlLayout;

    auto lineWillAppend( unsigned pos ) -> void override;
    auto updateVisibillity( ) -> void override;
    auto updated( Line::Block* block, Emulator::Interface::Model* model ) -> void override;

    auto updateBiasVisibillity() -> void;
    auto updateExtraSidVisibillity() -> void;

    auto getIdent( Emulator::Interface::Model* model, std::string& tooltip ) -> std::string override;
};

struct AudioLayout : GUIKIT::HorizontalLayout {
    
    TabWindow* tabWindow;
    Emulator::Interface* emulator;
    
    GUIKIT::Image processorImage;
    GUIKIT::Image driveImage;
    
    GUIKIT::FramedVerticalLayout moduleFrame;
    GUIKIT::ListView moduleList;

    GUIKIT::SwitchLayout moduleSwitch;
    
    AudioModelLayout settingsLayout;
    
    AudioDriveLayout* driveLayout;
    VolumeControlLayout volumeLayout;
    
    AudioLayout(TabWindow* tabWindow);
    
    auto translate() -> void;
    
    auto loadSettings() -> void;
    
    auto updateFloppyProfileList() -> void;
    auto updateTapeProfileList() -> void;


    auto updateVolumeSlider() -> void;
};

}
