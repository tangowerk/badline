
#include "manager.h"
#include "../view/status.h"
#include "../window/sdlWindow.h"
#include "../audio/manager.h"
#include "../thread/emuThread.h"
#include "../media/fileloader.h"
#include "../media/autoloader.h"
#include "../helper/miscHelper.h"

std::vector<InputMapping*> InputManager::hotkeyTriggers;

auto InputManager::setHotkeys() -> void {
    hotkeys.push_back( {Hotkey::Id::Pause, "Pause"} );
    hotkeys.push_back({ Hotkey::Id::Rewind, "rewind" });
    hotkeys.push_back( {Hotkey::Id::Fullscreen, "Fullscreen"} );
    hotkeys.push_back( {Hotkey::Id::ToggleWarp, "Toggle Warp"} );
    hotkeys.push_back( {Hotkey::Id::ToggleWarpAggressive, "Toggle Warp Aggressive"} );
    hotkeys.push_back( {Hotkey::Id::ToggleFastForward, "Toggle Fast Forward"} );
    
    hotkeys.push_back( {Hotkey::Id::ToggleMenu, "Toggle_menu"} );
    hotkeys.push_back( {Hotkey::Id::ToggleStatus, "Toggle_status"} );	
	
    hotkeys.push_back( {Hotkey::Id::RunAheadUp, "runahead up"} );	
    hotkeys.push_back( {Hotkey::Id::RunAheadDown, "runahead down"} );	
    hotkeys.push_back( {Hotkey::Id::RunAheadToggleMode, "runahead toggle mode"} );	
    
    hotkeys.push_back( {Hotkey::Id::ToggleCycleRenderer, "Toggle Cycle renderer"} );	

    hotkeys.push_back( {Hotkey::Id::Freeze, "freeze button"} );
    hotkeys.push_back( {Hotkey::Id::Rotation, "image rotation"} );

    hotkeys.push_back( {Hotkey::Id::SyncStatus, "Sync status"} );
    hotkeys.push_back( {Hotkey::Id::Quit, "exit"} );

    //hotkeys.push_back( {Hotkey::Id::ToggleSCVideo, "toggle S/C-Video"} );
    hotkeys.push_back( {Hotkey::Id::ToggleShader, "toggle Shader"} );
    hotkeys.push_back( {Hotkey::Id::ToggleBorder, "toggle border"} );
    hotkeys.push_back( {Hotkey::Id::ToggleBorderPrev, "toggle border prev"} );
    hotkeys.push_back( {Hotkey::Id::ToggleScaling, "toggle scaling"} );
    hotkeys.push_back( {Hotkey::Id::ToggleFPS, "show_fps"} );
    hotkeys.push_back( {Hotkey::Id::ToggleAudioStats, "show_audio_buffer"} );
    hotkeys.push_back( {Hotkey::Id::ApplyWindowSize, "apply window size"} );
    hotkeys.push_back( {Hotkey::Id::CropWindow, "crop window"} );
    hotkeys.push_back({ Hotkey::Id::TakeScreenShot, "take screenshot" });

    hotkeys.push_back( {Hotkey::Id::FloppyAccess, "select_disk_drive"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwapUp, "swapper up"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwapDown, "swapper down"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap0, "swap media0"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap1, "swap media1"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap2, "swap media2"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap3, "swap media3"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap4, "swap media4"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap5, "swap media5"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap6, "swap media6"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap7, "swap media7"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap8, "swap media8"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap9, "swap media9"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap10, "swap media10"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap11, "swap media11"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap12, "swap media12"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap13, "swap media13"} );
    hotkeys.push_back( {Hotkey::Id::DiskSwap14, "swap media14"} );

    hotkeys.push_back( {Hotkey::Id::DebuggerToggle, "debugger toggle"} );
    hotkeys.push_back( {Hotkey::Id::DebuggerStepInto, "debugger step into"} );
    hotkeys.push_back( {Hotkey::Id::DebuggerStepOver, "debugger step over"} );
    hotkeys.push_back( {Hotkey::Id::DebuggerStepOut, "debugger step out"} );
    hotkeys.push_back( {Hotkey::Id::DebuggerLine, "debugger step line"} );
    hotkeys.push_back( {Hotkey::Id::DebuggerFrame, "debugger step frame"} );

    // not assignable, not savable
    hiddenHotkeys.push_back( {Hotkey::Id::Warp, ""} );
    hiddenHotkeys.push_back( {Hotkey::Id::WarpOff, ""} );
}

