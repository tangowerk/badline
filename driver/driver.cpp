#define DRV_UNIT
#define _USE_MATH_DEFINES
#include <cmath>
#include "driver.h"
#include "video/dragnDropOverlay.h"
#include "video/splashScreen.h"

#include "video/opengl3/glx.cpp"

#ifdef DRV_OPENAL
	#include "audio/openal.cpp"
#endif

#include "audio/pulseaudio.cpp"
#include "input/udev.cpp"
#include "input/xlib.cpp"
#include "input/xcb.cpp"

namespace DRIVER {

auto Video::getFontNames(const std::string& fontPath) -> std::vector<std::string> {
	return Freetype::getFontNames(fontPath);
}

auto Video::available() -> std::vector<std::string> {
    return { "OpenGL" };
}

auto Video::preferred() -> std::string {
    return "OpenGL";
}

auto Video::create(const std::string& driver) -> Video* {
    if(driver == "OpenGL") return new GLX();

    return new Video;
}

auto Audio::available() -> std::vector<std::string> {
    std::vector<std::string> list;

    list.push_back("PulseAudio");

    #ifdef DRV_OPENAL
        list.push_back("OpenAL");
    #endif

    return list;
}

auto Audio::preferred() -> std::string {
    return "PulseAudio";
}

auto Audio::create(const std::string& driver) -> Audio* {
    if(driver == "PulseAudio") return new PulseAudio();

    #ifdef DRV_OPENAL
        if(driver == "OpenAL") return new OpenAL();
    #endif

    return new Audio;
}

auto Input::available() -> std::vector<std::string> {
    return { "XCB/Udev", "Xlib/Udev" };
}

auto Input::preferred() -> std::string {
    return "XCB/Udev";
}

auto Input::create(const std::string& driver) -> Input* {
    if(driver == "XCB/Udev") return new XCBInput("udev");
    if(driver == "Xlib/Udev") return new XInput("udev");

    return new Input;
}

}
