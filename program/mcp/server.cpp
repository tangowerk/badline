#include "server.h"
#include "../../deps/httplib.h"
#include "../view/message.h"
#include <chrono>
#include <cstdio>

McpServer* mcpServer = nullptr;

static auto hex4(unsigned v) -> std::string {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04x", v & 0xffff);
    return buf;
}

static const char* SERVER_NAME = "badline";
static const char* SERVER_VERSION = "0.1";
static const char* DEFAULT_PROTOCOL = "2025-06-18";
static const char* PROTOCOLS[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

// ---------------------------------------------------------------- Job

auto McpServer::Job::addText(const std::string& t) -> void {
    Json c = Json::object();
    c.set("type", "text");
    c.set("text", t);
    content.push(std::move(c));
}

auto McpServer::Job::addImage(const std::string& base64Png) -> void {
    Json c = Json::object();
    c.set("type", "image");
    c.set("data", base64Png);
    c.set("mimeType", "image/png");
    content.push(std::move(c));
}

auto McpServer::Job::fail(const std::string& message) -> void {
    addText(message);
    isError = true;
    finish();
}

auto McpServer::Job::finish() -> void {
    phase = Done;
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
    cv.notify_all();
}

auto McpServer::Job::waitDone(double seconds) -> bool {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds((int64_t)(seconds * 1000.0)), [this] { return done; });
}

// ---------------------------------------------------------------- server

McpServer::McpServer() {
    registerTools();
    registerDebugTools();
}

McpServer::~McpServer() {
    stop();
}

static auto originAllowed(const httplib::Request& req) -> bool {
    if (!req.has_header("Origin"))
        return true; // non-browser clients
    auto origin = req.get_header_value("Origin");
    for (auto prefix : {"http://localhost", "http://127.0.0.1", "http://[::1]"}) {
        std::string p = prefix;
        if (origin.compare(0, p.size(), p) == 0 && (origin.size() == p.size() || origin[p.size()] == ':'))
            return true;
    }
    return false;
}

auto McpServer::start(unsigned port) -> bool {
    this->port = port;
    http = std::make_unique<httplib::Server>();

    http->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        if (!originAllowed(req)) {
            res.status = 403;
            return;
        }
        Json request;
        std::string error;
        if (!Json::parse(req.body, request, error)) {
            Json resp = Json::object();
            resp.set("jsonrpc", "2.0");
            resp.set("id", Json());
            Json err = Json::object();
            err.set("code", -32700);
            err.set("message", "Parse error: " + error);
            resp.set("error", err);
            res.status = 400;
            res.set_content(resp.dump(), "application/json");
            return;
        }

        if (request.isArray()) {
            Json out = Json::array();
            for (auto& r : request.items) {
                Json response;
                if (handleRpc(r, response))
                    out.push(std::move(response));
            }
            if (out.items.empty()) {
                res.status = 202;
                return;
            }
            res.set_content(out.dump(), "application/json");
            return;
        }

        Json response;
        if (!handleRpc(request, response)) {
            res.status = 202; // notification or response from client
            return;
        }
        res.set_content(response.dump(), "application/json");
    });

    auto notAllowed = [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;
        res.set_header("Allow", "POST");
    };
    http->Get("/mcp", notAllowed);
    http->Delete("/mcp", notAllowed);

    if (!http->bind_to_port("127.0.0.1", (int)port)) {
        std::fprintf(stderr, "MCP: cannot bind to 127.0.0.1:%u\n", port);
        http.reset();
        return false;
    }

    httpThread = std::thread([this] { http->listen_after_bind(); });
    std::fprintf(stdout, "MCP server listening on http://127.0.0.1:%u/mcp\n", port);
    std::fflush(stdout);
    return true;
}

auto McpServer::stop() -> void {
    stopping = true;
    // wake up a waiting handler first, otherwise the HTTP thread pool cannot shut down
    auto job = currentJob();
    if (job) {
        job->cancelled = true;
        job->fail("Emulator shutting down");
        clearJob(job);
    }
    if (http) {
        http->stop();
        if (httpThread.joinable())
            httpThread.join();
        http.reset();
    }
}

