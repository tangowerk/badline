
#include "manager.h"
#include "../cmd/cmd.h"

auto InputManager::getManager( Emulator::Interface* emulator ) -> InputManager* {
	
	for (auto manager : inputManagers) {
		if (manager->emulator == emulator)
			return manager;
	}
	return nullptr;
}

auto InputManager::build() -> void {
    setHotkeys();
    setMappings();	
}

auto InputManager::init() -> void {       
	hidDevices = inputDriver->poll();
    assignChangedDeviceState();

    // SDL3 scancode inputs replace the X11 keycode inputs of the GTK era: stored assignments
    // point to other keys, so start once from the defaults. Emulator mappings and custom hotkeys
    // live in the emulator config, shared hotkeys in the global config: each has its own marker.
    if (!cmd->noGui) {
        // bump when the keyboard input list changes (SDL3b: letters follow the active layout)
        static const std::string marker = "SDL3b";
        auto clear = [](InputMapping* mapping) {
            for (auto m = mapping; m; m = m->alternate) {
                m->hids.clear();
                if (m->setting)
                    m->setting->value = "";
            }
        };

        if (globalSettings->get<std::string>("input_mapping_driver", "") != marker) {
            for (auto& hotkey : hotkeys)
                if (hotkey.guid)
                    clear((InputMapping*)hotkey.guid);
            assignDefaultHotkeys();
            globalSettings->set<std::string>("input_mapping_driver", marker);
        }

        for (auto manager : inputManagers) {
            if (!manager->emulator)
                continue;
            auto settings = Program::getSettings( manager->emulator );
            if (settings->get<std::string>("input_mapping_driver", "") == marker)
                continue;
            for (auto mapping : manager->mappings)
                if (mapping->inputManager == manager)
                    clear(mapping);
            settings->set<bool>( "automapped", false );
            settings->set<std::string>("input_mapping_driver", marker);
        }
    }
	
    for (auto manager : inputManagers) {
        
        if (!manager->emulator)
            continue;

        manager->autoAssignHotkeys();
        
        manager->updateAnalogSensitivity();

        manager->updateAutofireFrequency();

        manager->updateMiscSettings();
        
        auto settings = Program::getSettings( manager->emulator );
        
        auto alreadyMapped = settings->get<bool>( "automapped", false);    
        
        if (alreadyMapped)
            continue;
        
        manager->autoAssign( assumeLayoutType(), false );	                    
        
        settings->set<bool>( "automapped", true );
    }           

	bindHidsGlobal();
    autoAssignJoypads();
    
    if (activeInputManager)
        activeInputManager->setupKeycodeTransfer();
}

