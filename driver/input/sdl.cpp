// SDL3 host input: keyboard (scancodes), mouse (relative mode) and joypads.
// Events are pumped by the program on the UI thread and passed in via handleEvent();
// poll() runs on the emulation thread and reads the collected state.

#include <SDL3/SDL.h>
#include <cstring>
#include <mutex>
#include <atomic>

#include "../tools/hid.h"
#include "../tools/crc32.h"

namespace DRIVER {

struct SDLInput : public Input {
    Hid::Mouse* hidMouse = nullptr;
    Hid::Keyboard* hidKeyboard = nullptr;
    KeyCallback* keyCallback = nullptr;
    SDL_Window* window = nullptr;

    std::mutex stateMutex;
    std::vector<SDL_Scancode> scancodes; // index = hid input id
    bool keyState[SDL_SCANCODE_COUNT];
    bool keyLatch[SDL_SCANCODE_COUNT]; // press seen since the last poll (taps shorter than a frame)

    struct {
        int x = 0;
        int y = 0;
        bool buttons[5] = {false, false, false, false, false};
    } mouseState;

    std::atomic<bool> mouseAcquired{false};

    struct Joypad {
        SDL_JoystickID instance = 0;
        SDL_Gamepad* gamepad = nullptr;
        SDL_Joystick* joystick = nullptr;
        Hid::Joypad* hid = nullptr;
        unsigned hats = 0;
    };
    std::vector<Joypad> joypads;
    std::vector<std::pair<SDL_JoystickID, bool>> hotplug; // instance, added
    std::mutex hotplugMutex;

    struct KeyEntry { SDL_Scancode code; Hid::Key key; };