static auto rpcError(Json& response, int code, const std::string& message) -> void {
    Json err = Json::object();
    err.set("code", code);
    err.set("message", message);
    response.set("error", err);
}

auto McpServer::handleRpc(const Json& request, Json& response) -> bool {
    if (!request.isObject() || !request.has("method"))
        return false; // response or garbage from client: nothing to answer

    bool isNotification = !request.has("id");
    std::string method = request.getString("method");
    const Json& params = request["params"];

    if (isNotification)
        return false;

    response = Json::object();
    response.set("jsonrpc", "2.0");
    response.set("id", request["id"]);

    if (method == "initialize") {
        std::string requested = params.getString("protocolVersion");
        std::string version = DEFAULT_PROTOCOL;
        for (auto p : PROTOCOLS)
            if (requested == p)
                version = p;

        Json result = Json::object();
        result.set("protocolVersion", version);
        Json caps = Json::object();
        caps.set("tools", Json::object());
        result.set("capabilities", caps);
        Json info = Json::object();
        info.set("name", SERVER_NAME);
        info.set("version", SERVER_VERSION);
        result.set("serverInfo", info);
        result.set("instructions",
            "Commodore 64 emulator. Typical flow: load_media (autostart) -> wait_for_text / run_frames -> "
            "read_screen or screenshot. Frames are PAL 50 Hz / NTSC 60 Hz. Waiting tools step the emulation "
            "even when it is paused. Debugging: breakpoint_set -> continue (blocks until the CPU stops) -> "
            "registers / memory_read / disassemble / step. While the CPU is stopped, frame based tools fail; "
            "addresses accept numbers or hex strings like \"$c000\", \"0xc000\", \"c000\".");
        response.set("result", result);

    } else if (method == "ping") {
        response.set("result", Json::object());

    } else if (method == "tools/list") {
        Json list = Json::array();
        for (auto& tool : tools) {
            Json t = Json::object();
            t.set("name", tool.name);
            t.set("description", tool.description);
            t.set("inputSchema", tool.schema);
            list.push(std::move(t));
        }
        Json result = Json::object();
        result.set("tools", list);
        response.set("result", result);

    } else if (method == "tools/call") {
        if (!params.isObject() || !params["name"].isString()) {
            rpcError(response, -32602, "Invalid params: 'name' missing");
            return true;
        }
        bool known = false;
        for (auto& tool : tools)
            known |= tool.name == params.getString("name");
        if (!known) {
            rpcError(response, -32602, "Unknown tool: " + params.getString("name"));
            return true;
        }
        response.set("result", callTool(params));

    } else {
        rpcError(response, -32601, "Method not found: " + method);
    }
    return true;
}