// There is no input settings UI in the SDL window: host joypads take over the emulated
// joypads (#1 = first host pad, #2 = second) on the alternate mapping, if no joypad is assigned yet.
auto InputManager::autoAssignJoypads() -> void {
    if (cmd->noGui)
        return;

    std::vector<Hid::Device*> pads;
    for (auto hidDevice : hidDevices)
        if (hidDevice->isJoypad())
            pads.push_back(hidDevice);
    if (pads.empty())
        return;

    auto findInput = [](Hid::Device* pad, unsigned groupId, std::vector<std::string> names) -> Hid::Input* {
        for (auto& input : pad->group(groupId).inputs)
            for (auto& name : names)
                if (input.name == name)
                    return &input;
        return nullptr;
    };

    for (auto manager : inputManagers) {
        if (!manager->emulator)
            continue;

        unsigned padIndex = 0;
        bool changed = false;

        for (auto& device : manager->emulator->devices) {
            if (!device.isJoypad())
                continue;
            if (padIndex >= pads.size())
                break;
            auto pad = pads[padIndex++];

            bool hasJoypad = false;
            for (auto& input : device.inputs)
                for (auto m = (InputMapping*)input.guid; m; m = m->alternate)
                    for (auto& hid : m->hids)
                        if (hid.device->isJoypad())
                            hasJoypad = true;
            if (hasJoypad)
                continue;

            for (auto& input : device.inputs) {
                auto mapping = (InputMapping*)input.guid;
                auto alt = mapping ? mapping->alternate : nullptr;
                if (!alt || !alt->hids.empty())
                    continue;

                auto addHid = [&](unsigned groupId, Hid::Input* hidInput, unsigned qualifier) {
                    if (hidInput && alt->hids.size() < MaxMappings)
                        alt->hids.push_back({pad, &pad->group(groupId), hidInput, qualifier, 0});
                };
                using Q = InputMapping::Qualifier;
                auto hatX = findInput(pad, Hid::Joypad::Hat, {"DPad.X", "0.X"});
                auto hatY = findInput(pad, Hid::Joypad::Hat, {"DPad.Y", "0.Y"});
                auto axisX = findInput(pad, Hid::Joypad::Axis, {"X"});
                auto axisY = findInput(pad, Hid::Joypad::Axis, {"Y"});

                if (input.name == "Up") { addHid(Hid::Joypad::Hat, hatY, Q::Lo); addHid(Hid::Joypad::Axis, axisY, Q::Lo); }
                else if (input.name == "Down") { addHid(Hid::Joypad::Hat, hatY, Q::Hi); addHid(Hid::Joypad::Axis, axisY, Q::Hi); }
                else if (input.name == "Left") { addHid(Hid::Joypad::Hat, hatX, Q::Lo); addHid(Hid::Joypad::Axis, axisX, Q::Lo); }
                else if (input.name == "Right") { addHid(Hid::Joypad::Hat, hatX, Q::Hi); addHid(Hid::Joypad::Axis, axisX, Q::Hi); }
                else if (input.name == "Button 1" || input.name == "Button Red")
                    addHid(Hid::Joypad::Button, findInput(pad, Hid::Joypad::Button, {"A", "0"}), 0);
                else if (input.name == "Button 2" || input.name == "Button Blue")
                    addHid(Hid::Joypad::Button, findInput(pad, Hid::Joypad::Button, {"B", "1"}), 0);
                else
                    continue;

                alt->anded = false;
                alt->updateSetting();
                changed = true;
            }
        }
        if (changed) {
            manager->updateMappingsInUse();
            InputManager::urgentUpdate = true;
        }
    }
}

auto InputManager::assumeLayoutType() -> KeyboardLayout::Type {
    
    auto lang = GUIKIT::System::getOSLang();
    
    if (lang == GUIKIT::System::Language::DE)
        return KeyboardLayout::Type::De;
    
    if (lang == GUIKIT::System::Language::US)
        return KeyboardLayout::Type::Us;
    
    if (lang == GUIKIT::System::Language::FR)
        return KeyboardLayout::Type::Fr;

    return KeyboardLayout::Type::Uk;
}

auto InputManager::resetMappings() -> void {
    std::string settingIdent;
    InputMapping* mapper;    
    auto settings = Program::getSettings( emulator );
    
    for (auto& device : emulator->devices) {  
        for (auto& input : device.inputs) {
            
            settingIdent = device.name + "_" + std::to_string(input.id);
            GUIKIT::String::toLowerCase(GUIKIT::String::delSpaces(settingIdent));
         
            mapper = (InputMapping*)input.guid;
            mapper->setting = settings->add( settingIdent );
            
            if (!mapper->isAnalog())
                mapper->alternate->setting = settings->add( settingIdent + "_alt" );
        }
    }
    
    for(auto& item : customHotkeys) {        
        mapper = (InputMapping*)item.guid;
        settingIdent = "hotkey_" + std::to_string(item.id);
        
        mapper->setting = settings->add( settingIdent );
        mapper->alternate->setting = settings->add(settingIdent + "_alt");
    }
}