auto InputManager::setCustomHotkeys() -> void {

    customHotkeys.push_back( {Hotkey::Id::CaptureMouse, "Capture_mouse", false} );
	customHotkeys.push_back( {Hotkey::Id::Loadstate, "Loadstate", true} );
	customHotkeys.push_back( {Hotkey::Id::Savestate, "Savestate", true} );
	customHotkeys.push_back( {Hotkey::Id::IncSlot, "Incslot", true} );
    customHotkeys.push_back( {Hotkey::Id::DecSlot, "Decslot", true} );
	customHotkeys.push_back( {Hotkey::Id::Power, "Hard Reset", true} );
    if (dynamic_cast<LIBC64::Interface*>(emulator) )
        customHotkeys.push_back( {Hotkey::Id::PowerWithUnplugCart, "Hard Reset + Unplug Cart", false} );

	customHotkeys.push_back( {Hotkey::Id::SoftReset, "Soft Reset", true} );
    customHotkeys.push_back( {Hotkey::Id::AnyLoad, "load software", true} );

	if (dynamic_cast<LIBC64::Interface*>(emulator) ) {
	    customHotkeys.push_back( {Hotkey::Id::SwapPortDevices, "swap Ports", false} );
		customHotkeys.push_back( {Hotkey::Id::ToggleSidFilter, "sid_filter_toggle", false} );
		customHotkeys.push_back( {Hotkey::Id::SwapSid, "Swap_sid", false} );
		customHotkeys.push_back( {Hotkey::Id::DigiBoost, "Digi_boost", false} );	
		customHotkeys.push_back( {Hotkey::Id::AdjustSidCurveUp, "adjust_curve_up", false} );
		customHotkeys.push_back( {Hotkey::Id::AdjustSidCurveDown, "adjust_curve_down", false} );
		
		customHotkeys.push_back( {Hotkey::Id::PlayTape, "tape_play_key", false} );
		customHotkeys.push_back( {Hotkey::Id::StopTape, "tape_stop_key", false} );
		customHotkeys.push_back( {Hotkey::Id::RecordTape, "tape_record_key", false} );
		customHotkeys.push_back( {Hotkey::Id::ForwardTape, "tape_forward_key", false} );
		customHotkeys.push_back( {Hotkey::Id::RewindTape, "tape_rewind_key", false} );
		customHotkeys.push_back( {Hotkey::Id::ResetTapeCounter, "tape_counter_reset_key", false} );
        customHotkeys.push_back( {Hotkey::Id::EF3Menu, "ef3 menu button", false} );
        customHotkeys.push_back( {Hotkey::Id::Toggle2MHzCpuTurbo, "toggle 2 MHz Turbo", false });
	    customHotkeys.push_back( {Hotkey::Id::PrioDoubleAssignment, "Allow Keyset Joystick", false });
	}
    
	customHotkeys.push_back( {Hotkey::Id::Software, "Software", true} );	
    customHotkeys.push_back( {Hotkey::Id::System, "System", true} );
	customHotkeys.push_back( {Hotkey::Id::Control, "Control", true} );
	customHotkeys.push_back( {Hotkey::Id::Configurations, "Configurations", true} );
	customHotkeys.push_back( {Hotkey::Id::Presentation, "Presentation", true} );
	customHotkeys.push_back( {Hotkey::Id::Palette, "Palette", true} );
    customHotkeys.push_back( {Hotkey::Id::Firmware, "Firmware", true} );
    customHotkeys.push_back( {Hotkey::Id::Audio, "Audio", true} );
	customHotkeys.push_back( {Hotkey::Id::Geometry, "Geometry", true} );
    customHotkeys.push_back( {Hotkey::Id::Misc, "Miscellaneous", true} );
    customHotkeys.push_back( {Hotkey::Id::DiskSwapper, "swapper", true} );
    customHotkeys.push_back( {Hotkey::Id::AutoStart, "autostart media", true} );
}