    static auto keyTable() -> const std::vector<KeyEntry>& {
        using K = Hid::Key;
        static const std::vector<KeyEntry> table = {
            {SDL_SCANCODE_A, K::A}, {SDL_SCANCODE_B, K::B}, {SDL_SCANCODE_C, K::C}, {SDL_SCANCODE_D, K::D},
            {SDL_SCANCODE_E, K::E}, {SDL_SCANCODE_F, K::F}, {SDL_SCANCODE_G, K::G}, {SDL_SCANCODE_H, K::H},
            {SDL_SCANCODE_I, K::I}, {SDL_SCANCODE_J, K::J}, {SDL_SCANCODE_K, K::K}, {SDL_SCANCODE_L, K::L},
            {SDL_SCANCODE_M, K::M}, {SDL_SCANCODE_N, K::N}, {SDL_SCANCODE_O, K::O}, {SDL_SCANCODE_P, K::P},
            {SDL_SCANCODE_Q, K::Q}, {SDL_SCANCODE_R, K::R}, {SDL_SCANCODE_S, K::S}, {SDL_SCANCODE_T, K::T},
            {SDL_SCANCODE_U, K::U}, {SDL_SCANCODE_V, K::V}, {SDL_SCANCODE_W, K::W}, {SDL_SCANCODE_X, K::X},
            {SDL_SCANCODE_Y, K::Y}, {SDL_SCANCODE_Z, K::Z},
            {SDL_SCANCODE_0, K::D0}, {SDL_SCANCODE_1, K::D1}, {SDL_SCANCODE_2, K::D2}, {SDL_SCANCODE_3, K::D3},
            {SDL_SCANCODE_4, K::D4}, {SDL_SCANCODE_5, K::D5}, {SDL_SCANCODE_6, K::D6}, {SDL_SCANCODE_7, K::D7},
            {SDL_SCANCODE_8, K::D8}, {SDL_SCANCODE_9, K::D9},
            {SDL_SCANCODE_KP_0, K::NumPad0}, {SDL_SCANCODE_KP_1, K::NumPad1}, {SDL_SCANCODE_KP_2, K::NumPad2},
            {SDL_SCANCODE_KP_3, K::NumPad3}, {SDL_SCANCODE_KP_4, K::NumPad4}, {SDL_SCANCODE_KP_5, K::NumPad5},
            {SDL_SCANCODE_KP_6, K::NumPad6}, {SDL_SCANCODE_KP_7, K::NumPad7}, {SDL_SCANCODE_KP_8, K::NumPad8},
            {SDL_SCANCODE_KP_9, K::NumPad9},
            {SDL_SCANCODE_NUMLOCKCLEAR, K::NumLock}, {SDL_SCANCODE_KP_DIVIDE, K::NumDivide},
            {SDL_SCANCODE_KP_MULTIPLY, K::NumMultiply}, {SDL_SCANCODE_KP_MINUS, K::NumSubtract},
            {SDL_SCANCODE_KP_PLUS, K::NumAdd}, {SDL_SCANCODE_KP_ENTER, K::NumEnter},
            {SDL_SCANCODE_KP_PERIOD, K::NumComma}, {SDL_SCANCODE_KP_EQUALS, K::NumEqual},
            {SDL_SCANCODE_F1, K::F1}, {SDL_SCANCODE_F2, K::F2}, {SDL_SCANCODE_F3, K::F3}, {SDL_SCANCODE_F4, K::F4},
            {SDL_SCANCODE_F5, K::F5}, {SDL_SCANCODE_F6, K::F6}, {SDL_SCANCODE_F7, K::F7}, {SDL_SCANCODE_F8, K::F8},
            {SDL_SCANCODE_F9, K::F9}, {SDL_SCANCODE_F10, K::F10}, {SDL_SCANCODE_F11, K::F11}, {SDL_SCANCODE_F12, K::F12},
            {SDL_SCANCODE_RIGHT, K::CursorRight}, {SDL_SCANCODE_LEFT, K::CursorLeft},
            {SDL_SCANCODE_UP, K::CursorUp}, {SDL_SCANCODE_DOWN, K::CursorDown},
            {SDL_SCANCODE_TAB, K::Tab}, {SDL_SCANCODE_RETURN, K::Return}, {SDL_SCANCODE_SPACE, K::Space},
            {SDL_SCANCODE_ESCAPE, K::Esc}, {SDL_SCANCODE_BACKSPACE, K::Backspace}, {SDL_SCANCODE_CAPSLOCK, K::CapsLock},
            {SDL_SCANCODE_LALT, K::AltLeft}, {SDL_SCANCODE_RALT, K::AltRight},
            {SDL_SCANCODE_LSHIFT, K::ShiftLeft}, {SDL_SCANCODE_RSHIFT, K::ShiftRight},
            {SDL_SCANCODE_LCTRL, K::ControlLeft}, {SDL_SCANCODE_RCTRL, K::ControlRight},
            {SDL_SCANCODE_LGUI, K::SuperLeft}, {SDL_SCANCODE_RGUI, K::SuperRight},
            {SDL_SCANCODE_INSERT, K::Insert}, {SDL_SCANCODE_HOME, K::Home}, {SDL_SCANCODE_PAGEUP, K::Prior},
            {SDL_SCANCODE_DELETE, K::Delete}, {SDL_SCANCODE_END, K::End}, {SDL_SCANCODE_PAGEDOWN, K::Next},
            {SDL_SCANCODE_PRINTSCREEN, K::Print}, {SDL_SCANCODE_SCROLLLOCK, K::ScrollLock}, {SDL_SCANCODE_PAUSE, K::Pause},
            // layout dependent keys, named after their UK position (see hid.h)
            {SDL_SCANCODE_COMMA, K::Comma}, {SDL_SCANCODE_PERIOD, K::Period}, {SDL_SCANCODE_SLASH, K::Slash},
            {SDL_SCANCODE_EQUALS, K::Equal}, {SDL_SCANCODE_MINUS, K::Minus}, {SDL_SCANCODE_APOSTROPHE, K::Apostrophe},
            {SDL_SCANCODE_GRAVE, K::Grave}, {SDL_SCANCODE_SEMICOLON, K::Semicolon}, {SDL_SCANCODE_APPLICATION, K::Menu},
            {SDL_SCANCODE_LEFTBRACKET, K::OpenSquareBracket}, {SDL_SCANCODE_RIGHTBRACKET, K::ClosedSquareBracket},
            {SDL_SCANCODE_NONUSBACKSLASH, K::Backslash}, {SDL_SCANCODE_BACKSLASH, K::NumberSign},
        };
        return table;
    }

    auto setWindow(uintptr_t _handle) -> void {
        window = (SDL_Window*)_handle;
    }

