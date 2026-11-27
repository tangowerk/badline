#pragma once

// Embedded MCP server (Streamable HTTP, JSON responses only, localhost).
//
// Threading: httplib handler threads parse JSON-RPC and submit tool calls as
// jobs. A job moves through phases, each owned by one thread:
//   Ui       -> UI thread (uiTick), for operations that touch GTK (media, states, power, warp, pause)
//   Emu      -> emu thread before a frame (emuTick), for input and observation
//   Frames   -> emu thread after every emulated frame (frameDone), for waits/holds
//   UiFinish -> UI thread, cleanup after frame jobs (e.g. restore warp)
// Calls are processed strictly one after another.
//
// Debugger stops: the emulator reports breakpoints/steps via Program::debugger -> debuggerEvent
// (emu thread, inside the CPU). While stopped, the stop loop (GUI: EmuThread::lockDebugger,
// headless: Program::loopStopped) keeps calling emuTick/uiTick, so Emu and Ui jobs still run.

#include "json.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; }
namespace Emulator { struct Interface; }

struct McpServer {

    struct Job {
        enum Phase : int { Ui, Emu, Frames, UiFinish, Done };

        std::string tool;
        Json args;
        std::atomic<int> phase{Emu};
        std::atomic<bool> cancelled{false};

        std::function<void(Job&)> onUi;
        std::function<void(Job&)> onEmu;
        // called after each frame; frame jobs advance the phase themselves
        std::function<void(Job&)> onFrame;
        std::function<void(Job&)> onUiFinish;
        // runs on the HTTP thread after the job is done (e.g. waiting for a file)
        std::function<void(Job&)> postProcess;
        // emu thread, inside the CPU, when the debugger stops while the job is in phase Frames;
        // return true to keep the CPU running (e.g. further steps)
        std::function<bool(Job&)> onStop;
        // Ui phase resumes a stopped CPU first (power, reset, media, states)
        bool resumesCpu = false;

        // free state for tools
        uint64_t counter = 0;
        uint64_t limit = 0;
        int warpBefore = -1;
        std::string text;
        std::vector<unsigned> inputs;
        unsigned deviceId = 0;
        double wallTimeout = 60.0;

        // dialogs suppressed during UI phases (warnings, errors), appended to the result
        std::vector<std::string> messages;

        // result
        Json content = Json::array();
        bool isError = false;

        auto addText(const std::string& t) -> void;
        auto addImage(const std::string& base64Png) -> void;
        auto fail(const std::string& message) -> void; // adds text, marks error, finishes
        auto finish() -> void;
        auto waitDone(double seconds) -> bool;

    private:
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    using JobPtr = std::shared_ptr<Job>;

    struct Tool {
        std::string name;
        std::string description;
        Json schema;
        // fails immediately while the CPU is stopped by the debugger (frame based tools)
        bool needsRunning;
        // runs on the HTTP thread: validate args and set up phase/callbacks,
        // or fail() immediately
        std::function<void(Job&)> prepare;
    };

    McpServer();
    ~McpServer();

    auto start(unsigned port) -> bool;
    auto stop() -> void;

    // UI thread
    auto uiTick() -> void;
    // emu thread, once per loop iteration (also while paused / in debugger)
    auto emuTick() -> void;
    // emu thread, after each emulated frame
    auto frameDone() -> void;
    // emu thread: run a frame although the emulation is paused
    auto wantsFrame() -> bool;

    // ---- debugger (emu thread unless noted)
    enum class StopDecision { Ignore, Stop, Continue };
    // called for every debugger callback; Continue = keep running silently (MCP step in progress)
    auto debuggerEvent(void* snapshot) -> StopDecision;
    // CPU stopped by the debugger (any owner), any thread
    auto cpuStopped() -> bool;
    // stop loop: leave it so that a pending Ui job which resumes the CPU (or quit) can run
    auto stopLoopShouldExit() -> bool;
    // emu thread: stop report (reason, address, registers) of the current stop
    auto stopStatus() -> Json;

    struct StopInfo {
        bool valid = false;
        std::string reason;     // breakpoint, watchpoint_read, watchpoint_write, step, ...
        std::string cpu;        // c64, drive8..drive11
        unsigned address = 0;
        std::vector<unsigned> idents;
        uint64_t frame = 0;
    } lastStop;                 // guarded by stopMutex
    std::mutex stopMutex;

    struct Breakpoint {
        unsigned ident;
        unsigned theme;         // Emulator::Interface::DebuggerTheme
        int action;             // Emulator::Interface::DebuggerAction
        unsigned start, end;
        std::string condition;
        unsigned hitCount = 0;
        bool hitAtLeast = false;
        bool conditionEdge = false;
    };
    std::vector<Breakpoint> breakpoints; // owned by MCP
    unsigned nextBreakpointIdent = 0x40000000; // GUI debugger counts from 1
    bool stepPending = false;   // MCP issued break/step, the next stop belongs to MCP

    // UI thread: quit requested by the quit tool (handled by Program after uiTick)
    std::atomic<bool> quitRequested{false};

    // emu thread: input injection
    std::atomic<bool> inputActive{false};
    auto inputState(unsigned deviceId, unsigned inputId) -> int16_t;
    auto setInput(unsigned deviceId, unsigned inputId, bool pressed) -> void;
    auto releaseDevice(unsigned deviceId) -> void;

    uint64_t frameCounter = 0;
    unsigned port = 0;

private:
    std::unique_ptr<httplib::Server> http;
    std::thread httpThread;
    std::mutex callMutex;   // one tool call at a time
    std::mutex jobMutex;    // guards 'current'
    JobPtr current;
    std::atomic<bool> hasJob{false};
    std::atomic<bool> resetInputs{false};
    std::atomic<bool> stopping{false};
    std::vector<Tool> tools;

    std::vector<std::vector<uint8_t>> inputs; // [deviceId][inputId]

    auto currentJob() -> JobPtr;
    auto clearJob(const JobPtr& job) -> void;
    auto handleRpc(const Json& request, Json& response) -> bool;
    auto callTool(const Json& params) -> Json;
    auto registerTools() -> void;
    auto addTool(const std::string& name, const std::string& description, const std::string& schema, std::function<void(Job&)> prepare, bool needsRunning = false) -> void;
    auto registerDebugTools() -> void;
};

extern McpServer* mcpServer;