auto InputManager::fireHotkey(InputMapping* trigger) -> void {
    Emulator::Interface* emulator = trigger->inputManager ? trigger->inputManager->emulator : nullptr;
    if (emulator && !globalSettings->get<bool>("core_" + emulator->ident, true))
        return;

    Hotkey::Id id = (Hotkey::Id)trigger->hotkeyId;

    typedef LIBC64::Interface C64Interface;
    
    auto settings = Program::getSettings( activeEmulator );
    
    switch ( id ) {
        case Hotkey::Id::Rotation: {
            emuThread->lock();
            DRIVER::Rotation rotation = videoDriver->getRotation();

            if (rotation == DRIVER::ROT_0) rotation = DRIVER::ROT_90;
            else if (rotation == DRIVER::ROT_90) rotation = DRIVER::ROT_180;
            else if (rotation == DRIVER::ROT_180) rotation = DRIVER::ROT_270;
            else if (rotation == DRIVER::ROT_270) rotation = DRIVER::ROT_0;

            settings->set<unsigned>("rotation", (unsigned)rotation);
            videoDriver->setRotation(rotation);

        } break;

        case Hotkey::Id::RunAheadDown:
        case Hotkey::Id::RunAheadUp: {
            if (!activeEmulator)
                break;   
            
            unsigned pos = settings->get<unsigned>( "runahead", 0, {0u, 10u});
            bool down = id == Hotkey::Id::RunAheadDown;
            
            if ( down && (pos == 0) )
                break;
            else if ( !down && (pos == 10) )
                break;

            pos += down ? -1 : 1;
            settings->set<unsigned>( "runahead", pos);
            emuThread->lock();
            audioManager->drive.reset();
            activeEmulator->runAhead( pos );
            emuThread->unlock();

            statusHandler->setMessage( GUIKIT::String::substitute("RunAhead adjusts input latency for %count% picture(s).", {{"%count%", std::to_string(pos) }}), false, true );

        } break;
            
        case Hotkey::Id::ToggleCycleRenderer: {
            if (!activeEmulator)
                break;

            auto model = activeEmulator->getModel( activeEmulator->getModelIdOfCycleRenderer() );
            if (model) {
                emuThread->lock(true);
                bool val = activeEmulator->getModelValue( model->id );
                settings->set<bool>( _underscore(model->name), !val );
                activeEmulator->setModelValue( model->id, !val );
                program->setWarp(Program::Warp::Off);
                program->power(activeEmulator);
            }
        } break;
        
        case Hotkey::Id::RunAheadToggleMode: {
            if (!activeEmulator)
                break;
            
            bool state = settings->get<bool>( "runahead_performance", false);
            state ^= 1;
            settings->set<bool>( "runahead_performance", state);
            emuThread->lock();
            activeEmulator->runAheadPerformance( state );
            emuThread->unlock();

            
            statusHandler->setMessage( (!state ? "RunAhead uses the accurate mode" : "RunAhead uses the performance mode."), false, true );
        } break;

		case Hotkey::Id::SwapPortDevices: {
		    if (activeEmulator != emulator)
		        break;

            emuThread->lock();
            auto settings = Program::getSettings( emulator );
			auto connector1 = emulator->getConnector( 0 );
            auto connectedDevice1 = emulator->getConnectedDevice( connector1 );
            
            auto connector2 = emulator->getConnector( 1 );
            auto connectedDevice2 = emulator->getConnectedDevice( connector2 );

            emulator->connect( connector1, connectedDevice2 );
            emulator->connect( connector2, connectedDevice1 );

            settings->set<unsigned>( _underscore(connector1->name), connectedDevice2->id);
            settings->set<unsigned>( _underscore(connector2->name), connectedDevice1->id);


		} break;
        case Hotkey::Id::ToggleWarp:
            emuThread->lock();
            program->toggleWarp( Program::Warp::Normal );
            break;

        case Hotkey::Id::ToggleWarpAggressive:
            emuThread->lock();
            program->toggleWarp( Program::Warp::Aggressive );
            break;

        case Hotkey::Id::ToggleFastForward:
            emuThread->lock();
            program->toggleWarp( Program::Warp::FastForward );
            break;

        case Hotkey::Id::Warp:
            if (program->warp.mode != program->warp.autoMode) {
                emuThread->lock();
                program->setWarp( program->warp.autoMode );
            }
            break;

        case Hotkey::Id::WarpOff:
            if (program->warp.mode != Program::Warp::Off) {
                emuThread->lock();
                program->setWarp(Program::Warp::Off);
            }
            break;

        case Hotkey::Id::Fullscreen:
            if (sdlWindow)
                sdlWindow->setFullScreen( !sdlWindow->fullScreen() );
            break;

        case Hotkey::Id::Quit: {
            auto tempTimer = new GUIKIT::Timer;
            tempTimer->setInterval(5);
            tempTimer->onFinished = [tempTimer]() {
                tempTimer->setEnabled(false);
                if (!program->quitInProgress) {
                    program->quit();
                    GUIKIT::Application::quit();
                }
            };
            tempTimer->setEnabled();
        } break;
			
		case Hotkey::Power:
            emuThread->lock(true);
			program->power(emulator);
			break;

        case Hotkey::PowerWithUnplugCart:
            emuThread->lock(true);
            MiscHelper::removeExpansion(emulator);
            program->power(emulator);
            break;

			
		case Hotkey::SoftReset:
            emuThread->lock(true);
			program->reset(emulator);
			break;

        case Hotkey::ToggleFPS: {
            if (statusHandler) {
                statusHandler->showFPSScreen ^= 1;
                if (!statusHandler->showFPSScreen)
                    showScreenText("", 0 );
                else
                    statusHandler->setFpsCounterUpdate();

                auto fpsScreen = globalSettings->get<unsigned>("fps_screen", 0);

                if (statusHandler->showFPSScreen)
                    fpsScreen |= 1 << (sdlWindow && sdlWindow->fullScreen() ? 1 : 0);
                else
                    fpsScreen &= ~(1 << (sdlWindow && sdlWindow->fullScreen() ? 1 : 0));

                globalSettings->set<unsigned>("fps_screen", fpsScreen);
            }
        } break;

        case Hotkey::ToggleAudioStats: {
            if (audioManager) {
                audioManager->statistics.enable ^= 1;
                if (!audioManager->statistics.enable)
                    showScreenText("", 0 );
                else
                    statusHandler->setDrcBufferUpdate();
            }
        } break;
			
        case Hotkey::AnyLoad:
            break;
            
        case Hotkey::Id::CaptureMouse:
            // the SDL window applies the capture state on the UI thread
            if (inputDriver->mIsAcquired())
                inputDriver->mUnacquire();
            else if (!program->isPause)
                inputDriver->mAcquire();
            break;
        case Hotkey::Id::DiskSwapper:
        case Hotkey::Id::Software:
        case Hotkey::Id::Presentation:
        case Hotkey::Id::Palette:
        case Hotkey::Id::Geometry:
        case Hotkey::Id::Firmware:
        case Hotkey::Id::System:
        case Hotkey::Id::Control:
		case Hotkey::Id::Configurations:
        case Hotkey::Id::Audio:
        case Hotkey::Id::Misc:
            // opened GTK settings windows; overlay pages replace them
            break;
        case Hotkey::Id::SyncStatus: {
            std::string _str = VideoManager::pacingStateName();
            if (VideoManager::pacing.repeat > 1)
                _str += " (" + std::to_string(VideoManager::pacing.repeat) + "x)";
            _str += "  ";
            _str += audioManager->dynamicRateControl ? "DRC  " : "";
            float monitorFrequency = (sdlWindow ? sdlWindow->refreshRate() : 0.0f);
            _str += GUIKIT::String::convertDoubleToString(monitorFrequency, 3 );
            emuThread->lock();
            if (statusHandler)
                statusHandler->setMessage(_str, false, true, 4);
        } break;
        case Hotkey::Id::TakeScreenShot:
            emuThread->lock();
            program->requestScreenshot();
            break;
        case Hotkey::Id::ToggleShader:
        case Hotkey::Id::ToggleSCVideo: {
            if (!activeEmulator)
                break;

            emuThread->lock();
            bool state = VideoManager::getInstance( activeEmulator )->toggleShaderTemporary();
            if (statusHandler)
                statusHandler->setMessage( (state ? "off" : "Shader"), false, true );

        } break;
        case Hotkey::Id::Pause:
            program->togglePause();
            break;
        case Hotkey::IncSlot:
        case Hotkey::DecSlot:
            if (emulator == activeEmulator) {
                emuThread->lock();
                States::getInstance( activeEmulator )->changeSlot( id == Hotkey::DecSlot );
            }
            break;
        case Hotkey::Loadstate:
            emuThread->lock(true);
            States::getInstance( emulator )->load();
            break;
        case Hotkey::Savestate:
            if (emulator == activeEmulator) {
                emuThread->lock(true);
                States::getInstance( activeEmulator )->save();
            }
            break;
        case Hotkey::ToggleMenu:
            break;
        case Hotkey::ToggleStatus:
            break; // no status bar (HUD milestone)

        case Hotkey::ApplyWindowSize:
            break; // GTK window geometry; the SDL window size follows the compositor

        case Hotkey::CropWindow:
            break;

        case Hotkey::Id::ToggleScaling: {
            auto hotkeyState = settings->get<unsigned>( "scale_hotkey", program->getScaleHotkeyDefault() );
            int aspectMode = settings->get<int>("aspect_mode", 1, {0, 3});
            auto aspectModeOld = aspectMode;

            do {
                aspectMode++;
                if (aspectMode > 3)
                    aspectMode = 0;

                if (hotkeyState & (1 << aspectMode))
                    break;

            } while (aspectMode != aspectModeOld);

            if (aspectMode == aspectModeOld)
                break;

            emuThread->lock();
            settings->set<unsigned>("aspect_mode", aspectMode);
            program->setVideoDimension(activeEmulator);
            statusHandler->setMessage( program->getScaleMessage(activeEmulator, aspectMode), false, true );
        } break;

        case Hotkey::Id::ToggleBorder:
        case Hotkey::Id::ToggleBorderPrev: {
            bool prev = id == Hotkey::Id::ToggleBorderPrev;
            typedef Emulator::Interface::CropType CropType;
            int cropType = settings->get<int>("crop_type", (unsigned)CropType::Monitor, {0u, 11u});
            auto hotkeyState = settings->get<unsigned>( "border_hotkey", program->getCropHotkeyDefault() );
            auto cropTypeOld = cropType;

            do {
                cropType += prev ? -1 : 1;
                if (cropType > 11)
                    cropType = 0;
                else if (cropType < 0)
                    cropType = 11;

                if (hotkeyState & (1 << cropType))
                    break;

            } while (cropType != cropTypeOld);

            if (cropType == cropTypeOld)
                break;


            settings->set<unsigned>("crop_type", cropType);
            emuThread->lock();
            program->updateCrop(activeEmulator);
            statusHandler->setMessage(program->getCropMessage(activeEmulator, (CropType)cropType), false, true);
        } break;

        case Hotkey::ResetTapeCounter:
        case Hotkey::PlayTape:
        case Hotkey::RecordTape:
        case Hotkey::StopTape:
        case Hotkey::ForwardTape:
        case Hotkey::RewindTape: {
            if (!activeEmulator || !dynamic_cast<LIBC64::Interface*>(activeEmulator) )
                break;

            program->hintAutoWarp(0);

            emuThread->lock();
            auto media = activeEmulator->getTape( 0 );
            if (!media)
                break;

            unsigned driveCount = activeEmulator->getModelValue( activeEmulator->getModelIdOfEnabledDrives( media->group ) );

            if (driveCount == 0) {
                statusHandler->setMessage( "There is no cassette drive connected!", true );
                break;
            }                        

            typedef Emulator::Interface::TapeMode TapeMode;

            if (id == Hotkey::PlayTape) {
                activeEmulator->controlTape( media, TapeMode::Play );
                statusHandler->setMessage("Datasette play button was pressed", false, true);
            } else if (id == Hotkey::StopTape) {
                activeEmulator->controlTape( media, TapeMode::Stop );
                statusHandler->setMessage("Datasette stop button was pressed", false, true);
            } else if (id == Hotkey::RecordTape) {              
                activeEmulator->controlTape( media, TapeMode::Record );
                statusHandler->setMessage("Datasette record button was pressed", false, true);
                if (activeEmulator->isWriteProtected( media ))
                    statusHandler->setMessage( "Cassette is write protected", true );						

            } else if (id == Hotkey::ForwardTape) {
                activeEmulator->controlTape( media, TapeMode::Forward );
                //status->addMessage( "Datasette forward button was pressed" );                        
            } else if (id == Hotkey::RewindTape) {
                activeEmulator->controlTape( media, TapeMode::Rewind );
                //status->addMessage( "Datasette rewind button was pressed" );
            } else if (id == Hotkey::ResetTapeCounter) {
                activeEmulator->controlTape( media, TapeMode::ResetCounter );
                statusHandler->setMessage("Datasette counter was reseted", false, true);
            } 															
            break;
        }
        case Hotkey::Id::DigiBoost: {
            if (!activeEmulator || !dynamic_cast<LIBC64::Interface*>(activeEmulator))
                break;

            bool state = false;

            emuThread->lock();
            auto model = activeEmulator->getModel( C64Interface::ModelIdDigiboost );
            if (model) {
                state = activeEmulator->getModelValue( model->id );
                state ^= 1;
                settings->set<bool>( _underscore(model->name), state );
                activeEmulator->setModelValue( model->id, state );
            }

            statusHandler->setMessage((state ? "8580 digi boost is active" : "8580 digi boost is inactive"), false, true);
        } break;
        case Hotkey::Id::SwapSid: {
            if (!activeEmulator || !dynamic_cast<LIBC64::Interface*>(activeEmulator))
                break;
            unsigned val = 1;

            emuThread->lock();
            auto model = activeEmulator->getModel( C64Interface::ModelIdSid );
            if (model) {
                val = activeEmulator->getModelValue( model->id );
                val++;
                if (val == model->options.size())
                    val = 0;
                settings->set<int>( _underscore(model->name), val );
                activeEmulator->setModelValue( model->id, val );
            }


            statusHandler->setMessage((val == 1 ? "SID 6581 is emulated" : "SID 8580 is emulated"), false, true);
        } break;
        case Hotkey::Id::ToggleSidFilter: {
            if (!activeEmulator || !dynamic_cast<LIBC64::Interface*>(activeEmulator))
                break;
            bool state = false;

            emuThread->lock();
            auto model = activeEmulator->getModel( C64Interface::ModelIdFilter );
            if (model) {
                state = activeEmulator->getModelValue( model->id );
                state ^= 1;
                settings->set<bool>( _underscore(model->name), state );
                activeEmulator->setModelValue( model->id, state );
            }
            statusHandler->setMessage((state ? "SID-Filter is emulated" : "SID-Filter is deactivated"), false, true);
        } break;
        case Hotkey::AdjustSidCurveUp:
        case Hotkey::AdjustSidCurveDown: {
            if (!activeEmulator || !dynamic_cast<LIBC64::Interface*>(activeEmulator))
                break;
            
            int _sid = activeEmulator->getModelValue( C64Interface::ModelIdSid );
            int state = 0;

            emuThread->lock();
            auto model = activeEmulator->getModel( _sid == 0 ? C64Interface::ModelIdBias8580 : C64Interface::ModelIdBias6581);

            if (model) {
                state = activeEmulator->getModelValue( model->id );
                state += id == Hotkey::AdjustSidCurveUp ? 100: -100;
                state = std::max( model->range[0], std::min( state, model->range[1] ) );
                settings->set<int>( _underscore(model->name), state );
                activeEmulator->setModelValue( model->id, state );
            }
            statusHandler->setMessage(GUIKIT::String::substitute("SID curve: %state%", { {"%state%", GUIKIT::String::formatFloatingPoint( double(state) / 10000.0 ) } }), false, true);
        } break;
        
        case Hotkey::Id::FloppyAccess: {
            if (!activeEmulator)
                break;

            emuThread->lock();
            auto mediaId = settings->get<unsigned>("access_floppy", 0u, {0u, 3u});
            mediaId++;
            auto media = activeEmulator->getEnabledDisk(mediaId);
            if (!media)
                break;

            settings->set<unsigned>( "access_floppy", media->id, false);
            statusHandler->setMessage(GUIKIT::String::substitute("disk drive %drive% was accessed.", { {"%drive%", media->name} }), false, true);
            break;
        }

        case Hotkey::Freeze:
            emuThread->lock();
            if (activeEmulator)
                activeEmulator->freezeButton();
            break;

        case Hotkey::EF3Menu:
            emuThread->lock();
            if (activeEmulator)
                activeEmulator->customCartridgeButton();
            break;

        case Hotkey::Toggle2MHzCpuTurbo: {
            emuThread->lock();
            MiscHelper::toggle2Mhz();
        } break;

        case Hotkey::PrioDoubleAssignment: {
            emuThread->lock();
            auto prio = settings->get<unsigned>("prioritise_mappings", 1, {0,2});
            prio = prio <= 1 ? 2 : 1;
            settings->set<unsigned>("prioritise_mappings", prio);
            trigger->inputManager->updateMiscSettings();
            trigger->inputManager->updateMappingsInUse();
            statusHandler->setMessage((prio == 2 ? "prioritise Keyboard" : "prioritise Controlport"), false, true, 4);
        } break;

        case Hotkey::AutoStart: {
            emuThread->lock(true);
            bool trapped;
            unsigned selection;
            auto media = autoloader->get(emulator, trapped, selection);
            if (!media)
                break;

            auto fSetting = FileSetting::getInstance( emulator, _underscore(media->name ) );
            if (fSetting->path.empty())
                break;

            fileloader->autoload(emulator, media, selection, trapped );
        } break;
        
        case Hotkey::DiskSwap0: case Hotkey::DiskSwap1: case Hotkey::DiskSwap2:
        case Hotkey::DiskSwap3: case Hotkey::DiskSwap4: case Hotkey::DiskSwap5:
        case Hotkey::DiskSwap6: case Hotkey::DiskSwap7: case Hotkey::DiskSwap8:
        case Hotkey::DiskSwap9: case Hotkey::DiskSwap10: case Hotkey::DiskSwap11:
        case Hotkey::DiskSwap12: case Hotkey::DiskSwap13: case Hotkey::DiskSwap14:
        case Hotkey::DiskSwapUp: case Hotkey::DiskSwapDown: {
            if (!activeEmulator)
                break;

            int swapPos = settings->get<int>("swap_pos", -1);

            if (id == Hotkey::DiskSwapUp) {
                if (swapPos == -1)
                    swapPos = fileloader->getSwapPos(activeEmulator);
                swapPos++;
            } else if (id == Hotkey::DiskSwapDown) {
                if (swapPos == -1)
                    swapPos = fileloader->getSwapPos(activeEmulator);
                swapPos--;
            } else
                swapPos = id - Hotkey::DiskSwap0;

            if (swapPos < 0) swapPos = SWAPPER_SLOTS - 1;
            else if (swapPos >= SWAPPER_SLOTS) swapPos = 0;

            emuThread->lock();
            fileloader->insertSwapDisk(activeEmulator, swapPos);
            break;
        }
        case Hotkey::Autofire: {
            if (!activeEmulator)
                break;
            emuThread->lock();
            auto manager = trigger->inputManager;
            auto mapping = trigger->shadowMap[0];

            manager->toggleAutofire(mapping);

            statusHandler->setMessage((manager->isAutofireActive(mapping) ? "Autofire active" : "Autofire inactive"), false, true, 4);


        } break;

        case Hotkey::Rewind: {
            static GUIKIT::Timer* rewindTimer = nullptr;
            if (!rewindTimer) {
                rewindTimer = new GUIKIT::Timer();
                rewindTimer->setInterval(20);
            }

            rewindTimer->onFinished = [trigger]() {
                auto _t = trigger;
                bool pressed;

                do {
                    bool ored = !_t->anded || (_t->hids.size() <= 1);
                    pressed = !ored;

                    if (ored) {
                        for (auto& hid : _t->hids) {
                            if (hid.input->value) {
                                pressed = true;
                                break;
                            }
                        }
                    } else {
                        for (auto& hid : _t->hids) {
                            if (!hid.input->value) {
                                pressed = false;
                                break;
                            }
                        }
                    }
                    if (pressed)
                        break;

                    _t = _t->alternate;
                } while (_t);

                rewindTimer->setEnabled(pressed);
                if (!pressed) {
                    bool locked = emuThread->lock();
                    audioManager->setRewind(false);
                    activeEmulator->setRewind(false);
                    if (rewindTimer->data())
                        audioManager->volumeAdjust = rewindTimer->dataF();

                    if (locked)
                        emuThread->unlock();
                }
            };

            if (rewindTimer->enabled() || !settings->get<bool>("rewind_enable", false))
                return;

            emuThread->lock();
            if (settings->get<unsigned>("rewind_step", 1, {1, 60}) > 1) {
                rewindTimer->setDataF(audioManager->volumeAdjust);
                rewindTimer->setData(1u);
                audioManager->volumeAdjust = 0.0;
            } else
                rewindTimer->setData(0u);
            rewindTimer->setEnabled();
            audioManager->setRewind(true);
            activeEmulator->setRewind(true);

        } break;

        case Hotkey::DebuggerToggle:
        case Hotkey::DebuggerStepInto:
        case Hotkey::DebuggerStepOver:
        case Hotkey::DebuggerStepOut:
        case Hotkey::DebuggerLine:
        case Hotkey::DebuggerFrame:
            break; // GTK debugger windows are gone (overlay debugger later)
    }
    emuThread->unlock();
}

