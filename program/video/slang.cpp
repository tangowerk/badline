#include "slang.h"
#include "../program.h"
#include "../cmd/cmd.h"
#include "../overlay/overlay.h"
#include "../firmware/roms.h"
#include "../mcp/json.h"

#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <vector>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

// header-only zip reader of guikit (lives in namespace GUIKIT inside guikit/api.cpp)
namespace GUIKIT {
#include "../../guikit/tools/decode/zip.h"
}

namespace SlangPresets {

// libretro/slang-shaders at the commit of Arch libretro-shaders-slang 20260620.024025.g8ec4c82cf61a
static const std::string Commit = "8ec4c82cf61a6ce3faae1e6111457d0d31920b60";
static const std::string ArchiveUrl = "https://codeload.github.com/libretro/slang-shaders/zip/" + Commit;
static const std::string ArchiveSha1 = "f2b5f48f5605242a7561d8a4e4f6667ae312c513";
static const size_t ArchiveSize = 52032481;
static const std::string SettingKey = "video_slang_preset";

static auto isDir(const std::string& path) -> bool {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static auto makeDirs(const std::string& path) -> bool {
    std::string current;
    for (size_t i = 0; i < path.size(); i++) {
        current += path[i];
        if ((path[i] == '/' || i == path.size() - 1) && current.size() > 1)
            ::mkdir(current.c_str(), 0755);
    }
    return isDir(path);
}

// ------------------------------------------------------------ sources

auto systemDir() -> std::string {
    const char* env = std::getenv("XDG_DATA_DIRS");
    std::string dirs = (env && *env) ? env : "/usr/local/share:/usr/share";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos)
            end = dirs.size();
        std::string d = dirs.substr(start, end - start);
        if (!d.empty()) {
            if (d.back() != '/')
                d += '/';
            if (isDir(d + "libretro/shaders/shaders_slang"))
                return d + "libretro/shaders/shaders_slang";
        }
        start = end + 1;
    }
    return "";
}

auto downloadTarget() -> std::string {
    // Roms::userDir() = <data home>/badline/roms/
    std::string roms = Roms::userDir();
    std::string base = roms.substr(0, roms.size() - 5); // strip "roms/"
    return base + "shaders/slang-shaders";
}

auto downloadedDir() -> std::string {
    auto dir = downloadTarget();
    return isDir(dir) ? dir : "";
}

auto downloadInfo() -> std::string {
    return "libretro/slang-shaders " + Commit.substr(0, 12) + " from github.com (52 MB, 66 MB unpacked)";
}

static auto writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    auto f = (FILE*)userdata;
    return std::fwrite(ptr, size, nmemb, f) * size;
}

auto download(const std::function<void(const std::string&)>& progress, std::string& error) -> bool {
    static bool curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curlReady) {
        error = "curl could not be initialized";
        return false;
    }
    std::string target = downloadTarget();
    std::string parent = target.substr(0, target.rfind('/'));
    if (!makeDirs(parent)) {
        error = "cannot create " + parent;
        return false;
    }
    std::string archive = parent + "/slang-shaders.zip.part";
    FILE* f = std::fopen(archive.c_str(), "w+b");
    if (!f) {
        error = "cannot write " + archive;
        return false;
    }
    if (progress)
        progress("Downloading " + downloadInfo());
    CURL* curl = curl_easy_init();
    std::string agent = "badline/" VERSION;
    // tests: BADLINE_SHADERS_URL (e.g. file:// of a local copy of the same archive)
    const char* overrideUrl = std::getenv("BADLINE_SHADERS_URL");
    std::string url = overrideUrl && *overrideUrl ? overrideUrl : ArchiveUrl;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    auto fail = [&](const std::string& e) {
        std::fclose(f);
        ::unlink(archive.c_str());
        error = e;
        return false;
    };
    if (res != CURLE_OK)
        return fail(curl_easy_strerror(res));

    long size = std::ftell(f);
    std::vector<uint8_t> data((size_t)std::max(size, 0L));
    std::rewind(f);
    if (std::fread(data.data(), 1, data.size(), f) != data.size())
        return fail("cannot read " + archive);
    if (data.size() != ArchiveSize || Roms::sha1(data.data(), data.size()) != ArchiveSha1)
        return fail("checksum mismatch (archive changed upstream?)");
    data.clear();
    data.shrink_to_fit();

    if (progress)
        progress("Unpacking into " + target);
    std::string staging = target + ".part";
    std::string cleanup = "rm -rf '" + staging + "'";
    if (isDir(staging) && std::system(cleanup.c_str()) != 0)
        return fail("cannot remove " + staging);
    GUIKIT::Zip zip;
    std::rewind(f);
    if (!zip.open(f, (unsigned)size))
        return fail("not a zip archive");
    for (auto& file : zip.files) {
        // "slang-shaders-<commit>/path": strip the top folder, refuse anything leaving it
        auto slash = file.name.find('/');
        if (slash == std::string::npos)
            continue;
        std::string rel = file.name.substr(slash + 1);
        if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos)
            continue;
        std::string out = staging + "/" + rel;
        if (file.isDirectory || rel.back() == '/') {
            makeDirs(out);
            continue;
        }
        makeDirs(out.substr(0, out.rfind('/')));
        uint8_t* bytes = file.size ? zip.extract(file) : nullptr;
        if (file.size && !bytes)
            return fail("cannot unpack " + rel);
        FILE* o = std::fopen(out.c_str(), "wb");
        bool ok = o && std::fwrite(bytes, 1, file.size, o) == file.size;
        if (o)
            std::fclose(o);
        delete[] bytes;
        file.data = nullptr;
        if (!ok)
            return fail("cannot write " + out);
    }
    std::fclose(f);
    ::unlink(archive.c_str());
    if (isDir(target)) {
        std::string old = target + ".old";
        std::string rmOld = "rm -rf '" + old + "'";
        std::system(rmOld.c_str());
        std::rename(target.c_str(), old.c_str());
        std::system(rmOld.c_str());
    }
    if (std::rename(staging.c_str(), target.c_str()) != 0) {
        error = "cannot rename " + staging;
        return false;
    }
    return true;
}

