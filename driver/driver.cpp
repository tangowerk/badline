#define DRV_UNIT
#define _USE_MATH_DEFINES
#include <cmath>
#include "driver.h"

#include "video/opengl3/sdl.cpp"
#include "video/vulkan/vulkan.cpp"

#ifdef DRV_OPENAL
	#include "audio/openal.cpp"
#endif

#include "audio/pulseaudio.cpp"
#include "input/sdl.cpp"

namespace DRIVER {

auto Video::available() -> std::vector<std::string> {
    return { "OpenGL", "Vulkan" };
}

auto Video::preferred() -> std::string {
    return "Vulkan";
}

auto Video::create(const std::string& driver) -> Video* {
    if(driver == "OpenGL") return new SDLGL();
    if(driver == "Vulkan") return new SDLVK();

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
    return { "SDL3" };
}

auto Input::preferred() -> std::string {
    return "SDL3";
}

auto Input::create(const std::string& driver) -> Input* {
    if(driver == "SDL3") return new SDLInput();

    return new Input;
}

}