auto InputManager::pollHotkeys() -> void {

    emuThread->lockHotkeys();
    if (hotkeyTriggers.size() == 0) {
        emuThread->unlockHotkeys();
        return;
    }
    auto _hotkeyTriggers = hotkeyTriggers;
    hotkeyTriggers.clear();
    emuThread->unlockHotkeys();

	std::vector<InputMapping*> useTrigger;
	InputMapping* viewOpen = nullptr;
	InputMapping* warp = nullptr;
    InputMapping* warpAutostart = nullptr;
	InputMapping* stateHandler = nullptr;
	InputMapping* starter = nullptr;
    InputMapping* anyLoad = nullptr;
    InputMapping* lastAutostart = nullptr;
    InputMapping* swapAutofire = nullptr;
	
	auto useEmu = activeEmulator;
    if (!useEmu)
        useEmu = program->getLastUsedEmu();

    for( auto trigger : _hotkeyTriggers ) {
		
		switch(trigger->hotkeyId) {
            case Hotkey::Id::Warp:
            case Hotkey::Id::WarpOff:
                warpAutostart = trigger; // use last event
                break;

			case Hotkey::Id::DiskSwapper:
			case Hotkey::Id::Software:
			case Hotkey::Id::Presentation:
			case Hotkey::Id::Palette:
			case Hotkey::Id::Geometry:
			case Hotkey::Id::Firmware:
			case Hotkey::Id::System:
			case Hotkey::Id::Control:
			case Hotkey::Id::Configurations:
            case Hotkey::Id::Audio:
				if (!viewOpen)
					viewOpen = trigger;				
				
				else if (useEmu == trigger->inputManager->emulator)
					viewOpen = trigger;
				
				break;
				
			case Hotkey::Id::ToggleWarp:
			case Hotkey::Id::ToggleWarpAggressive:
		    case Hotkey::Id::ToggleFastForward:
				if(!warp)
					warp = trigger;
				break;

			case Hotkey::IncSlot:
			case Hotkey::DecSlot:
			case Hotkey::Loadstate:
			case Hotkey::Savestate:
				if (!stateHandler)
					stateHandler = trigger;

				else if (useEmu == trigger->inputManager->emulator)
					stateHandler = trigger;
				break;

			case Hotkey::Power:
			case Hotkey::SoftReset:
				if (!starter)
					starter = trigger;				
				
				else if (useEmu == trigger->inputManager->emulator)
					starter = trigger;
				break;
				
            case Hotkey::AnyLoad:
				if (!anyLoad)
					anyLoad = trigger;				
				
				else if (useEmu == trigger->inputManager->emulator)
					anyLoad = trigger;
				break;

            case Hotkey::AutoStart:
                if (!lastAutostart)
                    lastAutostart = trigger;

                else if (useEmu == trigger->inputManager->emulator)
                    lastAutostart = trigger;
                break;

		    case Hotkey::Autofire:
		        swapAutofire = trigger;
		        break;
                
			default:
				if (!GUIKIT::Vector::find( useTrigger, trigger ))
					useTrigger.push_back( trigger );
				break;			
		}		
	}

    if (!warpAutostart || warp) {
        if (!Program::hasFocus() && !swapAutofire)
            return;
    }
	
	if (viewOpen)
		useTrigger.push_back( viewOpen );

	if(warp)
		useTrigger.push_back( warp );
    else if(warpAutostart)
        useTrigger.push_back( warpAutostart );
	
	if(stateHandler)
		useTrigger.push_back( stateHandler );

    if(lastAutostart)
        useTrigger.push_back( lastAutostart );

	if (starter)
		useTrigger.push_back( starter );
    
    if (anyLoad)
		useTrigger.push_back( anyLoad );

    if (swapAutofire)
        useTrigger.push_back( swapAutofire );

	for( auto trigger : useTrigger )
        fireHotkey( trigger );
}

