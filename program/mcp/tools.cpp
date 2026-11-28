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
#include "../video/slang.h"
#include "../audio/manager.h"
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

// ---------------------------------------------------------------- pacing

auto round3(double v) -> double {
    return std::round(v * 1000.0) / 1000.0;
}

// Evaluates the presents of the Vulkan renderer: display intervals in refresh periods and how
// long each emulator picture stayed on screen (expected: `repeat` periods under video master).
auto pacingStats(const std::vector<DRIVER::PacingSample>& all, double refreshNs, unsigned repeat, double gridNs) -> Json {
    std::vector<const DRIVER::PacingSample*> shown;
    unsigned lost = 0, redraws = 0;
    for (auto& p : all) {
        if (p.flags & DRIVER::PACING_REDRAW) {
            redraws++;
            continue;
        }
        if (!p.displayNs || (p.flags & DRIVER::PACING_LOST)) {
            lost++;
            continue;
        }
        shown.push_back(&p);
    }
    Json out = Json::object();
    out.set("presents", (unsigned)all.size());
    out.set("presents_without_time", lost);
    out.set("redraws", redraws);
    if (shown.size() < 3) {
        out.set("note", "not enough presents with display time");
        return out;
    }

    std::vector<double> dts;
    for (size_t i = 1; i < shown.size(); i++)
        dts.push_back((double)(int64_t)(shown[i]->displayNs - shown[i - 1]->displayNs));
    if (refreshNs <= 0.0) {
        // estimate: median interval of the presents (one refresh each under video master)
        auto sorted = dts;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        refreshNs = sorted[sorted.size() / 2];
    }
    if (refreshNs <= 0.0) {
        out.set("note", "no refresh period (identical display times)");
        return out;
    }
    out.set("seconds", round3((double)(int64_t)(shown.back()->displayNs - shown.front()->displayNs) / 1e9));
    out.set("refresh_hz", round3(1e9 / refreshNs));

    // present intervals in refresh periods
    unsigned hist[5] = {0};
    double jitterMax = 0, jitterSum = 0;
    Json hitches = Json::array();
    for (size_t i = 0; i < dts.size(); i++) {
        double dt = dts[i];
        long periods = std::lround(dt / refreshNs);
        hist[std::clamp(periods, 0L, 4L)]++;
        // present shown later than one period after its predecessor: when was it submitted
        // relative to the predecessor's display (late submit = emulation/CPU side)
        if (periods > 1 && hitches.items.size() < 40) {
            auto* a = shown[i];
            auto* b = shown[i + 1];
            Json h = Json::object();
            h.set("t", round3((double)(b->displayNs - shown.front()->displayNs) / 1e9));
            h.set("periods", (int64_t)periods);
            h.set("submit_after_prev_display_ms", round3((double)((int64_t)b->submitNs - (int64_t)a->displayNs) / 1e6));
            h.set("submit_gap_ms", round3((double)((int64_t)b->submitNs - (int64_t)a->submitNs) / 1e6));
            h.set("copy", b->copy);
            h.set("display_ns", b->displayNs);
            hitches.push(h);
        }
        double err = std::abs(dt - periods * refreshNs) / 1e6;
        jitterMax = std::max(jitterMax, err);
        jitterSum += err;
    }
    Json intervals = Json::object();
    intervals.set("0", hist[0]);
    intervals.set("1", hist[1]);
    intervals.set("2", hist[2]);
    intervals.set("3", hist[3]);
    intervals.set(">=4", hist[4]);
    out.set("present_intervals", intervals);
    out.set("timestamp_jitter_ms_avg", round3(jitterSum / dts.size()));
    out.set("timestamp_jitter_ms_max", round3(jitterMax));
    out.set("hitches", hitches);

    // emulator pictures: periods between the first presents of consecutive pictures
    std::vector<const DRIVER::PacingSample*> firsts;
    for (auto* p : shown)
        if (p->copy == 0)
            firsts.push_back(p);
    unsigned frameHist[6] = {0};
    unsigned longer = 0, shorter = 0, skipped = 0;
    for (size_t i = 1; i < firsts.size(); i++) {
        long periods = std::lround((double)(int64_t)(firsts[i]->displayNs - firsts[i - 1]->displayNs) / refreshNs);
        frameHist[std::clamp(periods, 0L, 5L)]++;
        uint64_t gap = firsts[i]->frame - firsts[i - 1]->frame;
        if (gap > 1)
            skipped += (unsigned)(gap - 1); // pictures without any displayed present
        if (gap == 1) {
            if (periods > (long)repeat) longer++;
            if (periods < (long)repeat) shorter++;
        }
    }
    Json frames = Json::object();
    frames.set("count", (unsigned)firsts.size());
    Json fh = Json::object();
    for (int i = 0; i < 5; i++)
        fh.set(std::to_string(i), frameHist[i]);
    fh.set(">=5", frameHist[5]);
    frames.set("periods_on_screen", fh);
    frames.set("expected_periods", repeat);
    frames.set("repeated", longer);
    frames.set("shortened", shorter);
    frames.set("skipped", skipped);
    out.set("frames", frames);

    // submit -> display (same clock: CLOCK_MONOTONIC)
    double latSum = 0, latMax = 0;
    for (auto* p : shown) {
        double l = (double)(int64_t)(p->displayNs - p->submitNs) / 1e6;
        latSum += l;
        latMax = std::max(latMax, l);
    }
    out.set("present_to_display_ms_avg", round3(latSum / shown.size()));
    out.set("present_to_display_ms_max", round3(latMax));

    // grid (target times): interval between consecutive pictures vs. the grid period
    // (with VRR every interval is one period, acceptance tolerance 1 ms)
    if (gridNs > 0.0) {
        Json g = Json::object();
        unsigned n = 0, within = 0, longer = 0, shorter = 0;
        double devMax = 0, devSum = 0;
        Json outliers = Json::array();
        Json gaps = Json::array();
        for (size_t i = 1; i < firsts.size(); i++) {
            if (firsts[i]->frame != firsts[i - 1]->frame + 1) {
                // pictures without a displayed present (e.g. swapchain rebuilt on a resize)
                if (gaps.items.size() < 1000) {
                    Json o = Json::object();
                    o.set("display_ns", firsts[i]->displayNs);
                    o.set("missing", (uint64_t)(firsts[i]->frame - firsts[i - 1]->frame - 1));
                    gaps.push(o);
                }
                continue;
            }
            double dt = (double)(int64_t)(firsts[i]->displayNs - firsts[i - 1]->displayNs);
            double dev = (dt - gridNs) / 1e6;
            n++;
            devSum += std::abs(dev);
            devMax = std::max(devMax, std::abs(dev));
            if (std::abs(dev) <= 1.0)
                within++;
            else {
                if (dev > 0) longer++; else shorter++;
                // all of them (~10 min window cap in the driver): window/fullscreen phases are
                // evaluated per time range by tests/pacing_check.py
                if (outliers.items.size() < 20000) {
                    Json o = Json::object();
                    o.set("t", round3((double)(firsts[i]->displayNs - firsts.front()->displayNs) / 1e9));
                    o.set("interval_ms", round3(dt / 1e6));
                    o.set("submit_gap_ms", round3((double)((int64_t)firsts[i]->submitNs - (int64_t)firsts[i - 1]->submitNs) / 1e6));
                    o.set("present_to_display_ms", round3((double)((int64_t)firsts[i]->displayNs - (int64_t)firsts[i]->submitNs) / 1e6));
                    o.set("display_ns", firsts[i]->displayNs);
                    outliers.push(o);
                }
            }
        }
        g.set("period_ms", round3(gridNs / 1e6));
        g.set("intervals", n);
        g.set("within_1ms", within);
        g.set("longer", longer);
        g.set("shorter", shorter);
        g.set("skipped", skipped);
        if (n) {
            g.set("deviation_ms_avg", round3(devSum / n));
            g.set("deviation_ms_max", round3(devMax));
        }
        g.set("outliers", outliers);
        g.set("gaps", gaps);
        out.set("grid", g);
    }

    // blend (M-Pacing3): each present was mixed for a predicted vblank; the phase error is its
    // display time minus that vblank, consecutive blend presents should be one refresh apart
    std::vector<const DRIVER::PacingSample*> blends;
    for (auto* p : shown)
        if ((p->flags & DRIVER::PACING_BLEND) && p->targetNs)
            blends.push_back(p);
    if (!blends.empty()) {
        Json b = Json::object();
        unsigned within = 0, notOne = 0;
        double errMax = 0, errSum = 0;
        Json outliers = Json::array();
        for (size_t i = 0; i < blends.size(); i++) {
            double err = (double)(int64_t)(blends[i]->displayNs - blends[i]->targetNs) / 1e6;
            errSum += std::abs(err);
            errMax = std::max(errMax, std::abs(err));
            bool ok = std::abs(err) < 1.0;
            if (ok)
                within++;
            if (i > 0) {
                long periods = std::lround((double)(int64_t)(blends[i]->displayNs - blends[i - 1]->displayNs) / refreshNs);
                if (periods != 1) {
                    notOne++;
                    ok = false;
                }
            }
            if (!ok && outliers.items.size() < 20000) {
                Json o = Json::object();
                o.set("t", round3((double)(blends[i]->displayNs - blends.front()->displayNs) / 1e9));
                o.set("phase_error_ms", round3(err));
                if (i > 0)
                    o.set("interval_ms", round3((double)(int64_t)(blends[i]->displayNs - blends[i - 1]->displayNs) / 1e6));
                o.set("present_to_display_ms", round3((double)((int64_t)blends[i]->displayNs - (int64_t)blends[i]->submitNs) / 1e6));
                o.set("display_ns", blends[i]->displayNs);
                outliers.push(o);
            }
        }
        b.set("presents", (unsigned)blends.size());
        b.set("phase_within_1ms", within);
        b.set("phase_error_ms_avg", round3(errSum / blends.size()));
        b.set("phase_error_ms_max", round3(errMax));
        b.set("intervals_not_one_refresh", notOne);
        b.set("outliers", outliers);
        out.set("blend", b);
    }
    return out;
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

    // ------------------------------------------------------------ renderer
    addTool("renderer",
        "Get or switch the video renderer (window mode only). Without 'set' it reports the active one; "
        "'set' switches at runtime (the window is recreated, the emulation state stays). "
        "A failed switch restores the previous renderer and returns an error.",
        R"({"type":"object","properties":{"set":{"type":"string","enum":["gl","vulkan"]}}})",
        [](Job& job) {
            std::string set = job.args.getString("set");
            if (!set.empty() && set != "gl" && set != "vulkan")
                return job.fail("set must be 'gl' or 'vulkan'");
            job.phase = Job::Ui;
            job.onUi = [set](Job& job) {
                if (!set.empty()) {
                    std::string error;
                    if (!program->switchRenderer(set == "vulkan" ? "Vulkan" : "OpenGL", error))
                        return job.fail("Renderer switch failed: " + error);
                }
                Json s = Json::object();
                bool headless = cmd->noGui || cmd->noDriver;
                s.set("renderer", headless ? "none" : (program->getVideoDriver() == "Vulkan" ? "vulkan" : "gl"));
                std::string device = videoDriver->deviceName();
                if (!device.empty())
                    s.set("device", device);
                job.addText(s.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ shader
    addTool("shader",
        "RetroArch slang shader preset of the Vulkan renderer (window mode). Without arguments it reports "
        "the active preset (path, passes, parameters with values; null = none), the load status and the "
        "preset sources (system collection, downloaded collection). 'load' parses, compiles (disk cache "
        "~/.cache/badline/shaders) and activates a .slangp file, saves it in the C64 settings "
        "(video_slang_preset) and returns its summary (passes, LUTs, history depth, compile statistics, "
        "warnings); 'clear' removes it (saved too); 'set_param' with 'value' changes a parameter of the "
        "active preset (not saved).",
        R"({"type":"object","properties":{)"
        R"("load":{"type":"string","description":"Absolute path of a .slangp preset"},)"
        R"("clear":{"type":"boolean"},)"
        R"("set_param":{"type":"string","description":"Parameter id"},)"
        R"("value":{"type":"number"}}})",
        [](Job& job) {
            std::string load = job.args.getString("load");
            bool clear = job.args.getBool("clear");
            std::string param = job.args.getString("set_param");
            if (!param.empty() && !job.args["value"].isNumber())
                return job.fail("'value' (number) required with set_param");
            double value = job.args.getNumber("value");
            job.phase = Job::Ui;
            job.onUi = [load, clear, param, value](Job& job) {
                if (!SlangPresets::supported())
                    return job.fail("Shader presets need the Vulkan renderer in window mode");
                if (!load.empty() || clear) {
                    std::string result;
                    if (!SlangPresets::loadNow(clear ? "" : load, true, result))
                        return job.fail("Preset failed: " + result);
                    job.addText(result);
                    return job.finish();
                }
                if (!param.empty() && !videoDriver->setSlangParam(param, (float)value))
                    return job.fail("No such parameter in the active preset: " + param);
                SlangPresets::wait();
                auto st = SlangPresets::state();
                Json s = Json::object();
                std::string parseError;
                Json active;
                if (!Json::parse(videoDriver->slangState(), active, parseError))
                    active = Json();
                s.set("active", active);
                s.set("status", st.status);
                s.set("error", st.error);
                s.set("wanted", SlangPresets::wanted()); // -shader or the setting
                s.set("system_dir", SlangPresets::systemDir());
                s.set("downloaded_dir", SlangPresets::downloadedDir());
                job.addText(s.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ picture
    addTool("picture",
        "Picture settings of the C64 colour path (saved in the C64 settings). The Vulkan renderer decodes the "
        "colour indices on the GPU: palette, saturation/contrast/brightness (0..200, 100 = neutral), gamma "
        "(30..280, 100 = 1.0) and the C64 PAL/NTSC pass (pal_pass: per-line phase error in degrees, PAL delay "
        "line with Hanover bars -50..50, luma rise/fall delay in pixels 1..4, chroma low pass taps 0 = off or "
        "3..21). GL and raw screenshots show the plain palette. Without arguments it reports the settings, the "
        "palettes, the region and the raster parity of the first picture line.",
        R"({"type":"object","properties":{)"
        R"("palette":{"type":"integer","description":"Palette id"},)"
        R"("saturation":{"type":"integer"},"contrast":{"type":"integer"},"brightness":{"type":"integer"},)"
        R"("gamma":{"type":"integer"},"pal_pass":{"type":"boolean"},)"
        R"("pal_phase_error":{"type":"number"},"ntsc_phase_error":{"type":"number"},)"
        R"("hanover":{"type":"integer"},"luma_rise":{"type":"number"},"luma_fall":{"type":"number"},)"
        R"("chroma_taps":{"type":"integer"}}})",
        [](Job& job) {
            Json args = job.args;
            job.phase = Job::Ui;
            job.onUi = [args](Job& job) {
                if (!activeVideoManager)
                    return job.fail("Picture settings need a window (no video output headless)");
                auto vm = activeVideoManager;
                EmuLock lock;
                auto& p = vm->picture;
                auto num = [&](const char* key) { return args.has(key) && args[key].isNumber(); };
                if (num("palette")) {
                    unsigned id = (unsigned)args.getInt("palette");
                    bool found = false;
                    for (auto& pal : vm->emulator->palettes)
                        found |= pal.id == id;
                    if (!found)
                        return job.fail("No such palette id");
                    vm->settings->set<unsigned>("palette", id);
                    program->setPalette(vm->emulator);
                }
                if (num("saturation")) p.saturation = (unsigned)std::clamp<int64_t>(args.getInt("saturation"), 0, 200);
                if (num("contrast")) p.contrast = (unsigned)std::clamp<int64_t>(args.getInt("contrast"), 0, 200);
                if (num("brightness")) p.brightness = (unsigned)std::clamp<int64_t>(args.getInt("brightness"), 0, 200);
                if (num("gamma")) p.gamma = (unsigned)std::clamp<int64_t>(args.getInt("gamma"), 30, 280);
                if (args.has("pal_pass") && args["pal_pass"].isBool()) p.palPass = args.getBool("pal_pass");
                if (num("pal_phase_error")) p.phaseErrorPal = (float)std::clamp(args.getNumber("pal_phase_error"), -45.0, 45.0);
                if (num("ntsc_phase_error")) p.phaseErrorNtsc = (float)std::clamp(args.getNumber("ntsc_phase_error"), -45.0, 45.0);
                if (num("hanover")) p.hanover = (int)std::clamp<int64_t>(args.getInt("hanover"), -50, 50);
                if (num("luma_rise")) p.lumaRise = (float)std::clamp(args.getNumber("luma_rise"), 1.0, 4.0);
                if (num("luma_fall")) p.lumaFall = (float)std::clamp(args.getNumber("luma_fall"), 1.0, 4.0);
                if (num("chroma_taps")) p.chromaTaps = (unsigned)std::clamp<int64_t>(args.getInt("chroma_taps"), 0, 21);
                vm->savePicture();

                Json s = Json::object();
                s.set("palette", (int64_t)vm->palette->id);
                s.set("palette_name", vm->palette->name);
                Json colors = Json::array();
                char hex[8];
                for (unsigned c = 0; c < 16; c++) {
                    std::snprintf(hex, sizeof hex, "%06x", vm->colorTable[c] & 0xffffff);
                    colors.push(std::string(hex));
                }
                s.set("colors", colors);
                Json pals = Json::array();
                for (auto& pal : vm->emulator->palettes) {
                    Json e = Json::object();
                    e.set("id", (int64_t)pal.id);
                    e.set("name", pal.name);
                    pals.push(e);
                }
                s.set("palettes", pals);
                s.set("saturation", (int64_t)p.saturation);
                s.set("contrast", (int64_t)p.contrast);
                s.set("brightness", (int64_t)p.brightness);
                s.set("gamma", (int64_t)p.gamma);
                s.set("pal_pass", p.palPass);
                s.set("pal_phase_error", (double)p.phaseErrorPal);
                s.set("ntsc_phase_error", (double)p.phaseErrorNtsc);
                s.set("hanover", (int64_t)p.hanover);
                s.set("luma_rise", (double)p.lumaRise);
                s.set("luma_fall", (double)p.lumaFall);
                s.set("chroma_taps", (int64_t)p.chromaTaps);
                bool ntsc = vm->emulator->getRegionEncoding() == Emulator::Interface::Region::Ntsc;
                s.set("region", ntsc ? "NTSC" : "PAL");
                s.set("first_line_odd", (activeEmulator->cropTop() & 1) != 0);
                s.set("gpu_decode", program->getVideoDriver() == "Vulkan" && !cmd->noGui && !cmd->noDriver);
                job.addText(s.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ pacing
    addTool("pacing",
        "Frame pacing of the Vulkan renderer. 'status' (default): pacing decision (video_master = display "
        "clocks the emulation, each picture presented 'repeat' times; grid / grid_vrr = pictures presented at "
        "target times of a steady grid at the emulated rate, grid_vrr when the display follows it (VRR); "
        "blend = grid without VRR, every vblank shows a phase-weighted mix of two pictures; "
        "audio_master; lowest_latency; "
        "unthrottled), refresh/emulation rates, present feedback. 'reset' clears the collected presents and "
        "audio counters; 'stats' evaluates the presents since the last reset (display intervals in refresh "
        "periods, pictures shown longer/shorter than expected, skipped pictures, blend phase errors, audio DRC/xruns). "
        "'set_mode' changes the setting pacing (auto | lowest_latency).",
        R"({"type":"object","properties":{)"
        R"("action":{"type":"string","enum":["status","reset","stats"],"default":"status"},)"
        R"("set_mode":{"type":"string","enum":["auto","lowest_latency"]}}})",
        [](Job& job) {
            std::string action = job.args.getString("action", "status");
            std::string mode = job.args.getString("set_mode");
            if (action != "status" && action != "reset" && action != "stats")
                return job.fail("action must be status, reset or stats");
            if (!mode.empty() && mode != "auto" && mode != "lowest_latency")
                return job.fail("set_mode must be auto or lowest_latency");
            job.phase = Job::Ui;
            job.onUi = [action, mode](Job& job) {
                if (!videoDriver || !videoDriver->supportsPacing())
                    return job.fail("Pacing needs the Vulkan renderer in window mode");
                if (!mode.empty()) {
                    EmuLock modeLock;
                    globalSettings->set<unsigned>("pacing", mode == "auto" ? 0u : 1u);
                    VideoManager::setSynchronize();
                }
                auto caps = videoDriver->pacingCaps();
                // the emu thread rewrites the decision and the DRC statistics
                EmuLock lock;
                auto p = VideoManager::pacing;
                Json s = Json::object();
                s.set("state", VideoManager::pacingStateName());
                s.set("mode", p.mode == 1 ? "lowest_latency" : "auto");
                s.set("repeat", p.repeat);
                s.set("queue_depth", p.queueDepth);
                if (p.gridNs)
                    s.set("grid_period_ms", round3((double)p.gridNs / 1e6));
                s.set("absolute_time", caps.absoluteTime);
                if (p.stalled)
                    s.set("stalled", true);
                s.set("refresh_hz", round3(p.refresh));
                s.set("emulation_fps", round3(p.fps));
                s.set("clock_factor", p.clock);
                s.set("feedback", caps.feedback == DRIVER::PACING_FEEDBACK_TIMING ? "present_timing"
                    : caps.feedback == DRIVER::PACING_FEEDBACK_WAIT ? "present_wait" : "none");
                if (caps.refreshNs)
                    s.set("timing_refresh_hz", round3(1e9 / (double)caps.refreshNs));
                if (caps.feedback == DRIVER::PACING_FEEDBACK_TIMING)
                    s.set("time_domain", caps.timeDomain);

                if (action == "reset") {
                    std::vector<DRIVER::PacingSample> drop;
                    videoDriver->pacingSamples(drop, true);
                    audioDriver->resetXruns();
                    audioManager->resetDrcStats();
                    s.set("reset", true);
                } else if (action == "stats") {
                    std::vector<DRIVER::PacingSample> samples;
                    videoDriver->pacingSamples(samples, false);
                    double refreshNs = caps.refreshNs ? (double)caps.refreshNs : (p.refresh > 0 ? 1e9 / p.refresh : 0);
                    s.set("video", pacingStats(samples, refreshNs, p.repeat, (double)p.gridNs));

                    Json a = Json::object();
                    unsigned under = 0, dropped = 0;
                    audioDriver->xruns(under, dropped);
                    a.set("underflows", under);
                    a.set("dropped_bytes", dropped);
                    a.set("drc", audioManager->dynamicRateControl);
                    auto& d = audioManager->drcStats;
                    if (d.count) {
                        a.set("buffer_deviation_min", round3(d.devMin));
                        a.set("buffer_deviation_max", round3(d.devMax));
                        a.set("buffer_deviation_avg", round3(d.devSum / d.count));
                        a.set("rate_adjust_min_percent", round3(d.adjMin * 100.0));
                        a.set("rate_adjust_max_percent", round3(d.adjMax * 100.0));
                    }
                    s.set("audio", a);
                }
                job.addText(s.dump());
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
        "'busy' turns the busy spinner id 'id' (default 'mcp') on or off ('on', default true), e.g. to test it without shader compilation. "
        "'screen_text' shows 'text' like a program message for 'duration' seconds (default 3, 0 = permanent like FPS; 'warn' highlights), for tests. "
        "state.screen_text is the current screen text (messages, FPS, DRC; hidden while the menu is open), state.busy the active spinner ids. "
        "Every call returns the overlay state. "
        "Visible in screenshot rendered=true only; also works headless (no drawing).",
        R"({"type":"object","properties":{)"
        R"("action":{"type":"string","enum":["state","open","close","toggle","nav","text","click","rightclick","move","wheel","page","drag","drag_move","drop","drag_leave","busy","screen_text"],"default":"state"},)"
        R"("id":{"type":"string","description":"busy spinner id for 'busy'"},"on":{"type":"boolean","default":true},)"
        R"("duration":{"type":"integer","minimum":0,"default":3},"warn":{"type":"boolean","default":false},)"
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
            else if (action == "screen_text" && !job.args["text"].isString())
                return job.fail("'text' required");
            else if (action == "page" && !job.args["page"].isString())
                return job.fail("'page' (sidebar id) required");
            else if (action == "drag_move" && (!job.args["x"].isNumber() || !job.args["y"].isNumber()))
                return job.fail("'x' and 'y' (logical coordinates) required");
            else if (action == "drop" && (!job.args["files"].isArray() || job.args["files"].items.empty()))
                return job.fail("'files' (array of paths) required");
            else if (action != "state" && action != "open" && action != "close" && action != "toggle" && action != "nav"
                && action != "text" && action != "click" && action != "rightclick" && action != "move" && action != "wheel" && action != "page"
                && action != "drag" && action != "drag_move" && action != "drop" && action != "drag_leave" && action != "busy" && action != "screen_text")
                return job.fail("unknown action '" + action + "'");

            job.phase = Job::Ui;
            job.onUi = [action, navs](Job& job) {
                if (!overlay) return job.fail("Overlay not available (fonts missing?)");
                bool dragAction = action == "drag" || action == "drag_move" || action == "drop" || action == "drag_leave";
                if (action != "state" && action != "open" && action != "toggle" && action != "busy" && action != "screen_text" && !dragAction && !overlay->isOpen())
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
                else if (action == "busy") overlay->busy(job.args.getString("id", "mcp"), job.args.getBool("on", true));
                else if (action == "screen_text")
                    overlay->screenText(job.args.getString("text"), (unsigned)std::clamp<int64_t>(job.args.getInt("duration", 3), 0, 60), job.args.getBool("warn"));
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