// ------------------------------------------------------------ loading

auto supported() -> bool {
    return videoDriver && !cmd->noGui && !cmd->noDriver;
}

static std::mutex mutex;              // state, request, worker
static std::condition_variable idle;
static State current;
static std::string pending;           // next request (latest wins)
static bool hasPending = false;
static bool pendingSave = false;
static bool working = false;
static std::thread worker;
static bool saveDone = false;         // finished load to be saved on the UI thread
static std::string saveSessionPath;
static bool sessionOverride = false;  // -shader applies until a runtime change

static auto settings() -> GUIKIT::Settings* {
    // the C64 settings (before power() there is no active emulator yet)
    auto emulator = activeEmulator ? activeEmulator : program->getEmulator("C64");
    return emulator ? Program::getSettings(emulator) : nullptr;
}

auto state() -> State {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

auto wanted() -> std::string {
    if (cmd->shaderSet && !sessionOverride)
        return cmd->shader;
    auto s = settings();
    return s ? s->get<std::string>(SettingKey, "") : "";
}

// runs a load (callers checked supported() on the UI thread); the caller does not hold `mutex`.
// The video driver is not replaced meanwhile: initVideo/quit wait() first.
static auto run(const std::string& path, std::string& result) -> bool {
    auto start = std::chrono::steady_clock::now();
    if (!videoDriver) {
        result = "no video driver";
        return false;
    }
    bool ok = videoDriver->setSlangPreset(path, "", result);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> lock(mutex);
    current.loading = false;
    current.requested.clear();
    if (ok) {
        current.path = path;
        current.error = false;
        current.passes = 0;
        Json j;
        std::string parseError;
        if (!path.empty() && Json::parse(result, j, parseError))
            current.passes = (unsigned)j.getInt("passes");
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
        current.status = path.empty() ? "" : std::to_string(current.passes) + (current.passes == 1 ? " pass, " : " passes, ") + buf;
    } else {
        current.error = true;
        // first line of the error, without the path prefix glslang/the parser put in front
        std::string line = result.substr(0, result.find('\n'));
        current.status = line;
    }
    return ok;
}

static auto workerLoop() -> void {
    std::unique_lock<std::mutex> lock(mutex);
    while (hasPending) {
        std::string path = pending;
        bool save = pendingSave;
        hasPending = false;
        current.loading = true;
        current.requested = path;
        lock.unlock();
        std::string result;
        bool ok = run(path, result);
        if (!ok && overlay && !overlay->isOpen())
            overlay->screenText("Shader preset failed: " + result.substr(0, result.find('\n')), 6, true);
        lock.lock();
        if (ok && save) {
            saveDone = true;
            saveSessionPath = path;
        }
    }
    working = false;
    if (overlay)
        overlay->busy("slang", false);
    idle.notify_all();
}

auto load(const std::string& path, bool save) -> void {
    if (!supported())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    pending = path;
    pendingSave = save;
    hasPending = true;
    current.loading = true;
    current.requested = path;
    if (working)
        return; // the running worker picks it up
    if (worker.joinable())
        worker.join();
    working = true;
    if (overlay)
        overlay->busy("slang", true);
    worker = std::thread(workerLoop);
}

auto wait() -> void {
    std::unique_lock<std::mutex> lock(mutex);
    idle.wait(lock, [] { return !working; });
    if (worker.joinable())
        worker.join();
}

auto loadNow(const std::string& path, bool save, std::string& result) -> bool {
    if (!supported()) {
        result = "Shader presets need the Vulkan renderer";
        return false;
    }
    wait();
    {
        std::lock_guard<std::mutex> lock(mutex);
        current.loading = true;
        current.requested = path;
    }
    bool ok = run(path, result);
    if (ok && save) {
        std::lock_guard<std::mutex> lock(mutex);
        saveDone = true;
        saveSessionPath = path;
    }
    poll();
    return ok;
}

auto applyWanted() -> void {
    {
        std::lock_guard<std::mutex> lock(mutex);
        current = State{};
    }
    if (!supported())
        return;
    auto path = wanted();
    if (!path.empty())
        load(path, false);
}

auto poll() -> void {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!saveDone)
            return;
        saveDone = false;
        path = saveSessionPath;
    }
    sessionOverride = true;
    if (auto s = settings())
        s->set<std::string>(SettingKey, path);
}

}