auto InputManager::activateHotkey(Hotkey::Id id, Emulator::Interface* emulator) -> void {
	
	for (auto manager : inputManagers) {
		if (emulator && emulator != manager->emulator)
			continue;
		
		for( auto mapping : manager->mappings ) {
			
			if (mapping->emuDevice)
				continue;

            if (emulator && !mapping->inputManager)
                continue;

            if (emulator && mapping->inputManager->emulator != emulator)
                continue;

			if (mapping->hotkeyId == id) {
                emuThread->lockHotkeys();
				hotkeyTriggers.push_back( mapping );
                emuThread->unlockHotkeys();
				return;
			}
		}
	}
}

auto InputManager::activateHiddenHotkey(Hotkey::Id id) -> void {
    for (auto& item: hiddenHotkeys) {
        if (item.id == id) {
            emuThread->lockHotkeys();
            hotkeyTriggers.push_back((InputMapping*) item.guid);
            emuThread->unlockHotkeys();
            break;
        }
    }
}

auto InputManager::unmapHotkeys() -> void {
    for(auto& hotkey : hotkeys) {
                
        auto mapping = (InputMapping*)hotkey.guid;
        
        mapping->init();

        if (mapping->alternate)
            mapping->alternate->init();
    }
	
	InputManager::updateAllMappingsInUse();
}

auto InputManager::unmapCustomHotkeys() -> void {
	
	for(auto& hotkey : customHotkeys) {
		
		auto mapping = (InputMapping*)hotkey.guid;
        
        mapping->init();

        if (mapping->alternate)
            mapping->alternate->init();
	}
	// there are emulator specific hotkeys which are active for another emulator cores, like 'load state'
	InputManager::updateAllMappingsInUse(true);
}

