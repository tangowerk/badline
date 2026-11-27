// MCP debugging tools: breakpoints, continue/step, registers, memory, disassembler, chip state.
// See server.h for the threading model. Everything touching the emulator runs on the emu thread
// (phase Emu between frames or inside the stop loop, phase Frames, onStop inside the CPU).

#include "server.h"
#include "../program.h"
#include "../thread/emuThread.h"
#include "../../emulation/libc64/interface.h"
#include "../../emulation/libc64/system/system.h"
#include "../../emulation/libc64/system/debuggerSnapshot.h"
#include "../../emulation/libc64/disk/iec.h"
#include "../../emulation/libc64/disk/drive/drive.h"
#include "../../emulation/libc64/expansionPort/expansionPort.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>

using Job = McpServer::Job;
using Theme = Emulator::Interface::DebuggerTheme;
using Action = Emulator::Interface::DebuggerAction;

namespace {

const unsigned IDENT_BASE = 0x40000000;

auto c64() -> LIBC64::Interface* {
    return dynamic_cast<LIBC64::Interface*>(activeEmulator);
}

auto sys() -> LIBC64::System* {
    auto emu = c64();
    return emu ? emu->system : nullptr;
}

auto hex2(unsigned v) -> std::string {
    char buf[4];
    std::snprintf(buf, sizeof buf, "%02x", v & 0xff);
    return buf;
}

auto hex4(unsigned v) -> std::string {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04x", v & 0xffff);
    return buf;
}

auto addr(unsigned v) -> std::string { return "$" + hex4(v); }
auto byte(unsigned v) -> std::string { return "$" + hex2(v); }

auto fps() -> double {
    if (activeEmulator && activeEmulator->getRegionGeometry() == Emulator::Interface::Region::Ntsc)
        return 59.826;
    return 50.125;
}

// ---------------------------------------------------------------- argument parsing

// number, or string "$c000" / "0xc000" / "c000" (hex) / "#49152" (decimal)
auto parseValue(const Json& v, unsigned& out, std::string& error, const std::string& name, unsigned max = 0xffff) -> bool {
    if (v.isNumber()) {
        if (v.number < 0 || v.number > max || v.number != (double)(int64_t)v.number) {
            error = "'" + name + "' out of range";
            return false;
        }
        out = (unsigned)v.number;
        return true;
    }
    if (!v.isString()) {
        error = "'" + name + "' must be a number or hex string";
        return false;
    }
    std::string s = v.string;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    int radix = 16;
    if (i < s.size() && s[i] == '$') i++;
    else if (i < s.size() && s[i] == '#') { radix = 10; i++; }
    else if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) i += 2;
    if (i >= s.size()) {
        error = "'" + name + "' is empty";
        return false;
    }
    uint64_t value = 0;
    for (; i < s.size(); i++) {
        char c = (char)std::tolower((unsigned char)s[i]);
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (radix == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else {
            error = "'" + name + "': invalid number '" + v.string + "'";
            return false;
        }
        value = value * radix + d;
        if (value > max) {
            error = "'" + name + "' out of range";
            return false;
        }
    }
    out = (unsigned)value;
    return true;
}

// cpu index: 0 = C64, 1..4 = drive 8..11
auto parseCpu(const Json& args, int& cpu, std::string& error) -> bool {
    auto name = args.getString("cpu", "c64");
    for (auto& c : name) c = (char)std::tolower((unsigned char)c);
    if (name == "c64" || name == "cpu" || name == "main") { cpu = 0; return true; }
    for (int i = 0; i < 4; i++) {
        if (name == "drive" + std::to_string(8 + i) || name == std::to_string(8 + i)) { cpu = i + 1; return true; }
    }
    error = "cpu must be c64 or drive8..drive11";
    return false;
}

auto cpuName(int cpu) -> std::string {
    return cpu == 0 ? "c64" : "drive" + std::to_string(7 + cpu);
}

auto cpuTheme(int cpu) -> Theme {
    static const Theme themes[] = {Theme::CPU, Theme::Drive8CPU, Theme::Drive9CPU, Theme::Drive10CPU, Theme::Drive11CPU};
    return themes[cpu];
}

auto memTheme(int cpu) -> Theme {
    static const Theme themes[] = {Theme::CPU, Theme::Drive8Memory, Theme::Drive9Memory, Theme::Drive10Memory, Theme::Drive11Memory};
    return themes[cpu];
}

auto themeCpu(Theme theme) -> int {
    switch (theme) {
        case Theme::Drive8CPU: return 1;
        case Theme::Drive9CPU: return 2;
        case Theme::Drive10CPU: return 3;
        case Theme::Drive11CPU: return 4;
        default: return 0;
    }
}

auto drive(int cpu) -> LIBC64::Drive* {
    auto s = sys();
    if (!s || cpu < 1 || cpu > 4 || (size_t)cpu > s->iecBus.drives.size())
        return nullptr;
    auto d = s->iecBus.drives[cpu - 1];
    auto& enabled = s->iecBus.drivesEnabled;
    return std::find(enabled.begin(), enabled.end(), d) != enabled.end() ? d : nullptr;
}

auto debuggerOf(int cpu) -> LIBC64::M65Debugger* {
    auto s = sys();
    if (!s) return nullptr;
    if (cpu == 0) return &s->cpu;
    auto d = drive(cpu);
    return d ? &d->cpu : nullptr;
}

// validates emulator + cpu, fails the job otherwise
auto requireCpu(Job& job, int cpu) -> bool {
    if (!sys()) {
        job.fail("No emulator running");
        return false;
    }
    if (sys()->expansionPort->haltMainCpu()) {
        job.fail("SuperCPU active: not supported by the MCP debugger yet");
        return false;
    }
    if (cpu > 0 && !drive(cpu)) {
        job.fail(cpuName(cpu) + " is not enabled");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- registers

LIBC64::DebuggerSnapshot& scratch() {
    static LIBC64::DebuggerSnapshot* snap = new LIBC64::DebuggerSnapshot;
    return *snap;
}

struct Regs { unsigned pc, a, x, y, sp, flags; };

auto readRegs(int cpu) -> Regs {
    auto& snap = scratch();
    if (cpu == 0) {
        sys()->cpu.updateSnapshot(snap);
        return {snap.pcEdge & 0xffffu, snap.regA & 0xffu, snap.regX & 0xffu, snap.regY & 0xffu, snap.regS & 0xffu, snap.flags};
    }
    auto d = drive(cpu);
    d->cpu.updateSnapshot(snap);
    auto& r = snap.drives[d->number];
    return {r.pcEdge & 0xffffu, r.regA & 0xffu, r.regX & 0xffu, r.regY & 0xffu, r.regS & 0xffu, r.flags};
}

auto writeRegs(int cpu, const Regs& regs) -> void {
    auto& snap = scratch();
    if (cpu == 0) {
        auto& c = sys()->cpu;
        c.updateSnapshot(snap);
        snap.pc = regs.pc; snap.regA = regs.a; snap.regX = regs.x; snap.regY = regs.y; snap.regS = regs.sp; snap.flags = regs.flags;
        c.updateFromSnapshot(snap);
        return;
    }
    auto d = drive(cpu);
    d->cpu.updateSnapshot(snap);
    auto& r = snap.drives[d->number];
    r.pc = regs.pc; r.regA = regs.a; r.regX = regs.x; r.regY = regs.y; r.regS = regs.sp; r.flags = regs.flags;
    d->cpu.updateFromSnapshot(snap);
}

auto flagString(unsigned p) -> std::string {
    const char* names = "NV-BDIZC";
    std::string out;
    for (int i = 0; i < 8; i++)
        out += (p & (0x80 >> i)) ? names[i] : (char)std::tolower((unsigned char)names[i]);
    return out;
}

auto disassembleLine(int cpu, unsigned address, unsigned& bytes) -> std::string {
    return activeEmulator->disassemble(cpuTheme(cpu), address & 0xffff, bytes);
}

auto regsJson(int cpu) -> Json {
    auto r = readRegs(cpu);
    Json j = Json::object();
    j.set("pc", addr(r.pc));
    j.set("a", byte(r.a));
    j.set("x", byte(r.x));
    j.set("y", byte(r.y));
    j.set("sp", byte(r.sp));
    j.set("flags", flagString(r.flags));
    if (cpu == 0) {
        auto s = sys();
        auto& snap = scratch();
        s->cpu.updateSnapshot(snap);
        j.set("port_ddr", byte(snap.ddr));
        j.set("port", byte(snap.por));
        j.set("raster_line", (unsigned)s->vicII->getVcounterHR());
        j.set("raster_cycle", (unsigned)s->vicII->getCycle());
    }
    unsigned bytes = 0;
    j.set("next", disassembleLine(cpu, r.pc, bytes));
    return j;
}

auto reasonName(Action action) -> std::string {
    switch (action) {
        case Action::Breakpoint: return "breakpoint";
        case Action::Watchpoint: return "watchpoint_read";
        case Action::WatchpointWrite: return "watchpoint_write";
        case Action::ExceptionPoint: return "exception";
        case Action::Softstop: return "step";
        case Action::UIRequestedStop: return "ui_stop";
        case Action::Line: return "raster_line";
        case Action::Frame: return "frame";
        case Action::HaltCPU: return "halt";
        default: return "other";
    }
}

auto breakpointName(McpServer& server, unsigned ident) -> Json {
    if (ident >= IDENT_BASE)
        return Json((int64_t)(ident - IDENT_BASE + 1));
    return Json("gui:" + std::to_string(ident));
}

// full stop report (emu thread, CPU stopped or inside the stop callback)
auto stopReport(McpServer& server) -> Json {
    McpServer::StopInfo info;
    {
        std::lock_guard<std::mutex> lock(server.stopMutex);
        info = server.lastStop;
    }
    Json j = Json::object();
    j.set("stopped", true);
    j.set("reason", info.reason);
    j.set("cpu", info.cpu);
    j.set("address", addr(info.address));
    if (!info.idents.empty() && info.reason != "step") {
        Json ids = Json::array();
        for (auto id : info.idents)
            ids.push(breakpointName(server, id));
        j.set("breakpoints", ids);
    }
    int cpu = info.cpu == "c64" ? 0 : (info.cpu.size() > 5 ? std::atoi(info.cpu.c_str() + 5) - 7 : 0);
    if (cpu < 0 || cpu > 4 || (cpu > 0 && !drive(cpu)))
        cpu = 0;
    j.set("registers", regsJson(cpu));
    return j;
}

// ---------------------------------------------------------------- breakpoints

auto actionOf(const std::string& type, Action& action) -> bool {
    if (type == "exec") action = Action::Breakpoint;
    else if (type == "read") action = Action::Watchpoint;
    else if (type == "write") action = Action::WatchpointWrite;
    else return false;
    return true;
}

auto typeName(Action action) -> std::string {
    switch (action) {
        case Action::Breakpoint: return "exec";
        case Action::Watchpoint: return "read";
        case Action::WatchpointWrite: return "write";
        case Action::ExceptionPoint: return "exception";
        default: return "other";
    }
}

auto watchList(LIBC64::M65Debugger* dbg, Action action) -> Emulator::WatchPoints* {
    switch (action) {
        case Action::Breakpoint: return &dbg->breakPoints;
        case Action::Watchpoint: return &dbg->watchPoints;
        case Action::WatchpointWrite: return &dbg->watchPointsWrite;
        case Action::ExceptionPoint: return &dbg->exceptionPoints;
        default: return nullptr;
    }
}

// drops MCP entries the emulator no longer has (e.g. cleared in the GUI debugger)
auto prune(McpServer& server) -> void {
    auto& list = server.breakpoints;
    list.erase(std::remove_if(list.begin(), list.end(), [](const McpServer::Breakpoint& bp) {
        auto dbg = debuggerOf(themeCpu((Theme)bp.theme));
        auto wl = dbg ? watchList(dbg, (Action)bp.action) : nullptr;
        return !wl || !wl->find(bp.ident);
    }), list.end());
}

auto readFile(const std::string& path, std::vector<uint8_t>& out) -> bool {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

auto petsciiChar(uint8_t c) -> char {
    if (c >= 0x20 && c <= 0x5f) return (char)c;
    if (c >= 0xc1 && c <= 0xda) return (char)(c - 0x80);
    return '.';
}

// ---------------------------------------------------------------- run control helpers

// emu thread: leave the stop loop (GUI: lockDebugger, headless: loopStopped)
auto resumeCpu() -> void {
    if (emuThread->debugging)
        emuThread->unlockDebugger();
}

auto issueStep(int cpu, const std::string& mode) -> bool {
    auto theme = cpuTheme(cpu);
    if (mode == "into") {
        activeEmulator->debuggerStepInto(theme);
    } else if (mode == "over") {
        activeEmulator->debuggerStepOver(theme, true);
    } else {
        return activeEmulator->debuggerStepOut(theme);
    }
    return true;
}

// forget a pending step (e.g. a step over interrupted by a breakpoint), otherwise it stops later
auto clearSoftStop(int cpu) -> void {
    auto dbg = debuggerOf(cpu);
    if (!dbg) return;
    dbg->softStep = std::nullopt;
    dbg->flagDebugAction(LIBC64::M65Debugger::SoftStop, false);
}

auto clearAllSoftStops() -> void {
    for (int cpu = 0; cpu <= 4; cpu++)
        clearSoftStop(cpu);
}

// drives only run instruction-exact against the C64 when their debugger theme is active
auto enableCpuTheme(int cpu) -> void {
    if (cpu > 0)
        activeEmulator->debuggerAdd(cpuTheme(cpu), Action::None, 0);
}

auto finishWithStop(McpServer& server, Job& job) -> void {
    job.addText(stopReport(server).dump());
    if (job.warpBefore >= 0)
        job.phase = Job::UiFinish;
    else
        job.finish();
}

auto beginWarp(Job& job) -> void {
    bool locked = emuThread->lock();
    job.warpBefore = (int)program->warp.mode;
    if (program->warp.mode == Program::Warp::Off)
        program->setWarp(Program::Warp::Normal, true);
    if (locked) emuThread->unlock();
}

auto endWarp(Job& job) -> void {
    if (job.warpBefore < 0)
        return;
    bool locked = emuThread->lock();
    if ((Program::Warp::Mode)job.warpBefore == Program::Warp::Off && program->warp.mode != Program::Warp::Off)
        program->setWarp(Program::Warp::Off);
    job.warpBefore = -1;
    if (locked) emuThread->unlock();
}

} // namespace

// ---------------------------------------------------------------- server hooks

auto McpServer::cpuStopped() -> bool {
    return emuThread && emuThread->debugging;
}

auto McpServer::stopStatus() -> Json {
    return stopReport(*this);
}

auto McpServer::stopLoopShouldExit() -> bool {
    if (quitRequested)
        return true;
    auto job = currentJob();
    return job && job->phase == Job::Ui && job->resumesCpu;
}

auto McpServer::debuggerEvent(void* snapshotPtr) -> StopDecision {
    auto snap = static_cast<Emulator::Interface::DebuggerSnapshot*>(snapshotPtr);
    if (snap->callbackAction == Action::AutoUpdate || snap->callbackAction == Action::None)
        return StopDecision::Ignore;

    // record every stop (also those owned by the GUI debugger), status reports it
    {
        std::lock_guard<std::mutex> lock(stopMutex);
        lastStop.valid = true;
        lastStop.reason = reasonName(snap->callbackAction);
        lastStop.cpu = cpuName(themeCpu(snap->callbackTheme));
        lastStop.address = snap->callbackAddress & 0xffff;
        lastStop.idents = snap->watcherIdents;
        lastStop.frame = frameCounter;
    }

    auto job = currentJob();
    bool jobWaits = job && job->onStop && job->phase == Job::Frames && !job->cancelled;
    if (!jobWaits && !stepPending && breakpoints.empty())
        return StopDecision::Ignore;
    stepPending = false;

    if (jobWaits && job->onStop(*job))
        return StopDecision::Continue;
    return StopDecision::Stop;
}

auto McpServer::registerDebugTools() -> void {

    static const char* CPU_PROP = R"("cpu":{"type":"string","enum":["c64","drive8","drive9","drive10","drive11"],"default":"c64"})";
    static const char* ADDR_DESC = R"(Number or hex string: \"$c000\", \"0xc000\", \"c000\"; \"#49152\" = decimal)";

    // ------------------------------------------------------------ breakpoints
    addTool("breakpoint_set",
        "Set an exec breakpoint or a read/write watchpoint on an address or range. Optional condition "
        "(Denise expression, e.g. \"REGA == $10\", \"MEM:$d012 > $80 && FLAG-C\"; tokens: PC REGA REGX REGY REGS REGP "
        "VPOS HPOS DDR POR IRQ NMI FLAG-C/Z/I/D/B/V/N, MEM:<addr> RAM, CPU:<addr> CPU view, DRIVE8:<addr>) and hit count. "
        "Returns the id. Use continue to run until it hits.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("type":{"type":"string","enum":["exec","read","write"],"default":"exec"},)"
        R"("address":{"type":["integer","string"],"description":")" + ADDR_DESC + R"("},)"
        R"("end_address":{"type":["integer","string"],"description":"Inclusive end of a range, default = address"},)"
        R"("condition":{"type":"string","description":"Stop only if this expression is true"},)"
        R"("condition_mode":{"type":"string","enum":["while_true","becomes_true"],"default":"while_true"},)"
        R"("hit_count":{"type":"integer","minimum":0,"description":"Stop at the n-th hit, 0 = every hit"},)"
        R"("hit_mode":{"type":"string","enum":["equal","at_least"],"default":"equal"}},"required":["address"]})",
        [this](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            Action action;
            if (!actionOf(job.args.getString("type", "exec"), action)) return job.fail("type must be exec, read or write");
            unsigned start, end;
            if (!parseValue(job.args["address"], start, error, "address")) return job.fail(error);
            end = start;
            if (job.args.has("end_address") && !parseValue(job.args["end_address"], end, error, "end_address")) return job.fail(error);
            if (end < start) return job.fail("end_address < address");
            auto condMode = job.args.getString("condition_mode", "while_true");
            auto hitMode = job.args.getString("hit_mode", "equal");
            if (condMode != "while_true" && condMode != "becomes_true") return job.fail("condition_mode must be while_true or becomes_true");
            if (hitMode != "equal" && hitMode != "at_least") return job.fail("hit_mode must be equal or at_least");
            int64_t hits = job.args.getInt("hit_count", 0);
            if (hits < 0) return job.fail("hit_count must be >= 0");

            Breakpoint bp;
            bp.theme = (unsigned)cpuTheme(cpu);
            bp.action = (int)action;
            bp.start = start;
            bp.end = end;
            bp.condition = job.args.getString("condition");
            bp.conditionEdge = condMode == "becomes_true";
            bp.hitCount = (unsigned)hits;
            bp.hitAtLeast = hitMode == "at_least";

            job.phase = Job::Emu;
            job.onEmu = [this, cpu, bp](Job& job) mutable {
                if (!requireCpu(job, cpu)) return;
                bp.ident = nextBreakpointIdent++;
                auto theme = (Theme)bp.theme;
                enableCpuTheme(cpu);
                activeEmulator->debuggerAdd(theme, (Action)bp.action, bp.ident, bp.start, bp.end);
                if (!bp.condition.empty() || bp.hitCount) {
                    if (!activeEmulator->setWatchpointCondition(theme, (Action)bp.action, bp.ident, bp.hitCount,
                            bp.hitAtLeast ? 1 : 0, bp.condition, bp.conditionEdge ? 1 : 0)) {
                        activeEmulator->debuggerRemove(theme, (Action)bp.action, bp.ident);
                        return job.fail("Invalid condition: " + bp.condition);
                    }
                }
                // instruction history helps to see how the CPU got there (enabling resets it)
                if (!debuggerOf(cpu)->historyHandler._enable)
                    activeEmulator->debuggerAdd(theme, Action::History, 0);
                prune(*this);
                breakpoints.push_back(bp);
                Json r = Json::object();
                r.set("id", (int64_t)(bp.ident - IDENT_BASE + 1));
                r.set("cpu", cpuName(cpu));
                r.set("type", typeName((Action)bp.action));
                r.set("address", addr(bp.start));
                if (bp.end != bp.start) r.set("end_address", addr(bp.end));
                job.addText(r.dump());
                job.finish();
            };
        });

    addTool("breakpoint_list",
        "List all breakpoints/watchpoints of the C64 and enabled drives, including those set in the GUI debugger or "
        "binary monitor (origin 'gui', id 'gui:<n>'). Only MCP ones can be deleted via MCP.",
        R"({"type":"object","properties":{}})",
        [this](Job& job) {
            job.phase = Job::Emu;
            job.onEmu = [this](Job& job) {
                if (!sys()) return job.fail("No emulator running");
                prune(*this);
                Json list = Json::array();
                for (int cpu = 0; cpu <= 4; cpu++) {
                    auto dbg = debuggerOf(cpu);
                    if (!dbg) continue;
                    for (auto action : {Action::Breakpoint, Action::Watchpoint, Action::WatchpointWrite, Action::ExceptionPoint}) {
                        for (auto& w : watchList(dbg, action)->watchers) {
                            Json e = Json::object();
                            e.set("id", breakpointName(*this, w.ident));
                            e.set("origin", w.ident >= IDENT_BASE ? "mcp" : "gui");
                            e.set("cpu", cpuName(cpu));
                            e.set("type", typeName(action));
                            e.set("address", addr(w.addr));
                            if (w.endAddr != w.addr) e.set("end_address", addr(w.endAddr));
                            for (auto& bp : breakpoints) {
                                if (bp.ident != w.ident) continue;
                                if (!bp.condition.empty()) {
                                    e.set("condition", bp.condition);
                                    e.set("condition_mode", bp.conditionEdge ? "becomes_true" : "while_true");
                                }
                                if (bp.hitCount) {
                                    e.set("hit_count", bp.hitCount);
                                    e.set("hit_mode", bp.hitAtLeast ? "at_least" : "equal");
                                }
                            }
                            if (w.hitCount || w.curHitCount) e.set("hits", w.curHitCount);
                            list.push(std::move(e));
                        }
                    }
                }
                job.addText(list.items.empty() ? std::string("No breakpoints") : list.dump());
                job.finish();
            };
        });

    addTool("breakpoint_delete",
        "Delete MCP breakpoints by id, or all MCP breakpoints with all=true. GUI/monitor breakpoints are not touched.",
        R"({"type":"object","properties":{"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"all":{"type":"boolean","default":false}}})",
        [this](Job& job) {
            std::vector<unsigned> ids;
            if (job.args["id"].isNumber()) ids.push_back((unsigned)job.args.getInt("id"));
            for (auto& v : job.args["ids"].items)
                if (v.isNumber()) ids.push_back((unsigned)v.number);
            bool all = job.args.getBool("all");
            if (ids.empty() && !all) return job.fail("'id', 'ids' or all=true required");
            job.phase = Job::Emu;
            job.onEmu = [this, ids, all](Job& job) {
                if (!sys()) return job.fail("No emulator running");
                prune(*this);
                unsigned removed = 0;
                std::vector<unsigned> unknown = ids;
                for (auto it = breakpoints.begin(); it != breakpoints.end();) {
                    unsigned id = it->ident - IDENT_BASE + 1;
                    auto u = std::find(unknown.begin(), unknown.end(), id);
                    if (all || u != unknown.end()) {
                        if (u != unknown.end()) unknown.erase(u);
                        activeEmulator->debuggerRemove((Theme)it->theme, (Action)it->action, it->ident);
                        it = breakpoints.erase(it);
                        removed++;
                    } else
                        ++it;
                }
                std::string text = "Deleted " + std::to_string(removed) + " breakpoint(s)";
                if (!unknown.empty()) {
                    text += "; unknown id(s):";
                    for (auto u : unknown) text += " " + std::to_string(u);
                }
                job.addText(text);
                job.finish();
            };
        });