    auto init(uintptr_t _handle) -> bool {
        term();
        window = (SDL_Window*)_handle;

        if (!SDL_WasInit(SDL_INIT_GAMEPAD))
            SDL_InitSubSystem(SDL_INIT_GAMEPAD);

        hidKeyboard = new Hid::Keyboard;
        hidMouse = new Hid::Mouse;
        hidKeyboard->id = 0;
        hidMouse->id = 1;

        std::memset(keyState, 0, sizeof keyState);
        std::memset(keyLatch, 0, sizeof keyLatch);
        scancodes.clear();
        for (auto& entry : keyTable()) {
            // letters follow the active layout (like the former X11 driver): German Y/Z,
            // French A/Q, W/Z; everything else is positional (layout milestone pending)
            Hid::Key key = entry.key;
            SDL_Keycode keycode = SDL_GetKeyFromScancode(entry.code, SDL_KMOD_NONE, false);
            if (keycode >= 'a' && keycode <= 'z')
                key = (Hid::Key)((unsigned)Hid::Key::A + (keycode - 'a'));

            std::string name = SDL_GetScancodeName(entry.code);
            if (name.empty())
                name = "Key " + std::to_string((unsigned)entry.code);
            // ISO key between left shift and Z, and the second hash key
            if (entry.code == SDL_SCANCODE_NONUSBACKSLASH) name = "NonUSBackslash";
            hidKeyboard->buttons().append(name, key);
            scancodes.push_back(entry.code);
        }

        hidMouse->axes().append("X");
        hidMouse->axes().append("Y");
        hidMouse->buttons().append("Left");
        hidMouse->buttons().append("Middle");
        hidMouse->buttons().append("Right");
        hidMouse->buttons().append("Back");
        hidMouse->buttons().append("Forward");

        // devices already connected at startup
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        hotplugMutex.lock();
        for (int i = 0; ids && i < count; i++)
            hotplug.push_back({ids[i], true});
        hotplugMutex.unlock();
        SDL_free(ids);
        return true;
    }

    auto term() -> void {
        for (auto& jp : joypads)
            closeJoypad(jp);
        joypads.clear();

        if (hidMouse) delete hidMouse, hidMouse = nullptr;
        if (hidKeyboard) delete hidKeyboard, hidKeyboard = nullptr;
    }

    auto closeJoypad(Joypad& jp) -> void {
        if (jp.gamepad) SDL_CloseGamepad(jp.gamepad);
        else if (jp.joystick) SDL_CloseJoystick(jp.joystick);
        if (jp.hid) delete jp.hid;
        jp.gamepad = nullptr;
        jp.joystick = nullptr;
        jp.hid = nullptr;
    }

    // UI thread
    auto handleEvent(const void* _event) -> void {
        auto& ev = *(const SDL_Event*)_event;

        switch (ev.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (ev.key.repeat)
                    break;
                if (ev.key.scancode < SDL_SCANCODE_COUNT) {
                    stateMutex.lock();
                    keyState[ev.key.scancode] = ev.key.down;
                    if (ev.key.down)
                        keyLatch[ev.key.scancode] = true;
                    stateMutex.unlock();
                }
                if (keyCallback)
                    (*keyCallback)();
                break;

            case SDL_EVENT_MOUSE_MOTION:
                stateMutex.lock();
                mouseState.x += (int)ev.motion.xrel;
                mouseState.y += (int)ev.motion.yrel;
                stateMutex.unlock();
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int pos = -1;
                switch (ev.button.button) {
                    case SDL_BUTTON_LEFT: pos = 0; break;
                    case SDL_BUTTON_MIDDLE: pos = 1; break;
                    case SDL_BUTTON_RIGHT: pos = 2; break;
                    case SDL_BUTTON_X1: pos = 3; break;
                    case SDL_BUTTON_X2: pos = 4; break;
                }
                if (pos >= 0) {
                    stateMutex.lock();
                    mouseState.buttons[pos] = ev.button.down;
                    stateMutex.unlock();
                }
            } break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // released keys are not reported while unfocused
                stateMutex.lock();
                std::memset(keyState, 0, sizeof keyState);
                std::memset(keyLatch, 0, sizeof keyLatch);
                for (auto& b : mouseState.buttons) b = false;
                stateMutex.unlock();
                break;

            case SDL_EVENT_JOYSTICK_ADDED:
            case SDL_EVENT_JOYSTICK_REMOVED:
                hotplugMutex.lock();
                hotplug.push_back({ev.jdevice.which, ev.type == SDL_EVENT_JOYSTICK_ADDED});
                hotplugMutex.unlock();
                break;
        }
    }