auto InputManager::setIllegalMappings() -> void {
    InputMapping* mapper;

    for (auto& device : emulator->devices) {
        if (!device.isJoypad())
            continue;
        // up <> down
        mapper = (InputMapping*)device.inputs[0].guid;
        mapper->illegalMapping = (InputMapping*)device.inputs[1].guid;
        // left <> right
        mapper = (InputMapping*)device.inputs[2].guid;
        mapper->illegalMapping = (InputMapping*)device.inputs[3].guid;
    }
}

auto InputManager::setMappings() -> void {
	for (auto manager : inputManagers) {
		if (manager->emulator) {
            auto settings = Program::getSettings( manager->emulator );
            
            for (auto& device : manager->emulator->devices) {  
                for (auto& input : device.inputs) {

                    std::string settingIdent = device.name + "_" + std::to_string(input.id);
                    GUIKIT::String::toLowerCase(GUIKIT::String::delSpaces(settingIdent));
                    InputMapping* mapper = new InputMapping;
                    mapper->setting = settings->add( settingIdent );
                    mapper->state = 0;
                    mapper->type = (InputMapping::Type)input.type;
                    mapper->anded = 0;
                    mapper->emuDevice = &device;
                    mapper->emuInput = &input;
                    mapper->parent = nullptr;
                    mapper->alternate = nullptr;
                    mapper->inputManager = manager;
                    mapper->isShadowed = false;
                    mapper->shadowDelayed = nullptr;
                    mapper->useShadowDelay = false;

                    mapper->autoFire = device.isJoypadOrMultiAdapter() && (input.key == Emulator::Interface::Key::Autofire || input.key == Emulator::Interface::Key::AutofireDirection);
                    if (device.isJoypadOrMultiAdapter() && (input.key == Emulator::Interface::Key::ToggleAutofire)) {
                        mapper->type = InputMapping::Type::Switch;
                        mapper->inputManager = manager;
                        mapper->hotkeyId = Hotkey::Id::Autofire; // share id, later identified by mapping
                    }

                    input.guid = (uintptr_t) mapper;
                    
                    for( auto& inputId : input.shadowMap ) {
                        auto inputPtr = &device.inputs[ inputId ];

                        if (inputPtr->isDigital()) {
                            auto shadowMapper = (InputMapping*) (inputPtr->guid);
                            mapper->shadowMap.push_back(shadowMapper);
                            shadowMapper->isShadowed = true;
                        }
                    }

                    unsigned _ssize = mapper->shadowMap.size();
                    if (device.isKeyboard() && (_ssize > 1))
                        mapper->shadowDelayed = mapper->shadowMap[_ssize - 1];
                    
                    manager->addMapping( mapper );
                    if (!mapper->isAnalog())
                        mapper->generateAlternate( settings );
                }
            }
			
			manager->setCustomHotkeys();
			
			for(auto& item : manager->customHotkeys) {
				InputMapping* mapper = new InputMapping;
				mapper->setting = settings->add( "hotkey_" + std::to_string(item.id) );
				mapper->state = 0;
				mapper->type = InputMapping::Type::Switch;
				mapper->anded = 1;
                mapper->autoFire = false;
				mapper->emuDevice = nullptr;
                mapper->emuInput = nullptr;
				mapper->hotkeyId = item.id;
				mapper->parent = nullptr;
				mapper->inputManager = manager;
                mapper->isShadowed = false;
				item.guid = (uintptr_t) mapper;

				if (item.share) {
					for (auto _manager : inputManagers)
						_manager->addMapping( mapper ); //hotkeys are shared between all manager instances
				} else
					manager->addMapping( mapper );

				mapper->generateAlternate( settings );
			}

            manager->setIllegalMappings();
		}
	}
	
	for(auto& item : hotkeys) {
		item.share = true; // always share global hotkeys
        InputMapping* mapper = new InputMapping;
		mapper->setting = globalSettings->add( "hotkey_" + std::to_string(item.id) );
        mapper->state = 0;
        mapper->type = InputMapping::Type::Switch;
		mapper->anded = 1;
        mapper->autoFire = false;
        mapper->emuDevice = nullptr;
        mapper->emuInput = nullptr;
		mapper->hotkeyId = item.id;
        mapper->parent = nullptr;
        mapper->inputManager = nullptr; // shared mapper don't belong to a specific manager
        mapper->isShadowed = false;
        item.guid = (uintptr_t) mapper;
		
		for (auto manager : inputManagers)
			manager->addMapping( mapper ); //hotkeys are shared between all manager instances
        
        mapper->generateAlternate( globalSettings );
    }

    for(auto& item : hiddenHotkeys) {
        InputMapping* mapper = new InputMapping;
        mapper->hotkeyId = item.id;
        item.guid = (uintptr_t) mapper;
    }
}