    // ------------------------------------------------------------ run control
    addTool("continue",
        "Resume the CPU (if stopped) and wait until a breakpoint/watchpoint/step stops it or the timeout "
        "(emulated seconds) expires. Returns reason, address and registers. Also works while paused (frames are "
        "stepped). wait=false returns immediately after resuming. warp=true runs unthrottled meanwhile.",
        R"({"type":"object","properties":{)"
        R"("timeout":{"type":"number","minimum":0,"maximum":3600,"default":10,"description":"Emulated seconds"},)"
        R"("wait":{"type":"boolean","default":true},)"
        R"("warp":{"type":"boolean","default":false}}})",
        [this](Job& job) {
            double timeout = std::clamp(job.args.getNumber("timeout", 10.0), 0.0, 3600.0);
            bool wait = job.args.getBool("wait", true);
            bool warp = job.args.getBool("warp");
            job.wallTimeout = 30.0 + timeout * (warp ? 0.5 : 1.2);
            job.phase = Job::Emu;
            job.onEmu = [this, timeout, wait, warp](Job& job) {
                if (!sys()) return job.fail("No emulator running");
                bool wasStopped = cpuStopped();
                if (!wait) {
                    clearAllSoftStops();
                    resumeCpu();
                    job.addText(wasStopped ? "Resumed" : "Already running");
                    return job.finish();
                }
                job.limit = std::max<uint64_t>(1, (uint64_t)(timeout * fps()));
                job.counter = 0;
                clearAllSoftStops();
                resumeCpu();
                job.phase = warp ? Job::Ui : Job::Frames;
            };
            job.onUi = [](Job& job) {
                beginWarp(job);
                job.phase = Job::Frames;
            };
            job.onStop = [this](Job& job) {
                finishWithStop(*this, job);
                return false;
            };
            job.onFrame = [this](Job& job) {
                if (++job.counter < job.limit && !job.cancelled)
                    return;
                Json r = Json::object();
                r.set("stopped", false);
                r.set("timeout", true);
                r.set("frames", job.counter);
                job.addText(r.dump() + "\nNo stop within the timeout; the CPU keeps running.");
                if (job.warpBefore >= 0) job.phase = Job::UiFinish; else job.finish();
            };
            job.onUiFinish = [](Job& job) {
                endWarp(job);
                job.finish();
            };
        });

