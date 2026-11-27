#include "roms.h"
#include "../program.h"
#include "../cmd/cmd.h"

#include <curl/curl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace Roms {

static const std::string Zimmers = "http://www.zimmers.net/anonftp/pub/cbm/firmware/";
static auto appIdent() -> std::string {
    std::string name = APP_NAME;
    return GUIKIT::String::toLowerCase(name);
}

static const std::string ViceMirror = "https://raw.githubusercontent.com/VICE-Team/svn-mirror/main/vice/data/";

auto list() -> const std::vector<Rom>& {
    static const std::vector<Rom> roms = {
        {"Kernal", "1d503e56df85a62fee696e7618dc5b4e781df1bb", 8192,
            {"kernal.901227-03.bin", "kernal-901227-03.bin"},
            {Zimmers + "computers/c64/kernal.901227-03.bin", ViceMirror + "C64/kernal-901227-03.bin"}},
        {"Basic", "79015323128650c742a3694c9429aa91f355905e", 8192,
            {"basic.901226-01.bin", "basic-901226-01.bin"},
            {Zimmers + "computers/c64/basic.901226-01.bin", ViceMirror + "C64/basic-901226-01.bin"}},
        {"Char", "adc7c31e18c7c7413d54802ef2f4193da14711aa", 4096,
            {"characters.901225-01.bin", "chargen-901225-01.bin"},
            {Zimmers + "computers/c64/characters.901225-01.bin", ViceMirror + "C64/chargen-901225-01.bin"}},
        {"VC1541-II", "d3b78c3dbac55f5199f33f3fe0036439811f7fb3", 16384,
            {"1541-II.251968-03.bin", "dos1541ii-251968-03.bin"},
            {Zimmers + "drives/new/1541/1541-II.251968-03.bin", ViceMirror + "DRIVES/dos1541ii-251968-03.bin"}},
        {"VC1541", "ab16f56989b27d89babe5f89c5a8cb3da71a82f0", 16384,
            {"1541-II.355640-01.bin", "dos1541-325302-01+901229-05.bin"},
            {Zimmers + "drives/new/1541/1541-II.355640-01.bin", ViceMirror + "DRIVES/dos1541-325302-01+901229-05.bin"}},
        {"VC1541-C", "38a7a489c7bbc8661cf63476bf1eb07b38b1c704", 16384,
            {"1541C.251968-02.bin"},
            {Zimmers + "drives/new/1541/1541C.251968-02.bin"}},
        {"VC1570", "5fc06dc82ff6840f183bd43a4d9b8a16956b2f56", 32768,
            {"1570-rom.315090-01.bin", "dos1570-315090-01.bin"},
            {Zimmers + "drives/new/1571/1570-rom.315090-01.bin", ViceMirror + "DRIVES/dos1570-315090-01.bin"}},
        {"VC1571", "f1be619c106641a685f6609e4d43d6fc9eac1e70", 32768,
            {"1571-rom.310654-05.bin", "dos1571-310654-05.bin"},
            {Zimmers + "drives/new/1571/1571-rom.310654-05.bin", ViceMirror + "DRIVES/dos1571-310654-05.bin"}},
        {"VC1581", "01228eae6f066bd9b7b2b6a7fa3f667e41dad393", 32768,
            {"1581-rom.318045-02.bin", "dos1581-318045-02.bin"},
            {Zimmers + "drives/new/1581/1581-rom.318045-02.bin", ViceMirror + "DRIVES/dos1581-318045-02.bin"}},
    };
    return roms;
}

auto sourceHost() -> std::string {
    return "zimmers.net";
}

static auto fileExists(const std::string& path) -> bool {
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static auto makeDirs(const std::string& path) -> bool {
    std::string current;
    for (size_t i = 0; i < path.size(); i++) {
        current += path[i];
        if ((path[i] == '/' || i == path.size() - 1) && current.size() > 1)
            ::mkdir(current.c_str(), 0755);
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static auto withSlash(std::string dir) -> std::string {
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}

auto userDir() -> std::string {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && *xdg)
        base = withSlash(xdg);
    else {
        const char* home = std::getenv("HOME");
        base = withSlash(home ? home : ".") + ".local/share/";
    }
    return base + appIdent() + "/roms/";
}

auto searchDirs() -> std::vector<std::string> {
    std::vector<std::string> dirs;
    if (cmd && !cmd->romDir.empty())
        dirs.push_back(withSlash(cmd->romDir));
    dirs.push_back(userDir());
    if (program)
        dirs.push_back(program->dataFolder()); // distro packages
    return dirs;
}

static auto findRom(const std::string& name) -> const Rom* {
    for (auto& rom : list())
        if (rom.name == name)
            return &rom;
    return nullptr;
}

auto find(const std::string& name) -> std::string {
    auto rom = findRom(name);
    for (auto& dir : searchDirs()) {
        if (fileExists(dir + name))
            return dir + name;
        if (rom)
            for (auto& alias : rom->aliases)
                if (fileExists(dir + alias))
                    return dir + alias;
    }
    return "";
}

auto path(const std::string& name) -> std::string {
    auto found = find(name);
    return found.empty() ? userDir() + name : found;
}

auto missing() -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto& rom : list())
        if (find(rom.name).empty())
            out.push_back(rom.name);
    return out;
}

static auto writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    auto buffer = (std::vector<uint8_t>*)userdata;
    size_t bytes = size * nmemb;
    if (buffer->size() + bytes > 1024 * 1024) // ROMs are small, refuse anything big
        return 0;
    buffer->insert(buffer->end(), (uint8_t*)ptr, (uint8_t*)ptr + bytes);
    return bytes;
}

static auto fetch(const std::string& url, std::vector<uint8_t>& data, std::string& error) -> bool {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl init failed";
        return false;
    }
    data.clear();
    std::string agent = appIdent() + "/" VERSION;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        return false;
    }
    return true;
}

auto download(const std::function<void(const std::string&)>& progress, std::string& error) -> bool {
    static bool curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curlReady) {
        error = "curl could not be initialized";
        return false;
    }

    auto dir = userDir();
    if (!makeDirs(dir)) {
        error = "cannot create " + dir;
        return false;
    }

    bool allOk = true;
    for (auto& rom : list()) {
        if (!find(rom.name).empty())
            continue;

        bool stored = false;
        std::string lastError;
        for (auto& url : rom.urls) {
            if (progress)
                progress("Downloading " + rom.name + " from " + url);
            std::vector<uint8_t> data;
            if (!fetch(url, data, lastError))
                continue;
            if (data.size() != rom.size || sha1(data.data(), data.size()) != rom.sha1) {
                lastError = "checksum mismatch";
                continue;
            }
            std::string target = dir + rom.name;
            std::string temp = target + ".part";
            FILE* f = std::fopen(temp.c_str(), "wb");
            if (!f || std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
                if (f) std::fclose(f);
                lastError = "cannot write " + temp;
                continue;
            }
            std::fclose(f);
            std::rename(temp.c_str(), target.c_str());
            stored = true;
            break;
        }
        if (!stored) {
            allOk = false;
            error += (error.empty() ? "" : "; ") + rom.name + ": " + lastError;
        }
    }
    return allOk;
}

// --- SHA-1 (FIPS 180-1), only used to verify downloads
auto sha1(const uint8_t* data, size_t size) -> std::string {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); };

    std::vector<uint8_t> msg(data, data + size);
    uint64_t bitLen = (uint64_t)size * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; i--)
        msg.push_back((uint8_t)(bitLen >> (i * 8)));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) | (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    char out[41];
    for (int i = 0; i < 5; i++)
        std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 40);
}

}