auto InputManager::autoAssign( Emulator::Interface::Device& device ) -> void {
    if(!emulator)
        return;

    for (auto& input : device.inputs) {
        if (device.isJoypad() && input.id > 5)
            break;

        auto mapper = (InputMapping*) input.guid;
        auto setting = mapper->setting->value;

        std::vector<std::vector<Hid::Key>> keys;

        for (auto hidDevice : hidDevices) {
            if (hidDevice->isKeyboard())
                continue;

            if (hidDevice->isJoypad() && (device.isMouse() || device.isLightDevice() || device.isPaddles() ))
                continue;
            if (hidDevice->isMouse() && device.isJoypadOrMultiAdapter())
                continue;

            for(auto& group : hidDevice->groups) {
                for(auto& hidInput : group.inputs) {
                    if ( device.isJoypad() ) {
                        int qualifier = -1;

                        if (group.id == Hid::Joypad::Hat) {
                            if (input.name == "Up" && (hidInput.name.find(".Y") != std::string::npos))
                                qualifier = 1;
                            else if (input.name == "Down" && (hidInput.name.find(".Y") != std::string::npos))
                                qualifier = 2;
                            else if (input.name == "Left" && (hidInput.name.find(".X") != std::string::npos))
                                qualifier = 1;
                            else if (input.name == "Right" && (hidInput.name.find(".X") != std::string::npos))
                                qualifier = 2;
                        } else if (group.id == Hid::Joypad::Button) {
                            if ((input.name == "Button 1" || input.name == "Button Red") && hidInput.name == "0")
                                qualifier = 0;
                            else if ((input.name == "Button 2" || input.name == "Button Blue") && hidInput.name == "1")
                                qualifier = 0;
                        }

                        if (qualifier >= 0) {
                            mapper->hids.clear();
                            mapper->hids.push_back( {hidDevice, &group, &hidInput, (unsigned)qualifier, 0} );
                            mapper->anded = 0;
                            break;
                        }
                    } else if (matchButtons(&input, &hidInput) || (input.name.find(hidInput.name) != std::string::npos) ) {
                        mapper->hids.clear();
                        mapper->hids.push_back( {hidDevice, &group, &hidInput, 0, 0} );
                        mapper->anded = 0;
                        break;
                    }
                }
            }
        }

        mapper->updateSetting();
        if (mapper->alternate)
            mapper->alternate->updateSetting();
    }

}