    addTool("break",
        "Stop the CPU at the next instruction (C64 or a drive). Returns the stop report. If already stopped, "
        "returns the current state.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "}}",
        [this](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            job.phase = Job::Emu;
            job.onEmu = [this, cpu](Job& job) {
                if (!requireCpu(job, cpu)) return;
                if (cpuStopped()) {
                    job.addText(stopReport(*this).dump());
                    return job.finish();
                }
                enableCpuTheme(cpu);
                activeEmulator->debuggerStepInto(cpuTheme(cpu));
                stepPending = true;
                job.counter = 0;
                job.limit = (uint64_t)(2 * fps());
                job.phase = Job::Frames;
            };
            job.onStop = [this](Job& job) {
                {
                    std::lock_guard<std::mutex> lock(stopMutex);
                    if (lastStop.reason == "step") lastStop.reason = "break";
                }
                finishWithStop(*this, job);
                return false;
            };
            job.onFrame = [cpu](Job& job) {
                if (++job.counter >= job.limit || job.cancelled)
                    job.fail("CPU did not stop" + std::string(cpu > 0 ? " (drive idle? it only runs while the bus is active)" : ""));
            };
        });

    addTool("step",
        "Execute instructions while the CPU is stopped: mode 'into' (single instruction), 'over' (JSR runs through), "
        "'out' (run until the current subroutine returns). count repeats the step. Returns the stop report. "
        "A breakpoint hit ends the steps early.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("mode":{"type":"string","enum":["into","over","out"],"default":"into"},)"
        R"("count":{"type":"integer","minimum":1,"maximum":100000,"default":1},)"
        R"("timeout":{"type":"number","minimum":0.1,"maximum":3600,"default":10,"description":"Emulated seconds for the whole step"}}})",
        [this](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            auto mode = job.args.getString("mode", "into");
            if (mode != "into" && mode != "over" && mode != "out") return job.fail("mode must be into, over or out");
            int64_t count = std::clamp<int64_t>(job.args.getInt("count", 1), 1, 100000);
            double timeout = std::clamp(job.args.getNumber("timeout", 10.0), 0.1, 3600.0);
            job.wallTimeout = 30.0 + timeout * 1.2;
            job.phase = Job::Emu;
            job.onEmu = [this, cpu, mode, count, timeout](Job& job) {
                if (!requireCpu(job, cpu)) return;
                if (!cpuStopped())
                    return job.fail("CPU is running; use break or a breakpoint first");
                enableCpuTheme(cpu);
                if (!issueStep(cpu, mode))
                    return job.fail("step out: no return address known (no JSR seen since the debugger is active)");
                stepPending = true;
                job.inputs.assign(1, (unsigned)count); // remaining steps
                job.counter = 0;
                job.limit = std::max<uint64_t>(1, (uint64_t)(timeout * fps()));
                resumeCpu();
                job.phase = Job::Frames;
            };
            job.onStop = [this, cpu, mode](Job& job) {
                std::string reason;
                {
                    std::lock_guard<std::mutex> lock(stopMutex);
                    reason = lastStop.reason;
                }
                if (reason == "step" && --job.inputs[0] > 0 && issueStep(cpu, mode)) {
                    stepPending = true;
                    return true;
                }
                if (reason != "step")
                    clearSoftStop(cpu);
                finishWithStop(*this, job);
                return false;
            };
            job.onFrame = [](Job& job) {
                if (++job.counter >= job.limit || job.cancelled)
                    job.fail("Step did not finish within the timeout; the CPU keeps running (" +
                        std::to_string(job.inputs.empty() ? 0 : job.inputs[0]) + " step(s) left)");
            };
        });

    // ------------------------------------------------------------ registers
    addTool("registers",
        "Read CPU registers (PC, A, X, Y, SP, flags; C64 also $00/$01 and raster position) and the next instruction. "
        "With 'set' registers are written (only while the CPU is stopped), e.g. {\"set\":{\"pc\":\"$c000\",\"a\":1}}.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("set":{"type":"object","properties":{"pc":{"type":["integer","string"]},"a":{"type":["integer","string"]},)"
        R"("x":{"type":["integer","string"]},"y":{"type":["integer","string"]},"sp":{"type":["integer","string"]},)"
        R"("flags":{"type":["integer","string"],"description":"Status register value"}}}}})",
        [this](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            const Json& set = job.args["set"];
            if (!set.isNull() && !set.isObject()) return job.fail("'set' must be an object");
            static const char* names[] = {"pc", "a", "x", "y", "sp", "flags"};
            for (auto& m : set.members) {
                if (std::find_if(std::begin(names), std::end(names), [&](const char* n) { return m.first == n; }) == std::end(names))
                    return job.fail("Unknown register '" + m.first + "' (pc, a, x, y, sp, flags)");
                unsigned v;
                if (!parseValue(m.second, v, error, m.first, m.first == "pc" ? 0xffff : 0xff)) return job.fail(error);
            }
            job.phase = Job::Emu;
            job.onEmu = [this, cpu](Job& job) {
                if (!requireCpu(job, cpu)) return;
                const Json& set = job.args["set"];
                if (set.isObject() && !set.members.empty()) {
                    if (!cpuStopped())
                        return job.fail("Registers can only be written while the CPU is stopped (use break first)");
                    auto r = readRegs(cpu);
                    std::string error;
                    for (auto& m : set.members) {
                        unsigned v = 0;
                        parseValue(m.second, v, error, m.first, m.first == "pc" ? 0xffff : 0xff);
                        if (m.first == "pc") r.pc = v;
                        else if (m.first == "a") r.a = v;
                        else if (m.first == "x") r.x = v;
                        else if (m.first == "y") r.y = v;
                        else if (m.first == "sp") r.sp = v;
                        else if (m.first == "flags") r.flags = v;
                    }
                    writeRegs(cpu, r);
                }
                Json j = regsJson(cpu);
                j.set("cpu", cpuName(cpu));
                j.set("stopped", cpuStopped());
                job.addText(j.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ memory
    addTool("memory_read",
        "Read memory without side effects. bank (C64 only): 'cpu' = as the CPU sees it now (default), 'ram', 'rom' "
        "(BASIC/CHAR/KERNAL where present, else RAM), 'io' ($D000-$DFFF registers), 'cart' (ROML/ROMH). "
        "Drives always use their CPU view. format 'hexdump' (with PETSCII column) or 'base64'.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("address":{"type":["integer","string"],"description":")" + ADDR_DESC + R"("},)"
        R"("length":{"type":"integer","minimum":1,"maximum":65536,"default":256},)"
        R"("bank":{"type":"string","enum":["cpu","ram","rom","io","cart"],"default":"cpu"},)"
        R"("format":{"type":"string","enum":["hexdump","base64"],"default":"hexdump"}},"required":["address"]})",
        [](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            unsigned start;
            if (!parseValue(job.args["address"], start, error, "address")) return job.fail(error);
            int64_t length = job.args.getInt("length", 256);
            if (length < 1 || length > 65536) return job.fail("length must be 1..65536");
            unsigned end = (unsigned)std::min<int64_t>(0xffff, start + length - 1);
            auto bank = job.args.getString("bank", "cpu");
            Theme theme = memTheme(cpu);
            Action action = Action::None;
            if (cpu == 0) {
                if (bank == "cpu") theme = Theme::CPU;
                else if (bank == "ram") theme = Theme::Memory;
                else if (bank == "rom") { theme = Theme::Memory; action = Action::MemROM; }
                else if (bank == "io") { theme = Theme::Memory; action = Action::MemIO; }
                else if (bank == "cart") { theme = Theme::Memory; action = Action::MemCART; }
                else return job.fail("bank must be cpu, ram, rom, io or cart");
            } else if (bank != "cpu")
                return job.fail("Drives only support bank 'cpu'");
            bool base64 = job.args.getString("format", "hexdump") == "base64";
            job.phase = Job::Emu;
            job.onEmu = [cpu, start, end, theme, action, base64](Job& job) {
                if (!requireCpu(job, cpu)) return;
                std::vector<uint8_t> data(end - start + 1);
                activeEmulator->getMemory(theme, action, start, end, data.data());
                if (base64) {
                    job.addText(addr(start) + "-" + addr(end) + " (" + std::to_string(data.size()) + " bytes)");
                    job.addText(Base64::encode(data.data(), data.size()));
                    return job.finish();
                }
                std::string out;
                for (size_t row = 0; row < data.size(); row += 16) {
                    out += hex4(start + (unsigned)row) + ": ";
                    std::string text;
                    for (size_t i = 0; i < 16; i++) {
                        if (row + i < data.size()) {
                            out += hex2(data[row + i]) + " ";
                            text += petsciiChar(data[row + i]);
                        } else
                            out += "   ";
                        if (i == 7) out += " ";
                    }
                    out += " |" + text + "|\n";
                }
                job.addText(out);
                job.finish();
            };
        });

    auto writeBytes = [](int cpu, unsigned start, const std::vector<uint8_t>& data, bool ram) {
        if (cpu == 0 && ram) {
            auto s = sys();
            for (size_t i = 0; i < data.size(); i++)
                s->ram[(start + i) & 0xffff] = data[i];
            return;
        }
        std::vector<uint16_t> values(data.begin(), data.end());
        activeEmulator->editMemory(cpuTheme(cpu), start, values);
    };

    addTool("memory_write",
        "Write bytes to memory. bank (C64): 'cpu' = through the current memory map (I/O registers are written with "
        "their effects, ROM areas patch the ROM), 'ram' = plain RAM underneath (default). Drives: CPU view. "
        "Bytes as array of numbers/hex strings or one hex string \"a9 00 8d 20 d0\".",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("address":{"type":["integer","string"],"description":")" + ADDR_DESC + R"("},)"
        R"("bytes":{"type":["array","string"],"items":{"type":["integer","string"]}},)"
        R"("bank":{"type":"string","enum":["ram","cpu"],"default":"ram"}},"required":["address","bytes"]})",
        [writeBytes](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            unsigned start;
            if (!parseValue(job.args["address"], start, error, "address")) return job.fail(error);
            auto bank = job.args.getString("bank", "ram");
            if (bank != "ram" && bank != "cpu") return job.fail("bank must be ram or cpu");
            std::vector<uint8_t> data;
            const Json& bytes = job.args["bytes"];
            if (bytes.isString()) {
                std::string hex;
                for (char c : bytes.string)
                    if (std::isxdigit((unsigned char)c)) hex += c;
                    else if (!std::isspace((unsigned char)c) && c != ',' && c != '$') return job.fail("Invalid hex string in 'bytes'");
                if (hex.empty() || hex.size() % 2) return job.fail("'bytes' hex string needs pairs of digits");
                for (size_t i = 0; i < hex.size(); i += 2)
                    data.push_back((uint8_t)std::stoul(hex.substr(i, 2), nullptr, 16));
            } else if (bytes.isArray()) {
                for (auto& b : bytes.items) {
                    unsigned v;
                    if (!parseValue(b, v, error, "bytes", 0xff)) return job.fail(error);
                    data.push_back((uint8_t)v);
                }
            }
            if (data.empty()) return job.fail("'bytes' required");
            if (start + data.size() > 0x10000) return job.fail("Write exceeds $ffff");
            bool ram = bank == "ram";
            job.phase = Job::Emu;
            job.onEmu = [cpu, start, data, ram, writeBytes](Job& job) {
                if (!requireCpu(job, cpu)) return;
                writeBytes(cpu, start, data, ram);
                job.addText("Wrote " + std::to_string(data.size()) + " byte(s) to " + addr(start) + "-" + addr(start + (unsigned)data.size() - 1));
                job.finish();
            };
        });

    addTool("load_memory",
        "Copy binary data into memory without autostart (C64: plain RAM, drives: CPU view). Give 'path' or "
        "'data_base64'. Without 'address' the data must be a PRG (first two bytes = load address); "
        "prg=true strips the load address also when 'address' is given.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("path":{"type":"string","description":"Absolute path of a local file"},)"
        R"("data_base64":{"type":"string"},)"
        R"("address":{"type":["integer","string"],"description":")" + ADDR_DESC + R"("},)"
        R"("prg":{"type":"boolean","description":"Data starts with a 2-byte load address; default true when no address is given"}}})",
        [writeBytes](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            std::vector<uint8_t> data;
            std::string source;
            if (job.args.has("data_base64")) {
                if (!Base64::decode(job.args.getString("data_base64"), data)) return job.fail("Invalid base64 data");
                source = "base64 data";
            } else {
                source = job.args.getString("path");
                if (source.empty()) return job.fail("'path' or 'data_base64' required");
                if (!readFile(source, data)) return job.fail("Cannot read " + source);
            }
            bool hasAddress = job.args.has("address");
            bool prg = job.args.getBool("prg", !hasAddress);
            unsigned start = 0;
            if (hasAddress && !parseValue(job.args["address"], start, error, "address")) return job.fail(error);
            if (prg) {
                if (data.size() < 3) return job.fail("PRG too short");
                if (!hasAddress) start = data[0] | (data[1] << 8);
                data.erase(data.begin(), data.begin() + 2);
            }
            if (data.empty()) return job.fail("No data");
            if (start + data.size() > 0x10000) return job.fail("Data exceeds $ffff (" + std::to_string(data.size()) + " bytes at " + addr(start) + ")");
            job.phase = Job::Emu;
            job.onEmu = [cpu, start, data, source, writeBytes](Job& job) {
                if (!requireCpu(job, cpu)) return;
                writeBytes(cpu, start, data, true);
                job.addText("Loaded " + std::to_string(data.size()) + " bytes from " + source + " to " + addr(start) + "-" +
                    addr(start + (unsigned)data.size() - 1) + " (" + cpuName(cpu) + ")");
                job.finish();
            };
        });

    // ------------------------------------------------------------ disassembler
    addTool("disassemble",
        "Disassemble instructions starting at address (default: current PC). '>' marks the PC, '*' an exec breakpoint.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("address":{"type":["integer","string"],"description":")" + ADDR_DESC + R"("},)"
        R"("count":{"type":"integer","minimum":1,"maximum":500,"default":20}}})",
        [](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            bool hasAddress = job.args.has("address");
            unsigned start = 0;
            if (hasAddress && !parseValue(job.args["address"], start, error, "address")) return job.fail(error);
            int count = (int)std::clamp<int64_t>(job.args.getInt("count", 20), 1, 500);
            job.phase = Job::Emu;
            job.onEmu = [cpu, hasAddress, start, count](Job& job) mutable {
                if (!requireCpu(job, cpu)) return;
                unsigned pc = readRegs(cpu).pc;
                if (!hasAddress) start = pc;
                auto dbg = debuggerOf(cpu);
                std::string out;
                unsigned a = start;
                for (int i = 0; i < count && a <= 0xffff; i++) {
                    unsigned bytes = 1;
                    auto text = disassembleLine(cpu, a, bytes);
                    if (bytes < 1) bytes = 1;
                    uint8_t raw[3] = {0, 0, 0};
                    activeEmulator->getMemory(memTheme(cpu), Action::None, a, std::min(0xffffu, a + 2), raw);
                    std::string hex;
                    for (unsigned b = 0; b < 3; b++)
                        hex += b < bytes ? hex2(raw[b]) + " " : "   ";
                    bool bp = false;
                    if (dbg)
                        for (auto& w : dbg->breakPoints.watchers)
                            bp |= a >= w.addr && a <= w.endAddr;
                    out += std::string(a == pc ? ">" : " ") + (bp ? "*" : " ") + hex4(a) + "  " + hex + " " + text + "\n";
                    a += bytes;
                }
                job.addText(out);
                job.finish();
            };
        });

    addTool("cpu_history",
        "The last executed instructions (oldest first) with the flags at that time. Recording starts with the "
        "first breakpoint_set or the first call of this tool for that CPU.",
        std::string(R"({"type":"object","properties":{)") + CPU_PROP + "," +
        R"("count":{"type":"integer","minimum":1,"maximum":1000,"default":30}}})",
        [](Job& job) {
            int cpu;
            std::string error;
            if (!parseCpu(job.args, cpu, error)) return job.fail(error);
            int count = (int)std::clamp<int64_t>(job.args.getInt("count", 30), 1, 1000);
            job.phase = Job::Emu;
            job.onEmu = [cpu, count](Job& job) {
                if (!requireCpu(job, cpu)) return;
                auto dbg = debuggerOf(cpu);
                if (!dbg->historyHandler._enable) {
                    activeEmulator->debuggerAdd(cpuTheme(cpu), Action::History, 0);
                    job.addText("History recording was off and is enabled now; run the CPU and call again.");
                    return job.finish();
                }
                std::vector<std::string> lines;
                for (int i = 0; i < count; i++) {
                    uint16_t flags = 0;
                    auto line = activeEmulator->disassembleTrace(cpuTheme(cpu), (unsigned)i, flags);
                    if (line.empty()) break;
                    auto bar = line.find('|');
                    if (bar != std::string::npos)
                        line = line.substr(0, bar) + "  " + line.substr(bar + 1);
                    lines.push_back(line + "    " + flagString(flags));
                }
                if (lines.empty()) {
                    job.addText("No instructions recorded yet");
                    return job.finish();
                }
                std::string out;
                for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                    out += *it + "\n";
                job.addText(out);
                job.finish();
            };
        });

    // ------------------------------------------------------------ chip state
    addTool("chip_state",
        "Registers of a chip, raw (read without side effects) plus decoded values: 'vic', 'sid' (first SID), "
        "'cia1', 'cia2', or a drive ('drive8'..'drive11': its two VIAs).",
        R"({"type":"object","properties":{"chip":{"type":"string","enum":["vic","sid","cia1","cia2","drive8","drive9","drive10","drive11"]}},"required":["chip"]})",
        [](Job& job) {
            auto chip = job.args.getString("chip");
            static const std::vector<std::string> valid = {"vic", "sid", "cia1", "cia2", "drive8", "drive9", "drive10", "drive11"};
            if (std::find(valid.begin(), valid.end(), chip) == valid.end()) return job.fail("Invalid chip");
            job.phase = Job::Emu;
            job.onEmu = [chip](Job& job) {
                int cpu = chip.compare(0, 5, "drive") == 0 ? std::atoi(chip.c_str() + 5) - 7 : 0;
                if (!requireCpu(job, cpu)) return;
                auto s = sys();
                auto& snap = scratch();
                Json j = Json::object();
                j.set("chip", chip);

                auto rawRegs = [&](unsigned from, unsigned count, Theme theme, Action action) {
                    std::vector<uint8_t> regs(count);
                    activeEmulator->getMemory(theme, action, from, from + count - 1, regs.data());
                    std::string out;
                    for (unsigned i = 0; i < count; i++)
                        out += (i ? " " : "") + hex2(regs[i]);
                    return out;
                };

                if (chip == "vic") {
                    j.set("registers_d000", rawRegs(0xd000, 0x2f, Theme::Memory, Action::MemIO));
                    s->vicII->updateVideoSnapshot(snap);
                    auto& v = snap.vicII;
                    j.set("raster_line", (unsigned)s->vicII->getVcounterHR());
                    j.set("raster_cycle", (unsigned)s->vicII->getCycle());
                    j.set("irq_line", (unsigned)v.irqLine);
                    j.set("irq_latch", byte(v.irqLatch));
                    j.set("irq_enable", byte(v.irqEnable));
                    j.set("vic_bank", addr(v.vicBank));
                    j.set("screen", addr(v.screenMemory));
                    j.set("charset_bitmap", addr(v.charMemory));
                    static const char* modes[] = {"standard text", "multicolor text", "standard bitmap", "multicolor bitmap",
                        "extended color text", "invalid (ECM+MCM)", "invalid (ECM+BMM)", "invalid (ECM+BMM+MCM)"};
                    j.set("mode", modes[v.mode & 7]);
                    j.set("display_enabled", v.den);
                    j.set("x_scroll", (unsigned)v.xScroll);
                    j.set("y_scroll", (unsigned)v.yScroll);
                    j.set("bad_line", v.badLine);
                    Json sprites = Json::array();
                    for (int i = 0; i < 8; i++) {
                        auto& sp = v.spr[i];
                        Json e = Json::object();
                        e.set("x", (unsigned)sp.x);
                        e.set("y", (unsigned)sp.y);
                        e.set("data", addr(sp.addr & 0xffc0));
                        if (sp.expandX) e.set("expand_x", true);
                        if (sp.expandY) e.set("expand_y", true);
                        if (sp.multiColor) e.set("multicolor", true);
                        if (sp.prioMD) e.set("behind_background", true);
                        sprites.push(std::move(e));
                    }
                    j.set("sprites", sprites);
                } else if (chip == "sid") {
                    s->sidManager.updateSnapshot(snap);
                    auto& sid = snap.sids[0];
                    Json voices = Json::array();
                    for (int i = 0; i < 3; i++) {
                        auto& vc = sid.voices[i];
                        Json e = Json::object();
                        e.set("frequency", "$" + hex4(vc.frequency));
                        e.set("pulse_width", "$" + hex4(vc.pulseWidth));
                        e.set("control", byte(vc.control));
                        std::string wave;
                        if (vc.control & 0x10) wave += "triangle ";
                        if (vc.control & 0x20) wave += "saw ";
                        if (vc.control & 0x40) wave += "pulse ";
                        if (vc.control & 0x80) wave += "noise ";
                        if (!wave.empty()) wave.pop_back();
                        e.set("waveform", wave.empty() ? std::string("none") : wave);
                        e.set("gate", (vc.control & 1) != 0);
                        e.set("adsr", hex2(vc.attack << 4 | vc.decay) + hex2(vc.sustain << 4 | vc.release));
                        voices.push(std::move(e));
                    }
                    j.set("voices", voices);
                    j.set("filter_cutoff", "$" + hex4(sid.filter.cutOff));
                    j.set("filter_resonance", (unsigned)sid.filter.resonance);
                    j.set("filter_voices", byte(sid.filter.voices));
                    j.set("filter_mode", byte(sid.filter.mode));
                    j.set("volume", (unsigned)sid.volume);
                    j.set("pot_x", (unsigned)sid.potX);
                    j.set("pot_y", (unsigned)sid.potY);
                } else if (chip == "cia1" || chip == "cia2") {
                    int n = chip == "cia1" ? 0 : 1;
                    j.set("registers", rawRegs(n ? 0xdd00 : 0xdc00, 16, Theme::Memory, Action::MemIO));
                    s->updateCiaDebuggerSnapshot(snap);
                    auto& c = snap.cia[n];
                    static const char* portNames[] = {"a", "b"};
                    for (int p = 0; p < 2; p++) {
                        auto& port = c.port[p];
                        Json e = Json::object();
                        e.set("pr", byte(port.pr));
                        e.set("ddr", byte(port.ddr));
                        e.set("pins", byte(port.io));
                        e.set("timer", "$" + hex4(port.timer));
                        e.set("latch", "$" + hex4(port.timerLatch));
                        e.set("running", port.timerRunning);
                        e.set("oneshot", port.oneshot);
                        j.set(std::string("port_") + portNames[p] + "_timer_" + portNames[p], e);
                    }
                    j.set("icr", byte(c.icr));
                    j.set("icr_mask", byte(c.icrMask));
                    char tod[16];
                    std::snprintf(tod, sizeof tod, "%02x:%02x:%02x.%x", (c.tod >> 16) & 0xff, (c.tod >> 8) & 0xff, c.tod & 0xff, (c.tod >> 24) & 0xf);
                    j.set("tod", std::string(tod));
                    j.set("sdr", byte(c.sdr));
                    if (n == 1) {
                        // VIC bank from port A bits 0/1 (inverted)
                        unsigned bankBits = (~(c.port[0].pr | ~c.port[0].ddr)) & 3;
                        j.set("vic_bank", addr(bankBits * 0x4000));
                    }
                } else {
                    auto d = drive(cpu);
                    d->updateViaDebuggerSnapshot(snap);
                    j.set("via1_1800", rawRegs(0x1800, 16, memTheme(cpu), Action::None));
                    j.set("via2_1c00", rawRegs(0x1c00, 16, memTheme(cpu), Action::None));
                    for (int n = 0; n < 2; n++) {
                        auto& v = snap.via[n];
                        Json e = Json::object();
                        e.set("timer_a", "$" + hex4(v.port[0].timer));
                        e.set("latch_a", "$" + hex4(v.port[0].timerLatch));
                        e.set("timer_b", "$" + hex4(v.port[1].timer));
                        e.set("port_a", byte(v.port[0].pr));
                        e.set("ddr_a", byte(v.port[0].ddr));
                        e.set("port_b", byte(v.port[1].pr));
                        e.set("ddr_b", byte(v.port[1].ddr));
                        e.set("ifr", byte(v.ifr));
                        e.set("ier", byte(v.ier));
                        e.set("acr", byte(v.acr));
                        e.set("pcr", byte(v.pcr));
                        j.set(n ? "via2" : "via1", e);
                    }
                    Json regs = regsJson(cpu);
                    j.set("cpu", regs);
                }
                job.addText(j.dump());
                job.finish();
            };
        });

    // ------------------------------------------------------------ quit
    addTool("quit", "Quit the emulator (settings are not saved in headless mode). The response is sent before exiting.",
        R"({"type":"object","properties":{}})",
        [this](Job& job) {
            quitRequested = true;
            job.addText("Quitting");
            job.finish();
        });
}
