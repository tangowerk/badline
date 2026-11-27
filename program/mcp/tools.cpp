// MCP tool implementations. See server.h for the threading model.

#include "server.h"
#include "screen.h"
#include "../program.h"
#include "../cmd/cmd.h"
#include "../thread/emuThread.h"
#include "../media/autoloader.h"
#include "../media/fileloader.h"
#include "../helper/miscHelper.h"
#include "../states/states.h"
#include "../window/sdlWindow.h"
#include "../firmware/roms.h"
#include "../overlay/overlay.h"
#include "../view/status.h"
#include "../video/manager.h"
#include "../../emulation/libc64/system/system.h"
#include "../../emulation/libc64/system/keyBuffer.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using Job = McpServer::Job;
using Device = Emulator::Interface::Device;

namespace {

auto c64() -> LIBC64::Interface* {
    return dynamic_cast<LIBC64::Interface*>(activeEmulator);
}

auto lower(std::string s) -> std::string {
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

auto fps() -> double {
    if (activeEmulator && activeEmulator->getRegionGeometry() == Emulator::Interface::Region::Ntsc)
        return 59.826;
    return 50.125;
}

// locks the emu thread from the UI thread; nested/disabled locks are no-ops
struct EmuLock {
    bool locked;
    explicit EmuLock(bool unlockDebugging = false) { locked = emuThread->lock(unlockDebugging); }
    ~EmuLock() { if (locked) emuThread->unlock(); }
};

auto tempDir() -> std::string {
    std::string dir = "/tmp/badline-mcp-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    return dir;
}

auto fileExists(const std::string& path) -> bool {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

auto fileSize(const std::string& path) -> long {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

auto readFile(const std::string& path, std::vector<uint8_t>& out) -> bool {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------- keyboard

auto keyboardDevice() -> Device* {
    if (!activeEmulator)
        return nullptr;
    for (auto& d : activeEmulator->devices)
        if (d.type == Device::Type::Keyboard)
            return &d;
    return nullptr;
}

auto keyAlias(const std::string& name) -> std::string {
    static const std::vector<std::pair<std::string, std::string>> aliases = {
        {"enter", "return"}, {"shift", "left shift"}, {"lshift", "left shift"}, {"rshift", "right shift"},
        {"run/stop", "runstop"}, {"run stop", "runstop"}, {"stop", "runstop"},
        {"commodore", "c="}, {"cbm", "c="}, {"control", "ctrl"},
        {"up", "cursor up"}, {"down", "cursor down"}, {"left", "cursor left"}, {"right", "cursor right"},
        {"del", "backspace"}, {"delete", "backspace"}, {"inst/del", "backspace"},
        {"clr/home", "home"}, {"clr", "home"}, {"shift lock", "shiftlock"}, {"space bar", "space"}, {" ", "space"},
        {"+", "plus"}, {"-", "minus"}, {"=", "equal"}, {"£", "pound"}, {"←", "arrow left"}, {"↑", "arrow up"},
    };
    auto n = lower(name);
    for (auto& a : aliases)
        if (a.first == n)
            return a.second;
    return n;
}

// resolves a key name to physical input ids (virtual keys expand to their combination)
auto resolveKey(Device* kbd, const std::string& name, std::vector<unsigned>& ids) -> bool {
    auto want = keyAlias(name);
    for (auto& input : kbd->inputs) {
        if (lower(input.name) != want)
            continue;
        if (!input.shadowMap.empty())
            ids.insert(ids.end(), input.shadowMap.begin(), input.shadowMap.end());
        else
            ids.push_back(input.id);
        return true;
    }
    return false;
}

auto keyNames(Device* kbd) -> std::string {
    std::string out;
    for (auto& input : kbd->inputs) {
        if (!out.empty())
            out += ", ";
        out += input.name;
    }
    return out;
}

// ---------------------------------------------------------------- media

auto mediaStatus(Emulator::Interface::Media* media) -> Json {
    if (!media || !media->guid)
        return Json();
    return Json(program->getFileNameFromMedia(media));
}

// ---------------------------------------------------------------- warp helpers (UI thread)

auto warpOn() -> bool {
    return program->warp.mode != Program::Warp::Off;
}

auto beginWarp(Job& job) -> void {
    EmuLock lock;
    job.warpBefore = (int)program->warp.mode;
    if (!warpOn())
        program->setWarp(Program::Warp::Normal, true);
}

auto endWarp(Job& job) -> void {
    if (job.warpBefore < 0)
        return;
    EmuLock lock;
    if ((Program::Warp::Mode)job.warpBefore == Program::Warp::Off && warpOn())
        program->setWarp(Program::Warp::Off);
    job.warpBefore = -1;
}

auto requireEmu(Job& job) -> bool {
    if (!c64()) {
        job.fail("No emulator running");
        return false;
    }
    return true;
}

auto findText(const std::string& screen, const std::string& text, bool caseSensitive) -> bool {
    if (caseSensitive)
        return screen.find(text) != std::string::npos;
    return lower(screen).find(lower(text)) != std::string::npos;
}

} // namespace

auto McpServer::registerTools() -> void {

    // ------------------------------------------------------------ status
    addTool("status", "Emulator state: running/paused, warp, region, frame counter, inserted media.",
        R"({"type":"object","properties":{}})",
        [this](Job& job) {
            job.phase = Job::Emu;
            job.onEmu = [this](Job& job) {
                if (!requireEmu(job)) return;
                Json s = Json::object();
                s.set("paused", program->isPause != 0);
                s.set("overlay_open", overlay && overlay->isOpen());
                s.set("debugger_stopped", cpuStopped());
                if (cpuStopped())
                    s.set("stop", stopStatus());
                s.set("mcp_breakpoints", (unsigned)breakpoints.size());
                s.set("headless", cmd->noGui);
                {
                    auto missing = Roms::missing();
                    if (!missing.empty()) {
                        std::string list;
                        for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
                        s.set("roms_missing", list + " (run with -fetch-roms or put them into " + Roms::userDir() + ")");
                    }
                }
                s.set("warp", warpOn());
                s.set("region", activeEmulator->getRegionGeometry() == Emulator::Interface::Region::Ntsc ? "NTSC" : "PAL");
                s.set("frame", frameCounter);
                Json media = Json::object();
                for (unsigned i = 0; i < 4; i++)
                    media.set("drive" + std::to_string(8 + i), mediaStatus(activeEmulator->getDisk(i)));
                media.set("tape", mediaStatus(activeEmulator->getTape(0)));
                Json cart;
                auto exp = activeEmulator->getExpansion();
                if (exp && !exp->isEmpty()) {
                    cart = Json::object();
                    cart.set("type", exp->name);
                    if (exp->mediaGroup && !exp->mediaGroup->media.empty())
                        cart.set("file", mediaStatus(&exp->mediaGroup->media[0]));
                }
                media.set("cartridge", cart);
                s.set("media", media);
                s.set("charset", McpScreen::lowercaseCharset(c64()) ? "lowercase" : "uppercase");
                job.addText(s.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ reset
    addTool("reset", "Reset the C64. type 'soft' (default) = reset line, 'hard' = power cycle (clears RAM, keeps media).",
        R"({"type":"object","properties":{"type":{"type":"string","enum":["soft","hard"],"default":"soft"}}})",
        [](Job& job) {
            auto type = job.args.getString("type", "soft");
            if (type != "soft" && type != "hard")
                return job.fail("type must be 'soft' or 'hard'");
            job.resumesCpu = true;
            job.phase = Job::Ui;
            job.onUi = [type](Job& job) {
                if (!activeEmulator) return job.fail("No emulator running");
                EmuLock lock(true);
                if (type == "hard")
                    program->power(activeEmulator);
                else
                    program->reset(activeEmulator);
                job.addText(type == "hard" ? "Power cycled" : "Reset");
                job.finish();
            };
        });

    // ------------------------------------------------------------ pause / resume
    auto pauseTool = [](bool pause) {
        return [pause](Job& job) {
            job.phase = Job::Ui;
            job.onUi = [pause](Job& job) {
                if (!activeEmulator) return job.fail("No emulator running");
                EmuLock lock;
                if (pause)
                    program->isPause |= 1;
                else
                    program->isPause &= ~1u;
                job.addText(pause ? "Paused" : "Running");
                job.finish();
            };
        };
    };
    addTool("pause", "Pause the emulation. The last frame stays visible. run_frames/wait_for_text still step while paused.",
        R"({"type":"object","properties":{}})", pauseTool(true));
    addTool("resume", "Resume the emulation (an open overlay keeps it paused until closed).", R"({"type":"object","properties":{}})", pauseTool(false));

    // ------------------------------------------------------------ warp
    addTool("warp", "Switch warp mode (unthrottled speed, no audio) on or off.",
        R"({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})",
        [](Job& job) {
            if (!job.args["enabled"].isBool())
                return job.fail("'enabled' (boolean) required");
            bool enabled = job.args.getBool("enabled");
            job.phase = Job::Ui;
            job.onUi = [enabled](Job& job) {
                if (!activeEmulator) return job.fail("No emulator running");
                EmuLock lock;
                program->setWarp(enabled ? Program::Warp::Normal : Program::Warp::Off, true);
                job.addText(enabled ? "Warp on" : "Warp off");
                job.finish();
            };
        });

    // ------------------------------------------------------------ load_media
    addTool("load_media",
        "Insert a PRG/D64/G64/T64/TAP/CRT/... file (type detected from content/suffix) and optionally autostart it. "
        "Give a local 'path', or 'data_base64' plus 'filename' (suffix decides the type). "
        "After autostart use wait_for_text or run_frames to let it load.",
        R"({"type":"object","properties":{)"
        R"("path":{"type":"string","description":"Absolute path of a local file"},)"
        R"("data_base64":{"type":"string","description":"File content, base64"},)"
        R"("filename":{"type":"string","description":"File name incl. suffix, used with data_base64"},)"
        R"("autostart":{"type":"boolean","default":true},)"
        R"("listing_index":{"type":"integer","description":"Directory entry to load, 0 = first file","default":0}}})",
        [](Job& job) {
            std::string path = job.args.getString("path");
            if (job.args.has("data_base64")) {
                std::string name = job.args.getString("filename");
                if (name.empty() || name.find('/') != std::string::npos)
                    return job.fail("'filename' (without directory) required with data_base64");
                std::vector<uint8_t> data;
                if (!Base64::decode(job.args.getString("data_base64"), data) || data.empty())
                    return job.fail("Invalid base64 data");
                path = tempDir() + "/" + name;
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write((const char*)data.data(), (std::streamsize)data.size());
                if (!f)
                    return job.fail("Cannot write temporary file " + path);
            }
            if (path.empty())
                return job.fail("'path' or 'data_base64' required");
            if (!fileExists(path))
                return job.fail("File not found: " + path);

            job.text = path;
            job.counter = (uint64_t)std::max<int64_t>(0, job.args.getInt("listing_index", 0));
            bool autostart = job.args.getBool("autostart", true);
            job.resumesCpu = true;
            job.phase = Job::Ui;
            job.onUi = [autostart](Job& job) {
                EmuLock lock(true);
                autoloader->init({job.text}, autostart ? Autoloader::Mode::AutoStart : Autoloader::Mode::Open, (unsigned)job.counter);
                autoloader->setErrorLevel(1);
                autoloader->loadFiles();
                if (!autoloader->hasLoaded())
                    return job.fail("Could not load " + job.text);
                job.addText(std::string(autostart ? "Autostarted " : "Inserted ") + job.text);
                job.finish();
            };
        });

    // ------------------------------------------------------------ eject
    addTool("eject", "Remove media. target: drive8..drive11, tape, cartridge (followed by a hard reset), all.",
        R"({"type":"object","properties":{"target":{"type":"string","enum":["drive8","drive9","drive10","drive11","tape","cartridge","all"]}},"required":["target"]})",
        [](Job& job) {
            auto target = job.args.getString("target");
            static const std::vector<std::string> valid = {"drive8", "drive9", "drive10", "drive11", "tape", "cartridge", "all"};
            if (std::find(valid.begin(), valid.end(), target) == valid.end())
                return job.fail("Invalid target");
            job.resumesCpu = true;
            job.phase = Job::Ui;
            job.onUi = [target](Job& job) {
                auto emu = activeEmulator;
                if (!emu) return job.fail("No emulator running");
                EmuLock lock(true);
                std::vector<std::string> ejected;
                for (unsigned i = 0; i < 4; i++) {
                    auto name = "drive" + std::to_string(8 + i);
                    auto media = emu->getDisk(i);
                    if ((target == name || target == "all") && media && media->guid) {
                        fileloader->eject(emu, media);
                        ejected.push_back(name);
                    }
                }
                auto tape = emu->getTape(0);
                if ((target == "tape" || target == "all") && tape && tape->guid) {
                    fileloader->eject(emu, tape);
                    ejected.push_back("tape");
                }
                if (target == "cartridge" || target == "all") {
                    auto exp = emu->getExpansion();
                    bool any = false;
                    if (exp && exp->mediaGroup) {
                        for (auto& media : exp->mediaGroup->media) {
                            if (media.guid) {
                                fileloader->eject(emu, &media);
                                any = true;
                            }
                        }
                    }
                    if (any) {
                        MiscHelper::removeExpansion(emu);
                        program->power(emu);
                        ejected.push_back("cartridge");
                    }
                }
                job.addText(ejected.empty() ? std::string("Nothing to eject") : "Ejected: " + [&] {
                    std::string s;
                    for (auto& e : ejected) s += (s.empty() ? "" : ", ") + e;
                    return s;
                }());
                job.finish();
            };
        });

    // ------------------------------------------------------------ savestates
    addTool("savestate_save", "Save the complete machine state to a file (plus <path>.images with media paths).",
        R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute target path"}},"required":["path"]})",
        [](Job& job) {
            job.text = job.args.getString("path");
            if (job.text.empty() || job.text[0] != '/')
                return job.fail("Absolute 'path' required");
            job.resumesCpu = true;
            job.phase = Job::Ui;
            job.onUi = [](Job& job) {
                if (!activeEmulator) return job.fail("No emulator running");
                ::unlink(job.text.c_str());
                {
                    EmuLock lock(true);
                    States::getInstance(activeEmulator)->save(job.text);
                }
                long size = fileSize(job.text);
                if (size <= 0)
                    return job.fail("Saving state failed: " + job.text);
                job.addText("Saved state to " + job.text + " (" + std::to_string(size) + " bytes)");
                job.finish();
            };
        });

    addTool("savestate_load", "Load a machine state saved with savestate_save (or the GUI).",
        R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path"}},"required":["path"]})",
        [](Job& job) {
            job.text = job.args.getString("path");
            if (!fileExists(job.text))
                return job.fail("File not found: " + job.text);
            job.resumesCpu = true;
            job.phase = Job::Ui;
            job.onUi = [](Job& job) {
                auto emu = activeEmulator ? activeEmulator : program->getEmulator("C64");
                std::vector<uint8_t> data;
                if (!readFile(job.text, data) || data.empty())
                    return job.fail("Cannot read " + job.text);
                EmuLock lock(true);
                if (!emu->checkstate(data.data(), (unsigned)data.size()))
                    return job.fail("Incompatible or broken state file: " + job.text);
                States::getInstance(emu)->load(job.text);
                job.addText("Loaded state " + job.text);
                job.finish();
            };
        });

    // ------------------------------------------------------------ type_text
    addTool("type_text",
        "Type text via the keyboard buffer (like pasting). ASCII is converted to PETSCII, '\\n' = RETURN. "
        "Returns when the text has been consumed. Unshifted letters appear uppercase in the default charset.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
        [](Job& job) {
            job.text = job.args.getString("text");
            if (job.text.empty())
                return job.fail("'text' required");
            if (job.text.size() > 4000)
                return job.fail("Text too long (max 4000 characters)");
            job.wallTimeout = 30.0 + job.text.size() * 0.2;
            job.phase = Job::Emu;
            job.onEmu = [](Job& job) {
                if (!requireEmu(job)) return;
                activeEmulator->pasteText(job.text);
                job.counter = 0;
                job.phase = Job::Frames;
            };
            job.onFrame = [](Job& job) {
                auto emu = c64();
                if (!emu || job.cancelled) return job.finish();
                if (emu->system->keyBuffer->hasJobs) {
                    job.counter = 0;
                    return;
                }
                // a few frames for the KERNAL to process the last key
                if (++job.counter >= 3) {
                    job.addText("Typed " + std::to_string(job.text.size()) + " characters");
                    job.finish();
                }
            };
        }, true);

    // ------------------------------------------------------------ key
    addTool("key",
        "Press keys simultaneously for a number of frames, then release. Names: A-Z, 0-9, F1-F8, Return, Space, "
        "RunStop, Restore, Left Shift, Right Shift, C=, Ctrl, Home, Backspace, Cursor Up/Down/Left/Right, "
        "Arrow Left, Arrow Up, Pound, Plus, Minus, ShiftLock and punctuation such as ':' or '!'.",
        R"({"type":"object","properties":{)"
        R"("keys":{"type":"array","items":{"type":"string"},"description":"e.g. [\"RunStop\",\"Restore\"]"},)"
        R"("frames":{"type":"integer","minimum":1,"maximum":3000,"default":3,"description":"Hold time in frames"}},"required":["keys"]})",
        [this](Job& job) {
            auto& keys = job.args["keys"];
            if (!keys.isArray() || keys.items.empty())
                return job.fail("'keys' (array of key names) required");
            job.limit = (uint64_t)std::clamp<int64_t>(job.args.getInt("frames", 3), 1, 3000);
            job.wallTimeout = 30.0 + job.limit / 25.0;
            job.phase = Job::Emu;
            job.onEmu = [this](Job& job) {
                if (!requireEmu(job)) return;
                auto kbd = keyboardDevice();
                if (!kbd) return job.fail("No keyboard device");
                job.inputs.clear();
                for (auto& k : job.args["keys"].items) {
                    if (!k.isString() || !resolveKey(kbd, k.string, job.inputs))
                        return job.fail("Unknown key: " + (k.isString() ? k.string : k.dump()) + ". Known keys: " + keyNames(kbd));
                }
                job.deviceId = kbd->id;
                for (auto id : job.inputs)
                    setInput(job.deviceId, id, true);
                job.counter = 0;
                job.phase = Job::Frames;
            };
            job.onFrame = [this](Job& job) {
                job.counter++;
                if (job.counter == job.limit || job.cancelled) {
                    for (auto id : job.inputs)
                        setInput(job.deviceId, id, false);
                    if (job.cancelled) return job.finish();
                }
                if (job.counter >= job.limit + 2) {
                    job.addText("Pressed for " + std::to_string(job.limit) + " frames");
                    job.finish();
                }
            };
        }, true);

    // ------------------------------------------------------------ joystick
    addTool("joystick",
        "Set the joystick in control port 1 or 2. With frames > 0 the state is held for that many frames, then released; "
        "with frames = 0 it stays until the next joystick call (all false releases it).",
        R"({"type":"object","properties":{)"
        R"("port":{"type":"integer","enum":[1,2]},)"
        R"("up":{"type":"boolean"},"down":{"type":"boolean"},"left":{"type":"boolean"},"right":{"type":"boolean"},)"
        R"("fire":{"type":"boolean"},)"
        R"("frames":{"type":"integer","minimum":0,"maximum":30000,"default":5}},"required":["port"]})",
        [this](Job& job) {
            int port = (int)job.args.getInt("port", 0);
            if (port != 1 && port != 2)
                return job.fail("'port' must be 1 or 2");
            job.limit = (uint64_t)std::clamp<int64_t>(job.args.getInt("frames", 5), 0, 30000);
            job.wallTimeout = 30.0 + job.limit / 25.0;
            job.phase = Job::Emu;
            job.onEmu = [this, port](Job& job) {
                if (!requireEmu(job)) return;
                auto& connector = activeEmulator->connectors[port - 1];
                auto device = activeEmulator->getConnectedDevice(&connector);
                if (!device || device->type != Device::Type::Joypad)
                    return job.fail("No joystick connected to port " + std::to_string(port) +
                        " (connected: " + (device ? device->name : std::string("none")) + ")");
                job.deviceId = device->id;
                releaseDevice(job.deviceId);
                static const char* dirs[] = {"up", "down", "left", "right", "fire"};
                std::string state;
                for (unsigned i = 0; i < 5; i++) {
                    if (job.args.getBool(dirs[i])) {
                        setInput(job.deviceId, i, true);
                        state += (state.empty() ? "" : "+") + std::string(dirs[i]);
                    }
                }
                if (state.empty()) state = "neutral";
                if (job.limit == 0) {
                    job.addText("Port " + std::to_string(port) + ": " + state);
                    return job.finish();
                }
                if (cpuStopped()) {
                    releaseDevice(job.deviceId);
                    return job.fail("CPU stopped by the debugger; use frames=0 or continue first");
                }
                job.text = "Port " + std::to_string(port) + ": " + state + " for " + std::to_string(job.limit) + " frames";
                job.counter = 0;
                job.phase = Job::Frames;
            };
            job.onFrame = [this](Job& job) {
                job.counter++;
                if (job.counter == job.limit || job.cancelled) {
                    releaseDevice(job.deviceId);
                    if (job.cancelled) return job.finish();
                }
                if (job.counter >= job.limit + 1) {
                    job.addText(job.text);
                    job.finish();
                }
            };
        });

    // ------------------------------------------------------------ run_frames
    addTool("run_frames",
        "Run the emulation for a number of frames (PAL 50/s, NTSC 60/s). When running it waits; when paused it "
        "advances exactly that many frames and stays paused. warp=true runs unthrottled meanwhile.",
        R"({"type":"object","properties":{)"
        R"("frames":{"type":"integer","minimum":1,"maximum":1000000},)"
        R"("warp":{"type":"boolean","default":false}},"required":["frames"]})",
        [](Job& job) {
            int64_t frames = job.args.getInt("frames", 0);
            if (frames < 1 || frames > 1000000)
                return job.fail("'frames' must be 1..1000000");
            job.limit = (uint64_t)frames;
            bool warp = job.args.getBool("warp");
            job.wallTimeout = 30.0 + frames / (warp ? 100.0 : 25.0);
            job.counter = 0;
            job.phase = warp ? Job::Ui : Job::Frames;
            job.onUi = [](Job& job) {
                beginWarp(job);
                job.phase = Job::Frames;
            };
            job.onFrame = [](Job& job) {
                if (++job.counter >= job.limit || job.cancelled) {
                    job.addText("Ran " + std::to_string(job.counter) + " frames");
                    if (job.warpBefore >= 0)
                        job.phase = Job::UiFinish;
                    else
                        job.finish();
                }
            };
            job.onUiFinish = [](Job& job) {
                endWarp(job);
                job.finish();
            };
        }, true);

    // ------------------------------------------------------------ wait_for_text
    addTool("wait_for_text",
        "Run the emulation until the text appears on the text screen (checked every frame) or the timeout "
        "(emulated seconds) expires. Case-insensitive by default. Works while paused (stays paused afterwards).",
        R"({"type":"object","properties":{)"
        R"("text":{"type":"string"},)"
        R"("timeout":{"type":"number","minimum":0,"maximum":3600,"default":10,"description":"Emulated seconds"},)"
        R"("case_sensitive":{"type":"boolean","default":false},)"
        R"("warp":{"type":"boolean","default":false}},"required":["text"]})",
        [](Job& job) {
            job.text = job.args.getString("text");
            if (job.text.empty())
                return job.fail("'text' required");
            double timeout = std::clamp(job.args.getNumber("timeout", 10.0), 0.0, 3600.0);
            bool warp = job.args.getBool("warp");
            bool caseSensitive = job.args.getBool("case_sensitive");
            job.wallTimeout = 30.0 + timeout * (warp ? 0.5 : 1.2);
            job.counter = 0;

            auto done = [](Job& job, bool found) {
                if (found) {
                    job.addText("Found after " + std::to_string(job.counter) + " frames");
                } else {
                    job.isError = true;
                    job.addText("Timeout after " + std::to_string(job.counter) + " frames. Screen:\n" + McpScreen::readText(c64()));
                }
                if (job.warpBefore >= 0)
                    job.phase = Job::UiFinish;
                else
                    job.finish();
            };

            job.phase = Job::Emu;
            job.onEmu = [timeout, warp, caseSensitive, done](Job& job) {
                if (!requireEmu(job)) return;
                job.limit = (uint64_t)(timeout * fps());
                if (findText(McpScreen::readText(c64()), job.text, caseSensitive))
                    return done(job, true);
                if (job.limit == 0)
                    return done(job, false);
                job.phase = warp ? Job::Ui : Job::Frames;
            };
            job.onUi = [](Job& job) {
                beginWarp(job);
                job.phase = Job::Frames;
            };
            job.onFrame = [caseSensitive, done](Job& job) {
                job.counter++;
                auto emu = c64();
                if (!emu || job.cancelled) {
                    job.isError = true;
                    job.addText("Cancelled");
                    if (job.warpBefore >= 0) job.phase = Job::UiFinish; else job.finish();
                    return;
                }
                if (findText(McpScreen::readText(emu), job.text, caseSensitive))
                    return done(job, true);
                if (job.counter >= job.limit)
                    return done(job, false);
            };
            job.onUiFinish = [](Job& job) {
                endWarp(job);
                job.finish();
            };
        }, true);

    // ------------------------------------------------------------ read_screen
    addTool("read_screen",
        "Read the 40x25 text screen from screen RAM (current VIC bank and $D018). Letters follow the active charset; "
        "PETSCII graphics map to similar Unicode characters; reverse video is not marked. raw=true returns screen codes as hex.",
        R"({"type":"object","properties":{"raw":{"type":"boolean","default":false}}})",
        [](Job& job) {
            bool raw = job.args.getBool("raw");
            job.phase = Job::Emu;
            job.onEmu = [raw](Job& job) {
                if (!requireEmu(job)) return;
                job.addText(raw ? McpScreen::readRaw(c64()) : McpScreen::readText(c64()));
                job.finish();
            };
        });

    // ------------------------------------------------------------ screenshot
    addTool("screenshot",
        "PNG of the current frame. Default: raw emulator framebuffer with palette (no shader). "
        "border: 'visible' (as displayed), 'none' (320x200 display area), 'full' (whole frame). scale 1-4. "
        "rendered=true captures the window output incl. shaders (needs a window; ignores border/scale; may advance one frame while paused).",
        R"({"type":"object","properties":{)"
        R"("border":{"type":"string","enum":["visible","none","full"],"default":"visible"},)"
        R"("scale":{"type":"integer","minimum":1,"maximum":4,"default":2},)"
        R"("rendered":{"type":"boolean","default":false}}})",
        [](Job& job) {
            auto borderName = job.args.getString("border", "visible");
            McpScreen::Border border;
            if (borderName == "visible") border = McpScreen::Border::Visible;
            else if (borderName == "none") border = McpScreen::Border::None;
            else if (borderName == "full") border = McpScreen::Border::Full;
            else return job.fail("border must be visible, none or full");
            unsigned scale = (unsigned)std::clamp<int64_t>(job.args.getInt("scale", 2), 1, 4);

            if (job.args.getBool("rendered")) {
                if (cmd->noGui || !sdlWindow || !videoDriver)
                    return job.fail("rendered=true needs a window");
                job.text = tempDir() + "/screenshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png";
                job.phase = Job::Ui;
                job.onUi = [](Job& job) {
                    if (!activeEmulator) return job.fail("No emulator running");
                    EmuLock lock;
                    auto& screenshot = program->screenshot;
                    screenshot.unscaled = 0;
                    screenshot.withEffects = true;
                    screenshot.pause = 0;
                    screenshot.twoFrames = false;
                    screenshot.type = GUIKIT::Image::Type::PNG;
                    screenshot.path = job.text;
                    screenshot.saveState = false;
                    screenshot.writePalette = false;
                    screenshot.gun = 0;
                    screenshot.interval = 1;
                    screenshot.intervalPos = 1;
                    VideoManager::takeScreenShots = 1;
                    job.finish();
                };
                job.postProcess = [](Job& job) {
                    // the video driver writes the file after the next rendered frame
                    for (int i = 0; i < 300 && fileSize(job.text) <= 0; i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    std::vector<uint8_t> data;
                    {
                        std::lock_guard<std::mutex> lock(program->screenshot.sharedMutex);
                        readFile(job.text, data);
                    }
                    ::unlink(job.text.c_str());
                    if (data.empty()) {
                        job.isError = true;
                        job.addText("No rendered frame captured (window hidden or video driver without screenshot support?)");
                        return;
                    }
                    job.addImage(Base64::encode(data.data(), data.size()));
                };
                return;
            }

            job.phase = Job::Emu;
            job.onEmu = [border, scale](Job& job) {
                if (!requireEmu(job)) return;
                std::vector<uint32_t> palette;
                if (activeVideoManager && activeVideoManager->palette) {
                    for (auto& col : activeVideoManager->palette->paletteColors)
                        palette.push_back(col.rgb);
                }
                if (palette.size() < 16) {
                    palette.clear();
                    for (auto& pal : activeEmulator->palettes)
                        if (pal.name == "Pepto PAL")
                            for (auto& col : pal.paletteColors)
                                palette.push_back(col.rgb);
                }
                std::string png, error;
                unsigned w, h;
                if (!McpScreen::screenshot(c64(), palette, border, scale, png, w, h, error))
                    return job.fail(error);
                job.addImage(png);
                job.addText(std::to_string(w) + "x" + std::to_string(h));
                job.finish();
            };
        });

    // ------------------------------------------------------------ overlay
    addTool("overlay",
        "Drive the overlay UI (F12 menu; pauses the emulation while open). action: "
        "'state' (default) describes it, 'open', 'close', 'toggle', "
        "'nav' presses keys (keys: up, down, left, right, ok, back, pageup, pagedown, home, end, backspace, delete, "
        "tab, shift_tab, insert (Shift+Return), bookmark (Ctrl+B), all_files (Ctrl+H), eject (Ctrl+E), parent (pad B)), "
        "'text' types into the text field being edited or the media browser filter, "
        "'click' / 'rightclick' / 'move' / 'wheel' use logical x/y (see rects in the state; media page: widgets[0].detail). "
        "'page' selects a sidebar page by id. "
        "Drag & drop tiles (also with the menu closed): 'drag' starts a file drag over the window, 'drag_move' hovers logical x/y, "
        "'drop' releases 'files' (paths) at the last position (or x/y if given), 'drag_leave' cancels; tiles and hover are in state.drag. "
        "Every call returns the overlay state. "
        "Visible in screenshot rendered=true only; also works headless (no drawing).",
        R"({"type":"object","properties":{)"
        R"("action":{"type":"string","enum":["state","open","close","toggle","nav","text","click","rightclick","move","wheel","page","drag","drag_move","drop","drag_leave"],"default":"state"},)"
        R"("files":{"type":"array","items":{"type":"string"},"description":"absolute paths for 'drop'"},)"
        R"("keys":{"type":"array","items":{"type":"string"}},)"
        R"("text":{"type":"string"},"page":{"type":"string"},)"
        R"("x":{"type":"integer"},"y":{"type":"integer"},"clicks":{"type":"integer","minimum":1,"maximum":2,"default":1},)"
        R"("dy":{"type":"integer","description":"wheel steps, positive = up"}}})",
        [](Job& job) {
            auto action = job.args.getString("action", "state");
            std::vector<Overlay::Nav> navs;
            if (action == "nav") {
                auto& keys = job.args["keys"];
                if (!keys.isArray() || keys.items.empty())
                    return job.fail("'keys' (array of key names) required");
                for (auto& k : keys.items) {
                    Overlay::Nav n;
                    if (!k.isString() || !Overlay::navFromName(k.string, n))
                        return job.fail("unknown key '" + k.string + "'");
                    navs.push_back(n);
                }
            } else if (action == "text" && !job.args["text"].isString())
                return job.fail("'text' required");
            else if ((action == "click" || action == "rightclick" || action == "move" || action == "wheel") && (!job.args["x"].isNumber() || !job.args["y"].isNumber()))
                return job.fail("'x' and 'y' (logical coordinates) required");
            else if (action == "page" && !job.args["page"].isString())
                return job.fail("'page' (sidebar id) required");
            else if (action == "drag_move" && (!job.args["x"].isNumber() || !job.args["y"].isNumber()))
                return job.fail("'x' and 'y' (logical coordinates) required");
            else if (action == "drop" && (!job.args["files"].isArray() || job.args["files"].items.empty()))
                return job.fail("'files' (array of paths) required");
            else if (action != "state" && action != "open" && action != "close" && action != "toggle" && action != "nav"
                && action != "text" && action != "click" && action != "rightclick" && action != "move" && action != "wheel" && action != "page"
                && action != "drag" && action != "drag_move" && action != "drop" && action != "drag_leave")
                return job.fail("unknown action '" + action + "'");

            job.phase = Job::Ui;
            job.onUi = [action, navs](Job& job) {
                if (!overlay) return job.fail("Overlay not available (fonts missing?)");
                bool dragAction = action == "drag" || action == "drag_move" || action == "drop" || action == "drag_leave";
                if (action != "state" && action != "open" && action != "toggle" && !dragAction && !overlay->isOpen())
                    return job.fail("Overlay is closed (use action 'open')");
                if (action != "drag" && action != "state" && dragAction && !overlay->isDragging())
                    return job.fail("No drag in progress (use action 'drag')");
                int x = (int)job.args.getInt("x"), y = (int)job.args.getInt("y");
                bool hasPos = job.args["x"].isNumber() && job.args["y"].isNumber();
                if (action == "open") overlay->open();
                else if (action == "close") overlay->close();
                else if (action == "toggle") overlay->toggle();
                else if (action == "nav") { for (auto n : navs) overlay->nav(n); }
                else if (action == "text") overlay->textInput(job.args.getString("text"));
                else if (action == "click") { overlay->pointer(x, y, 0); overlay->pointer(x, y, (int)std::clamp<int64_t>(job.args.getInt("clicks", 1), 1, 2)); }
                else if (action == "rightclick") { overlay->pointer(x, y, 0); overlay->rightClick(x, y); }
                else if (action == "move") overlay->pointer(x, y, 0);
                else if (action == "wheel") overlay->wheel(x, y, (int)job.args.getInt("dy", 1));
                else if (action == "page" && !overlay->selectPage(job.args.getString("page")))
                    return job.fail("unknown page '" + job.args.getString("page") + "'");
                else if (action == "drag") { overlay->dragBegin(); if (hasPos) overlay->dragMove(x, y); }
                else if (action == "drag_move") overlay->dragMove(x, y);
                else if (action == "drag_leave") overlay->dragEnd({});
                else if (action == "drop") {
                    std::vector<std::string> files;
                    for (auto& f : job.args["files"].items)
                        if (f.isString()) files.push_back(f.string);
                    if (hasPos) overlay->dragMove(x, y);
                    overlay->dragEnd(files);
                }
                Json state;
                overlay->describe(state);
                job.addText(state.dump());
                job.finish();
            };
        });
}