auto InputManager::autoAssign( KeyboardLayout::Type type, bool keyboardOnly ) -> void {
    
    if(!emulator)
        return;

    for( auto& device : emulator->devices ) {
        if (device.name.find("#2") != std::string::npos)
            continue;
        
        for (auto& input : device.inputs) {
            auto mapper = (InputMapping*) input.guid;
            auto setting = mapper->setting->value;
            if (!setting.empty() && (setting != "0"))
                continue;
            
            std::vector<std::vector<Hid::Key>> keys;
            if (device.isKeyboard())
                keys = automap( type, input.key, emulator );

            for (auto hidDevice : hidDevices) {
                if ( (hidDevice->isKeyboard() && device.isKeyboard()) || 
                    ( !keyboardOnly && (
                        (hidDevice->isMouse() && (device.isMouse() || device.isLightDevice() || device.isPaddles() )) ||
                        (hidDevice->isKeyboard() && device.isJoypad() )
                    ) )                    
                    ) {
                    for(auto& group : hidDevice->groups) {
                        for(auto& hidInput : group.inputs) {

                            if (device.isKeyboard()) {
                                mapper->anded = 1;
                                mapper->alternate->anded = 1;

                                if (keys[0].size() == 0 && ( (keys.size() < 2) || (keys[1].size() == 0))) // all elements were mapped
                                    break;

                                for( auto& key : keys[0] ) {
                                    if (key == hidInput.key) {
                                        mapper->hids.push_back( {hidDevice, &group, &hidInput, 0, 0} );
                                        // better we remove the element, as a result multiple
                                        // mappings of the same key can not accidentally found twice
                                        GUIKIT::Vector::eraseVectorElement( keys[0], key );
                                        break;
                                    }
                                }
                                if (keys.size() > 1)
                                    for (auto& key : keys[1]) {
                                        if (key == hidInput.key) {
                                            mapper->alternate->hids.push_back({hidDevice, &group, &hidInput, 0, 0});
                                            GUIKIT::Vector::eraseVectorElement(keys[1], key);
                                            break;
                                        }
                                    }
                            } else if ( device.isJoypad() ) {
                                if (matchButtons(&input, &hidInput)) {
                                    mapper->hids.push_back( {hidDevice, &group, &hidInput, 0, 0} );
                                    mapper->anded = 0;                                    
                                    break;
                                }                                
                            } else if (matchButtons(&input, &hidInput) || (input.name.find(hidInput.name) != std::string::npos) ) {
                                mapper->hids.push_back( {hidDevice, &group, &hidInput, 0, 0} );
                                mapper->anded = 0;
                                break;
                            }                                        
                        }
                    }						
                }					
            }
                          
            mapper->updateSetting();
            if (mapper->alternate)
                mapper->alternate->updateSetting();
        }			
    }
}

auto InputManager::matchButtons( Emulator::Interface::Device::Input* emuInput, Hid::Input* hidInput ) -> bool {
    
    if (emuInput->name == "Up" && hidInput->key == Hid::Key::NumPad8) return true;
    if (emuInput->name == "Down" && hidInput->key == Hid::Key::NumPad2) return true;
    if (emuInput->name == "Left" && hidInput->key == Hid::Key::NumPad4) return true;
    if (emuInput->name == "Right" && hidInput->key == Hid::Key::NumPad6) return true;
    if (emuInput->name == "Button 1" && hidInput->key == Hid::Key::NumPad0) return true;
    
    if (emuInput->name == "Button X" && hidInput->name == "Left") return true;
    if (emuInput->name == "Button Y" && hidInput->name == "Right") return true;
    
    if (emuInput->name == "Trigger" && hidInput->name == "Left") return true;
    
    if (emuInput->name == "Touch" && hidInput->name == "Left") return true;
    if (emuInput->name == "Button" && hidInput->name == "Right") return true;
    
    return false;
}

