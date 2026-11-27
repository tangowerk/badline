
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

#include <dirent.h>
#include <unistd.h>
#include <pwd.h>
#include <pthread.h>
#include <climits>

#include <sys/stat.h>
#include <ctime>
#include <cmath>
#include <sstream>
#include <regex>

#include "api.h"

#include "tools/decode/lha.h"
namespace GUIKIT {

#include "tools/file/file.cpp"
#include "tools/setting.cpp"
#include "tools/image.cpp"
#include "tools/string.cpp"
#include "tools/utf8.cpp"
#include "tools/system.cpp"

bool Application::isQuit = false;
int Application::exitCode = 0;
std::string Application::name;
std::string Application::vendor;
std::function<void ()> Application::loop = nullptr;

auto Application::run() -> void {
    while (!isQuit) {
        if (loop)
            loop();
        else
            System::sleep(1);
        if (isQuit)
            break; // quit() ran inside loop(): timer owners may be deleted already
        Timer::tickAll();
    }
}

auto Application::quit() -> void {
    isQuit = true;
}

static auto steadyMicros() -> int64_t {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<Timer*> Timer::timers;

Timer::Timer() {
    timers.push_back(this);
}

Timer::~Timer() {
    timers.erase(std::remove(timers.begin(), timers.end(), this), timers.end());
}

auto Timer::setEnabled(bool enabled) -> void {
    state.enabled = enabled;
    if (enabled)
        due = steadyMicros() + (int64_t)state.interval * 1000;
}

auto Timer::setInterval(unsigned intervalInMs) -> void {
    state.interval = intervalInMs;
    if (state.enabled)
        due = steadyMicros() + (int64_t)state.interval * 1000;
}

auto Timer::tickAll() -> void {
    int64_t now = steadyMicros();
    // onFinished may enable/disable or delete timers: iterate over a copy and recheck
    auto list = timers;
    for (auto timer : list) {
        if (std::find(timers.begin(), timers.end(), timer) == timers.end())
            continue;
        if (!timer->state.enabled || now < timer->due)
            continue;
        if (Application::isQuit)
            return; // like the former GLib timers: nothing fires once quitting started
        timer->due = now + (int64_t)timer->state.interval * 1000;
        if (timer->onFinished)
            timer->onFinished();
    }
}

}