auto McpServer::callTool(const Json& params) -> Json {
    std::lock_guard<std::mutex> callLock(callMutex);

    auto job = std::make_shared<Job>();
    job->tool = params.getString("name");
    job->args = params["arguments"].isObject() ? params["arguments"] : Json::object();

    // a cancelled job may still be cleaning up
    for (int i = 0; i < 500 && currentJob(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Json result = Json::object();

    if (stopping) {
        job->addText("Emulator shutting down");
        job->isError = true;
    } else if (currentJob()) {
        job->addText("Emulator busy (previous call still cleaning up)");
        job->isError = true;
    } else {
        for (auto& tool : tools) {
            if (tool.name == job->tool) {
                if (tool.needsRunning && cpuStopped()) {
                    std::string where;
                    {
                        std::lock_guard<std::mutex> lock(stopMutex);
                        if (lastStop.valid)
                            where = " at $" + hex4(lastStop.address) + " (" + lastStop.cpu + ")";
                    }
                    job->fail("CPU stopped by the debugger" + where + ". Frames do not advance; use continue or step first.");
                } else
                    tool.prepare(*job);
                break;
            }
        }

        if (job->phase != Job::Done) {
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                current = job;
                hasJob = true;
            }
            if (!job->waitDone(job->wallTimeout)) {
                job->cancelled = true;
                // give the owning thread a moment to clean up
                if (!job->waitDone(2.0)) {
                    clearJob(job);
                    resetInputs = true;
                    Json r = Json::object();
                    Json content = Json::array();
                    Json c = Json::object();
                    c.set("type", "text");
                    c.set("text", "Timeout: the emulator did not complete '" + job->tool + "' (emulation stopped or in debugger?)");
                    content.push(c);
                    r.set("content", content);
                    r.set("isError", true);
                    return r;
                }
            }
            clearJob(job);
        }

        if (job->postProcess && !job->isError)
            job->postProcess(*job);

        for (auto& m : job->messages)
            job->addText("Emulator " + m);
    }

    result.set("content", job->content);
    result.set("isError", job->isError);
    return result;
}

auto McpServer::currentJob() -> JobPtr {
    if (!hasJob)
        return nullptr;
    std::lock_guard<std::mutex> lock(jobMutex);
    return current;
}

auto McpServer::clearJob(const JobPtr& job) -> void {
    std::lock_guard<std::mutex> lock(jobMutex);
    if (current == job) {
        current.reset();
        hasJob = false;
    }
}

// ---------------------------------------------------------------- ticks

auto McpServer::uiTick() -> void {
    auto job = currentJob();
    if (!job)
        return;

    int phase = job->phase;
    if (phase == Job::Done) {
        clearJob(job);
    } else if (phase == Job::Ui) {
        Message::capture = &job->messages;
        if (job->onUi)
            job->onUi(*job);
        else
            job->finish();
        Message::capture = nullptr;
    } else if (phase == Job::UiFinish) {
        Message::capture = &job->messages;
        if (job->onUiFinish)
            job->onUiFinish(*job);
        Message::capture = nullptr;
        if (job->phase != Job::Done)
            job->finish();
    }
}

auto McpServer::emuTick() -> void {
    if (resetInputs) {
        resetInputs = false;
        for (auto& d : inputs)
            std::fill(d.begin(), d.end(), 0);
        inputActive = false;
    }
    auto job = currentJob();
    if (!job)
        return;

    int phase = job->phase;
    if (phase == Job::Done) {
        clearJob(job);
    } else if (phase == Job::Emu) {
        if (job->onEmu)
            job->onEmu(*job);
        else
            job->finish();
    }
}

auto McpServer::frameDone() -> void {
    frameCounter++;
    auto job = currentJob();
    if (!job || job->phase != Job::Frames)
        return;

    if (job->onFrame)
        job->onFrame(*job);
    else
        job->finish();
}

auto McpServer::wantsFrame() -> bool {
    auto job = currentJob();
    return job && job->phase == Job::Frames;
}

// ---------------------------------------------------------------- input

auto McpServer::inputState(unsigned deviceId, unsigned inputId) -> int16_t {
    if (deviceId >= inputs.size() || inputId >= inputs[deviceId].size())
        return 0;
    return inputs[deviceId][inputId];
}

auto McpServer::setInput(unsigned deviceId, unsigned inputId, bool pressed) -> void {
    if (deviceId >= inputs.size())
        inputs.resize(deviceId + 1);
    auto& dev = inputs[deviceId];
    if (inputId >= dev.size())
        dev.resize(inputId + 1, 0);
    dev[inputId] = pressed ? 1 : 0;

    bool any = false;
    for (auto& d : inputs)
        for (auto v : d)
            any |= v != 0;
    inputActive = any;
}

auto McpServer::releaseDevice(unsigned deviceId) -> void {
    if (deviceId < inputs.size())
        std::fill(inputs[deviceId].begin(), inputs[deviceId].end(), 0);
    bool any = false;
    for (auto& d : inputs)
        for (auto v : d)
            any |= v != 0;
    inputActive = any;
}

auto McpServer::addTool(const std::string& name, const std::string& description, const std::string& schema, std::function<void(Job&)> prepare, bool needsRunning) -> void {
    Json s;
    std::string error;
    if (!Json::parse(schema, s, error)) {
        std::fprintf(stderr, "MCP: invalid schema for tool %s: %s\n", name.c_str(), error.c_str());
        s = Json::object();
        s.set("type", "object");
    }
    tools.push_back({name, description, s, needsRunning, std::move(prepare)});
}