auto InputManager::prioritiseConnectedDevicesOverKeyboard() -> void {

    if (!emulator)
        return;
    
    Emulator::Interface::Device* keyboard = nullptr;
    
    for( auto& device : emulator->devices ) {
        if (device.isKeyboard()) {
            keyboard = &device;
            break;
        }
    }
    
    if (!keyboard)
        return;
    
    std::vector<Emulator::Interface::Device*> connectedDevices;    
    for (auto& connector : emulator->connectors)  {
        
        auto device = emulator->getConnectedDevice( &connector );
        
        if (!device->isUnplugged())
            connectedDevices.push_back( device );
    }         
        
    if (connectedDevices.size() == 0)
        return;
    
    for( auto device : connectedDevices ) {

        for (auto& input : device->inputs) {
            if (!input.isDigital())
                continue;
            
            auto mapper = (InputMapping*) input.guid;
            
            while(1) {
                
                if (mapper->hids.size() == 0)
                    goto NextMapper;

                if (mapper->anded && (mapper->hids.size() > 1))
                    goto NextMapper;

                for (auto& hid : mapper->hids) {

                    for(auto& keyboardInput : keyboard->inputs) {

                        auto keyboardMapper = (InputMapping*) keyboardInput.guid;
                        
                        while(1) {

                            if (keyboardMapper->hids.size() == 0)
                                goto NextKeyboardMapper;

                            if (keyboardMapper->anded && (keyboardMapper->hids.size() > 1))
                                goto NextKeyboardMapper;

                            for (auto& keyboardHid : keyboardMapper->hids) {

                                if (keyboardHid.disable)
                                    continue;
                                
                                if (keyboardHid.device == hid.device && keyboardHid.group == hid.group
                                && keyboardHid.input == hid.input && keyboardHid.qualifier == hid.qualifier ) {
                                    if (prioritise == 1)
                                        keyboardHid.disable = true;
                                    else
                                        hid.disable = true;
                                }
                            }
                            
                            NextKeyboardMapper:
                            if (keyboardMapper->alternate)
                                keyboardMapper = keyboardMapper->alternate;
                            else
                                break;
                        }
                    }
                }
                NextMapper:
                if (mapper->alternate)
                    mapper = mapper->alternate;
                else
                    break;
            }            
        }
    }
}

// Fullscreen: F11, alternate Alt+Return
auto InputManager::assignDefaultHotkeys() -> void {
    Hid::Device* keyboard = nullptr;
    for (auto hidDevice : hidDevices)
        if (hidDevice->isKeyboard())
            keyboard = hidDevice;
    if (!keyboard)
        return;

    auto find = [keyboard](Hid::Key key) -> Hid::Input* {
        for (auto& input : keyboard->group(Hid::Keyboard::GroupID::Button).inputs)
            if (input.key == key)
                return &input;
        return nullptr;
    };

    for (auto& hotkey : hotkeys) {
        if (hotkey.id != Hotkey::Id::Fullscreen)
            continue;
        auto mapper = (InputMapping*)hotkey.guid;
        auto& group = keyboard->group(Hid::Keyboard::GroupID::Button);
        auto f11 = find(Hid::Key::F11);
        auto alt = find(Hid::Key::AltLeft);
        auto ret = find(Hid::Key::Return);

        if (mapper && f11) {
            mapper->hids = { {keyboard, &group, f11, 0, 0} };
            mapper->anded = true;
            mapper->updateSetting();
        }
        if (mapper && mapper->alternate && alt && ret) {
            mapper->alternate->hids = { {keyboard, &group, alt, 0, 0}, {keyboard, &group, ret, 0, 0} };
            mapper->alternate->anded = true;
            mapper->alternate->updateSetting();
        }
    }
}

auto InputManager::autoAssignHotkeys() -> void {
    for (auto& hotkey : customHotkeys) {
        if (hotkey.id == Hotkey::Id::CaptureMouse) {
            auto mapper = (InputMapping*)hotkey.guid;
            auto setting = mapper->setting->value;
            if (!setting.empty())
                continue;
			
			for (auto hidDevice : hidDevices) {
				if(hidDevice->isMouse()) {
					for (auto& hidInput : hidDevice->group(Hid::Mouse::GroupID::Button).inputs) {
						if ( hidInput.name.find("Middle") != std::string::npos ) {
							mapper->hids.push_back( {hidDevice, &hidDevice->group(Hid::Mouse::GroupID::Button), &hidInput, 0, 0} );
							mapper->anded = false;
							mapper->updateSetting();
							break;
						}
					}
				}  
			}	              
            break;
        }
    }
}

// load settings data at application start, reload only when host devices changing
// missing devices will be removed
auto InputManager::bindHidsGlobal( ) -> void {	
    
	for (auto manager : inputManagers)
        manager->bindHids();
	
    clearLastDeviceState();
}