    auto uniqueId(unsigned id) -> unsigned {
        // 0 and 1 are keyboard and mouse
        if (id < 2) id += 2;
        for (bool found = true; found; ) {
            found = false;
            for (auto& jp : joypads)
                if (jp.hid && jp.hid->id == id) { id++; found = true; break; }
        }
        return id;
    }

    auto uniqueName(std::string name) -> std::string {
        std::string candidate = name;
        for (unsigned n = 2; ; n++) {
            bool found = false;
            for (auto& jp : joypads)
                if (jp.hid && jp.hid->name == candidate) { found = true; break; }
            if (!found)
                return candidate;
            candidate = name + " #" + std::to_string(n);
        }
    }

    auto openJoypad(SDL_JoystickID instance) -> void {
        for (auto& jp : joypads)
            if (jp.instance == instance)
                return;

        Joypad jp;
        jp.instance = instance;
        std::string name;

        if (SDL_IsGamepad(instance)) {
            jp.gamepad = SDL_OpenGamepad(instance);
            if (!jp.gamepad)
                return;
            jp.joystick = SDL_GetGamepadJoystick(jp.gamepad);
            const char* n = SDL_GetGamepadName(jp.gamepad);
            name = n ? n : "Gamepad";
        } else {
            jp.joystick = SDL_OpenJoystick(instance);
            if (!jp.joystick)
                return;
            const char* n = SDL_GetJoystickName(jp.joystick);
            name = n ? n : "Joypad";
        }

        // stable id across sessions: device GUID (+ name), mappings are stored by this id
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUID(jp.joystick), guid, sizeof guid);
        std::string buf = std::string(guid) + name;
        CRC32 crc32((uint8_t*)buf.c_str(), buf.size());

        jp.hid = new Hid::Joypad;
        jp.hid->id = uniqueId(crc32.value());
        jp.hid->name = uniqueName(name);

        if (jp.gamepad) {
            // unified gamepad layout: D-pad as hat, sticks as axes, triggers, named buttons
            jp.hid->hats().append("DPad.X");
            jp.hid->hats().append("DPad.Y");
            jp.hats = 1;
            jp.hid->axes().append("X");
            jp.hid->axes().append("Y");
            jp.hid->axes().append("X|Rot");
            jp.hid->axes().append("Y|Rot");
            jp.hid->triggers().append("LT");
            jp.hid->triggers().append("RT");
            for (auto& b : gamepadButtons())
                jp.hid->buttons().append(b.name);
        } else {
            int axes = SDL_GetNumJoystickAxes(jp.joystick);
            int hats = SDL_GetNumJoystickHats(jp.joystick);
            int buttons = SDL_GetNumJoystickButtons(jp.joystick);
            for (int i = 0; i < axes; i++) {
                std::string axis = std::to_string(i);
                switch (i) {
                    case 0: axis = "X"; break;
                    case 1: axis = "Y"; break;
                    case 2: axis = "Z"; break;
                    case 3: axis = "Z|Rot"; break;
                    case 4: axis = "X|Rot"; break;
                    case 5: axis = "Y|Rot"; break;
                }
                jp.hid->axes().append(axis);
            }
            jp.hats = hats < 0 ? 0 : hats;
            for (unsigned i = 0; i < jp.hats; i++) {
                jp.hid->hats().append(std::to_string(i) + ".X");
                jp.hid->hats().append(std::to_string(i) + ".Y");
            }
            for (int i = 0; i < buttons; i++)
                jp.hid->buttons().append(std::to_string(i));
        }