auto InputManager::bindHids( ) -> void {
    
    for (auto mapping : mappings) {
        while (1) {

            mapping->hids.clear();
            auto parts = GUIKIT::String::split(mapping->setting->value, '|');

            if (parts.size() == 0)
                goto Done;

            mapping->anded = parts[0] != "0";
            mapping->hasUnknownAssignment = false;
            GUIKIT::Vector::eraseVectorPos(parts, 0);

            for (;;) {
                if (mapping->hids.size() == InputManager::MaxMappings) break;
                if (parts.size() < 4) break;
                try {
                    unsigned deviceId = std::stoul(parts[0]);
                    unsigned groupId = std::stoul(parts[1]);
                    unsigned inputId = std::stoul(parts[2]);
                    unsigned qualifier = std::stoul(parts[3]);

                    Hid::Device* hidDevice = getDeviceFromIdent(deviceId);

                    // if device ID can not be found anymore, take first device in list
                    if (!hidDevice) {
                        if (!mapping->emuDevice || mapping->emuDevice->isJoypadOrMultiAdapter() || mapping->emuDevice->isKeyboard()) {
                            for(auto _hidDevice : hidDevices) {
                                if (_hidDevice->isJoypad()) {
                                    hidDevice = _hidDevice;
                                    break;
                                }
                            }
                        } else if (!mapping->emuDevice->isUnplugged()) {
                            for(auto _hidDevice : hidDevices) {
                                if (_hidDevice->isMouse()) {
                                    hidDevice = _hidDevice;
                                    break;
                                }
                            }
                        }
                    }

                    if (hidDevice) {
                        unsigned result = mapping->checkSanity(hidDevice, groupId, inputId);
                        if (result == 0);
                        else if (result == 3 && qualifier == 0);

                        else mapping->hids.push_back({hidDevice, &hidDevice->group(groupId),
                                &hidDevice->group(groupId).inputs[inputId], qualifier, 0});
                    } else if (!driverChange)
                        mapping->hasUnknownAssignment = true;

                } catch (...) { /* conversion to uint failed */ }

                GUIKIT::Vector::eraseVectorPos(parts, 0, 4);
            }

Done:
            if (driverChange)
                // remove unknown devices when driver is changing
                mapping->updateSetting(); //restructure setting in case of corruption or removed devices

            if (!mapping->alternate)
                break;

            mapping = mapping->alternate;
        }
    }
    
    updateMappingsInUse();
}

auto InputManager::clearLastDeviceState() -> void {
    for (auto& remap : remapDevices)
        delete remap.remember;    
    
    remapDevices.clear();
    driverChange = false;
}

auto InputManager::rememberLastDeviceState() -> void {
    
    clearLastDeviceState();
    Hid::Device* remember;
    
    for (auto hidDevice : hidDevices) {
        
        if (hidDevice->isMouse())
            remember = new Hid::Mouse;
        else if (hidDevice->isJoypad())
            remember = new Hid::Joypad;
        else
            continue;
        // don't remember keyboard, all driver use device id = 0 for keyboards
        
        // remember id of device just before a driver change.
        // each driver has it's own unique way for generating device id's.
        // we need to connect these id's, otherwise all taken mappings will be lost when driver has changed
        remember->id = hidDevice->id;
        remember->name = hidDevice->name;
        
        remapDevices.push_back( { remember, nullptr } );
    }
    
    driverChange = true;
}

auto InputManager::assignChangedDeviceState() -> void {
    
    std::vector<Hid::Device*> todo;
    
    for (auto hidDevice : hidDevices) {
        
        bool found = false;
        
        for (auto& remap : remapDevices) {
            
            if (remap.current)
                continue;            

            if (remap.remember->isMouse() && hidDevice->isMouse()) {
                remap.current = hidDevice;
                found = true;
                break;
            }
            
            if (remap.remember->isJoypad() && hidDevice->isJoypad()) {
                if (remap.remember->name == hidDevice->name) {
                    remap.current = hidDevice;
                    found = true;
                    break;                    
                }
            }
        }
        
        if (!found)
            todo.push_back( hidDevice );       
    }
    
    for (auto hidDevice : todo) {
        for (auto& remap : remapDevices) {

            if (remap.current)
                continue;  

            if (remap.remember->isJoypad() && hidDevice->isJoypad()) {
                remap.current = hidDevice;
                break;
            }
        }
    }
}

auto InputManager::getDeviceFromIdent( unsigned id ) -> Hid::Device* {
    
    for (auto hidDevice : hidDevices) {
        
        if (hidDevice->id == id)
            return hidDevice;
    }
    
    for (auto& remap : remapDevices) {
        
        if (!remap.current)
            continue;
        
        if (remap.remember->id == id)
            return remap.current;
    }
    
    return nullptr;
}

auto InputManager::updateMiscSettings() -> void {
    if (!emulator)
        return;

    auto settings = Program::getSettings(emulator);
    oppositeDirections = settings->get<bool>("allow_opposite_directions", false);
    prioritise = settings->get<unsigned>("prioritise_mappings", 1, {0,2});
}

auto InputManager::updateAutofireFrequency() -> void {
    if (!emulator)
        return;

    auto settings = Program::getSettings(emulator);
    autoFireHold = settings->get<unsigned>( "autofire_hold", false );
    autoFireFrequency = settings->get<unsigned>( "autofire_frequency", 1, {1, 99} );

    for(auto mapper : autoFireMappings)
        mapper->autoFirePos = 1;
}

auto InputManager::resetTouchlessAutofire() -> void {
    autoFireMappings.clear();
    allowTouchlessAutofire = false;

    if (!emulator)
        return;

    auto emuView = EmuConfigView::TabWindow::getView( emulator );
    if (emuView && emuView->inputLayout)
        emuView->inputLayout->updatedAutofireButtonHints();
}

auto InputManager::updateAnalogSensitivity(Emulator::Interface::Device* updateDevice) -> void {
    
    int tresholdLo = 200;
    int tresholdHi = 100;
    
    for (auto& device : emulator->devices) {
        
		if (updateDevice && updateDevice != &device)
			continue;		        
        
        auto sensePercent = Program::getSettings(emulator)->get<unsigned>( "analog_sensitivity_" + _underscore(device.name), 40u, { 0u, 100u});

		int sense = sensePercent;
				
		if (device.isJoypadOrMultiAdapter() || device.isKeyboard()) {
			sense = (sensePercent * 32768) / 100;

			sense = std::min(std::max(sense, tresholdLo), 32768 - tresholdHi);		
		} else
			sense = std::max( 5, sense );
         
        for (auto& input : device.inputs) {
            
            auto mapper = (InputMapping*)input.guid;                        
            
			mapper->analogSensitivity = sense;			
			
            if (mapper->alternate)
                mapper->alternate->analogSensitivity = mapper->analogSensitivity;
        }
    }
}

auto InputManager::toggleAutofire(InputMapping* trigger) -> void {

    if (isAutofireActive(trigger))
        GUIKIT::Vector::eraseVectorElement(autoFireMappings, trigger);
    else if (GUIKIT::Vector::find(mappingsInUse, trigger))
        autoFireMappings.push_back(trigger);

    allowTouchlessAutofire = autoFireMappings.size() > 0;
}

auto InputManager::isAutofireActive(InputMapping* trigger) -> bool {

    return GUIKIT::Vector::find(autoFireMappings, trigger);
}

auto InputManager::getFirstAutoFireMapping(unsigned connectorId) -> InputMapping* {
    auto connector = emulator->getConnector(connectorId);
    auto connectedDevice = emulator->getConnectedDevice(connector);
    if (!connectedDevice->isJoypadOrMultiAdapter())
        return nullptr;

    for (auto& input : connectedDevice->inputs) {
        if (input.key == Emulator::Interface::Key::ToggleAutofire) {
            InputMapping* mapping = (InputMapping*)input.guid;
            if (mapping)
                return mapping->shadowMap[0];
        }
    }
    return nullptr;
}