        joypads.push_back(jp);
    }

    struct ButtonName { SDL_GamepadButton button; const char* name; };

    static auto gamepadButtons() -> const std::vector<ButtonName>& {
        // position based (SDL "south/east/west/north"), labelled like an Xbox pad
        static const std::vector<ButtonName> list = {
            {SDL_GAMEPAD_BUTTON_SOUTH, "A"}, {SDL_GAMEPAD_BUTTON_EAST, "B"},
            {SDL_GAMEPAD_BUTTON_WEST, "X"}, {SDL_GAMEPAD_BUTTON_NORTH, "Y"},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB"},
            {SDL_GAMEPAD_BUTTON_BACK, "Back"}, {SDL_GAMEPAD_BUTTON_START, "Start"},
            {SDL_GAMEPAD_BUTTON_GUIDE, "Guide"},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, "LS"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RS"},
            {SDL_GAMEPAD_BUTTON_MISC1, "Misc"}, {SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad"},
            {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "P1"}, {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "P2"},
            {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "P3"}, {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "P4"},
        };
        return list;
    }

    auto removeJoypad(SDL_JoystickID instance) -> void {
        for (unsigned i = 0; i < joypads.size(); i++) {
            if (joypads[i].instance == instance) {
                closeJoypad(joypads[i]);
                joypads.erase(joypads.begin() + i);
                return;
            }
        }
    }

    static auto hatValue(bool neg, bool pos) -> int16_t {
        return neg ? -32767 : (pos ? 32767 : 0);
    }

    // emulation thread
    auto poll() -> std::vector<Hid::Device*> {
        std::vector<Hid::Device*> devices;

        hotplugMutex.lock();
        auto changes = hotplug;
        hotplug.clear();
        hotplugMutex.unlock();
        for (auto& change : changes) {
            if (change.second) openJoypad(change.first);
            else removeJoypad(change.first);
        }

        stateMutex.lock();
        auto& keys = hidKeyboard->buttons().inputs;
        for (unsigned i = 0; i < keys.size(); i++) {
            auto code = scancodes[i];
            keys[i].setValue(keyState[code] || keyLatch[code]);
            keyLatch[code] = false;
        }

        hidMouse->axes().inputs[0].setValue(mouseState.x);
        hidMouse->axes().inputs[1].setValue(mouseState.y);
        mouseState.x = 0;
        mouseState.y = 0;
        for (unsigned i = 0; i < 5; i++)
            hidMouse->buttons().inputs[i].setValue(mouseState.buttons[i]);
        stateMutex.unlock();

        devices.push_back(hidKeyboard);
        devices.push_back(hidMouse);

        for (auto& jp : joypads) {
            auto clampAxis = [](int v) -> int16_t { return v < -32767 ? -32767 : v; };

            if (jp.gamepad) {
                auto g = jp.gamepad;
                auto& hats = jp.hid->hats().inputs;
                hats[0].setValue(hatValue(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT), SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)));
                hats[1].setValue(hatValue(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP), SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)));

                auto& axes = jp.hid->axes().inputs;
                axes[0].setValue(clampAxis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)));
                axes[1].setValue(clampAxis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)));
                axes[2].setValue(clampAxis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX)));
                axes[3].setValue(clampAxis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY)));

                auto& triggers = jp.hid->triggers().inputs;
                triggers[0].setValue(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
                triggers[1].setValue(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

                auto& buttons = jp.hid->buttons().inputs;
                auto& names = gamepadButtons();
                for (unsigned i = 0; i < names.size(); i++)
                    buttons[i].setValue(SDL_GetGamepadButton(g, names[i].button));
            } else {
                auto j = jp.joystick;
                auto& axes = jp.hid->axes().inputs;
                for (unsigned i = 0; i < axes.size(); i++)
                    axes[i].setValue(clampAxis(SDL_GetJoystickAxis(j, i)));

                auto& hats = jp.hid->hats().inputs;
                for (unsigned i = 0; i < jp.hats; i++) {
                    uint8_t h = SDL_GetJoystickHat(j, i);
                    hats[i * 2].setValue(hatValue(h & SDL_HAT_LEFT, h & SDL_HAT_RIGHT));
                    hats[i * 2 + 1].setValue(hatValue(h & SDL_HAT_UP, h & SDL_HAT_DOWN));
                }

                auto& buttons = jp.hid->buttons().inputs;
                for (unsigned i = 0; i < buttons.size(); i++)
                    buttons[i].setValue(SDL_GetJoystickButton(j, i));
            }
            devices.push_back(jp.hid);
        }

        return devices;
    }

    auto setKeyboardCallback( KeyCallback* callback ) -> void {
        this->keyCallback = callback;
    }

    // the program applies the requested state on the UI thread (SDL window calls)
    auto mAcquire() -> void { mouseAcquired = true; }
    auto mUnacquire() -> void { mouseAcquired = false; }
    auto mIsAcquired() -> bool { return mouseAcquired; }

    SDLInput() {}
    ~SDLInput() { term(); }
};

}
