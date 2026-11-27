#pragma once

// GTK-free tools library (settings, files + archive decoders, strings, images, timers, main loop).
// The GTK widget toolkit was removed in M9.

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <iterator>
#include <optional>
#include <cstdint>

namespace GUIKIT {

struct Zip;
struct Gzip;
struct Tar;
struct Lha;
struct File;

struct Image {
    unsigned width;
    unsigned height;
    uint8_t* data = nullptr;
    uint8_t* resizedData = nullptr;
    bool keepDataOnDestruction = false;
    bool alphaBlendApplied = false;
    int resourceId = -1; // win xp only 
    enum Format : unsigned { RGBA, BGRA, RGB } format;
    enum class Type { PNG, JPG, TGA, BMP };

    struct Encoded {
        uint8_t* data = nullptr;
        unsigned size = 0;
        Type type;
    };

    auto loadPng(const uint8_t* src, unsigned size, bool keepDataOnDestruction = false) -> bool;
    auto load(const uint8_t* src, unsigned size, bool keepDataOnDestruction = false) -> bool;
    auto generate(Type type, const uint8_t* src, unsigned width, unsigned height) -> Encoded;
    auto generate(Type type) -> Encoded;
    auto generate(Type type, std::vector<uint32_t>& colorTable, const std::vector<uint8_t*>& srcs, unsigned width, unsigned height, unsigned delay = 1) -> Encoded;
    auto generatePng( uint8_t* rgbData, unsigned width, unsigned height, unsigned channels, unsigned& pngSize ) -> uint8_t*;
    auto alphaBlend(unsigned alphaColor) -> void;
	auto alphaMultiply() -> void;
    auto scaleNearest(unsigned outputWidth, unsigned outputHeight) -> void;
    auto scaleLinear(unsigned outputWidth, unsigned outputHeight) -> void;
    auto scaleLinearWidth(unsigned outputWidth) -> void;
    auto scaleLinearHeight(unsigned outputHeight) -> void;
    auto scaleLinearBoth(unsigned outputWidth, unsigned outputHeight) -> void;

    auto resize(unsigned outputWidth, unsigned outputHeight) -> uint8_t*;

    auto switchBetweenBGRandRGB() -> void;
    auto empty() -> bool;
    auto free() -> void;
    auto channels() -> unsigned;
    auto create(unsigned _width, unsigned _height, uint8_t* src = nullptr) -> void;
    auto setResourceId( int rId ) -> void;
    //helper to convert binary file to comma separated hardcoded unsigned char sequence
    static auto getCharDataStringFromBinary(std::string inFile, std::string outFile) -> bool;    

    Image() : width(0), height(0), data(nullptr), keepDataOnDestruction(false), format(RGBA) {}
    Image(unsigned width, unsigned height, uint8_t* src, Format format = RGBA);
    Image(const Image& source);
    Image(Image&& source);
    ~Image();

    Image& operator=(const Image& source);
    Image& operator=(Image&& source);
protected:
    auto read(uint8_t* p) -> unsigned;
    auto write(uint8_t* p, unsigned value) -> void;
    auto normalize(uint64_t color, unsigned sourceDepth, unsigned targetDepth) -> uint64_t;
};

struct Size {
    static const unsigned Maximum = ~0u;
    static const unsigned Minimum =  0u;

    unsigned width, height;
    Size() : width(0), height(0) {}
    Size(unsigned width, unsigned height) : width(width), height(height) {}
};

struct Position {
    signed x, y;
    Position() : x(0), y(0) {}
    Position(signed x, signed y) : x(x), y(y) {}
};

struct Geometry {
    signed x, y;
    unsigned width, height;
    Position position() const { return {x, y}; }
    auto size() const -> Size { return {width, height}; }
    Geometry() : x(0), y(0), width(0), height(0) {}
    Geometry(Position position, Size size) : x(position.x), y(position.y), width(size.width), height(size.height) {}
    Geometry(signed x, signed y, unsigned width, unsigned height) : x(x), y(y), width(width), height(height) {}
};

struct Application {
    static std::function<void ()> loop;

    static auto run() -> void;   // calls loop() and due timers until quit()
    static auto quit() -> void;

    static bool isQuit;
    static int exitCode;
    static std::string name;
    static std::string vendor;

    Application() = delete;
};

// polled timer: fires on the thread that runs Application::run (UI thread)
struct Timer {
    std::function<void ()> onFinished = nullptr;

    struct State {
        bool enabled = false;
        unsigned interval = 0;
        unsigned data = 0;
        float dataF = 0.0;
    } state;

    auto enabled() const -> bool { return state.enabled; }
    auto interval() const -> unsigned { return state.interval; }
    auto data() const -> unsigned { return state.data; }
    auto dataF() const -> float { return state.dataF; }
    auto setEnabled(bool enabled = true) -> void;
    auto setInterval(unsigned intervalInMs) -> void;
    auto setData(unsigned data) -> void { state.data = data; }
    auto setDataF(float data) -> void { state.dataF = data; }

    static auto tickAll() -> void;

    Timer();
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
private:
    int64_t due = 0; // steady clock, microseconds
    static std::vector<Timer*> timers;
};

struct System {
    enum class Language { DE, UK, US, FR };
    static auto getUserDataFolder(std::string appIdent = "") -> std::string;
    static auto getResourceFolder(std::string appIdent = "") -> std::string;
    static auto getWorkingDirectory() -> std::string;
    static auto sleep(unsigned milliSeconds) -> void;
    static auto getOSLang() -> Language;
    static auto printToCmd( std::string str ) -> void;
    System() = delete;
};

// priority changes of std::threads isn't portable and needs to be handled carefully.
// furthermore it's not possible (Windows, macOS) to change priority of std::thread from other threads, so priority is changed only for caller thread
struct ThreadPriority {

    enum class Mode { Normal = 0, High = 1, Realtime = 2 };

    static auto setPriority( Mode mode, float typicalProcessingTimeInMilliSeconds = 0, float maxProcessingTimeInMilliSeconds = 0 ) -> bool;
};

struct File {
    struct Info {
        std::string name;
        std::string date;
        uint64_t size = 0;
        bool exists = true;
        bool isDir = false;
    };
    struct Item {
        unsigned id;
        Info info;
        bool isDirectory = false;
        Item* parent = nullptr;
        std::vector<Item*> childs;
    };
    enum class Mode { Read, Write, Update, Append };
    enum class Type { Default, Zip, TarGz, Gzip, Tar, Lha };
    //overall file access, compressed archives are not considered
    auto open(Mode mode = Mode::Read, bool createFolderIfNotExists = false) -> bool;
    auto read() -> uint8_t*;
	auto read(uint8_t* buffer, unsigned length, uint64_t offset = 0) -> unsigned;
    auto write() -> bool;
    auto write(const uint8_t* buffer, unsigned length, uint64_t offset = 0) -> unsigned;
    auto append(const uint8_t* buffer, unsigned length) -> unsigned;
    auto truncate() -> bool;
    auto getSize() const -> uint64_t { return fileInfo.size; }
    auto exists() const -> bool { return fileInfo.exists; }
    auto getDate() const -> std::string { return fileInfo.date; }
    auto getType() const -> Type { return type; }
    auto getFileName(bool removeExtension = false, bool truncateFromEnd = false) -> std::string;
    auto getFileName(Item* item) -> std::string;
    auto getPath() -> std::string;
    auto getExtension() -> std::string;
    auto getHandle() -> FILE* { return fp; }
    auto del() -> bool;
    
    auto setReadOnly() -> void { readOnly = true; }
    auto isReadOnly() -> bool { return readOnly; }
    // inform about data was changed by another task handling the same file
    auto forceDataChange() -> void { dataChanged = true; }
    auto wasDataChanged() -> bool { return dataChanged; }

    //archive access
    auto scanArchive() -> std::vector<Item>&;
    auto archiveData(unsigned id) -> uint8_t*;
    auto freeArchiveData(unsigned id) -> void;
    auto archiveDataSize(unsigned id) -> uint64_t;
    
    //helper
    auto isSizeValid(uint64_t maxSize) -> bool;
    auto isSizeValid(unsigned fileId, uint64_t maxSize) -> bool;
    auto isArchived() -> bool;
    static auto SizeFormated(uint64_t bytes) -> std::string;

    //static
    static auto suppportedCompressionExtensions() -> std::vector<std::string> { 
        return {"zip", "gz", "lha", "lzh", "tar", "tgz", "tar.gz"};
    }
    static auto suppportedCompressionFilter() -> std::string { return "zip, gz, lha, lzh, tar, tgz, tar.gz (*.zip,*.gz,*.lha,*.lzh,*.tar,*.tgz,*.tar.gz)"; }
    static auto getFolderList( std::string path, const std::string& subStr = "") -> std::vector<Info>;
    static auto getFolderListAlt( std::string path, std::vector<std::string> subStrs, bool fromBeginning, unsigned limit = 0, const std::string& suffix = "" ) -> std::vector<std::string>;
    static auto getFileList(const std::string& path, const std::string& subPath = "" ) -> std::vector<std::string>;
    static auto isDir( std::string path ) -> bool;
    static auto createDir( std::string path, std::string basePath = "" ) -> bool;
    static auto beautifyPath(std::string path) -> std::string;
    static auto getOffsetDataStringFromBinary( std::string inFile, std::string outFile ) -> bool;

    static auto getPath( std::string _fn, bool returnSlashIfError = false ) -> std::string;
    static auto resolveRelativePath(std::string _fn, std::string relPath) -> std::string;
    static auto buildRelativePath(std::string refPath, std::string targetPath, bool onlyWhenInRefPath = false) -> std::string;
    static auto resolveRelativePath(std::string relPath ) -> std::string;
    static auto buildRelativePath(std::string targetPath) -> std::string;
    static auto isAbsolute(const std::string& path) -> bool;
    static auto removeDirectory( const std::string& path ) -> void;
    static auto xcopy(const std::string& src, const std::string& dest) -> bool;

    auto setFile(std::string filePath) -> void;
    auto getFile() const -> std::string { return filePath; }
    auto unload() -> void;
    auto reset() -> void;
    File(std::string filePath = "", bool keepDataOnDestruction = false);
    ~File();
    File(const File& source);
    File& operator=(const File& source);
private:
    auto detectType() -> void;
    auto freeData(uint8_t** dataPtr) -> void;
    auto close() -> void;
    static auto setStats(std::string path, Info& info) -> void;
    static auto _createDir( std::string path ) -> bool;
    auto connectItems() -> void;

    std::string filePath = "";
    Type type;
    Mode mode;
    Info fileInfo;
    std::vector<Item> items;
    FILE* fp = nullptr;
    uint8_t* data = nullptr;
    bool dataChanged = false;
    bool readOnly = false;
    bool keepDataOnDestruction = false;
    
    Zip* zip;
    Gzip* gzip;
    Tar* tar;
    Lha* lha;
};

struct Setting {
    friend class Settings;

    unsigned uValue = ~0u; //for fast access without string conversion
    int iValue = 1 << ( (sizeof(int) << 3) - 1 ); //for fast access without string conversion
    std::string value = "";
    bool saveable = true;

    auto operator==(int data) -> bool;
    auto operator!=(int data) -> bool;
    auto operator=(int data) -> void;
    operator int();
    auto operator==(unsigned data) -> bool;
    auto operator!=(unsigned data) -> bool;
    auto operator=(unsigned data) -> void;
    operator unsigned();
	operator bool();
	
	std::vector<Setting*> childs;
	auto add(const std::string& ident) -> Setting*;
	auto set(std::string data) -> void;
	auto getIdent() -> std::string { return ident; }
    auto setIdent(std::string ident ) -> void { this->ident = ident; }
protected:
    std::string ident;
    Setting(const std::string& ident);
};

struct Settings {
    auto find(const std::string& ident) -> Setting*;
    auto add(const std::string& ident) -> Setting*;
    auto add(Setting* setting) -> void;
    auto remove(const std::string& ident) -> bool;
    auto changeIdent(const std::string& ident, const std::string& newIdent) -> void;
    auto clear() -> void;
    auto hasDepthError() -> bool { return errorDepth; }
    auto getBrokenPaths() -> std::vector<std::string>& { return brokenPaths; }

    template<typename T> class type_info{};

    template<typename T>
    auto get(const std::string& ident, T defaultValue = T(), std::vector<T> range = {} ) -> T {
        auto result = get(type_info<T>(), ident, defaultValue);
		return !range.empty() ? std::min<T>( std::max<T>(result, (T)range[0]), (T)range[1] ) : result;
    }
	
	template<typename T> auto getOrInit(const std::string& ident, T defaultValue = T(), std::vector<T> range = {}) -> Setting* {
		auto setting = find( ident );
		if(setting) defaultValue = get(ident, defaultValue, range);
		set(type_info<T>(), ident, defaultValue, true);
		return find( ident );
	}
	
    template<typename T> auto set(const std::string& ident, T value, bool saveable = true) -> void {
        if (ident.empty()) return;
        set(type_info<T>(), ident, value, saveable);
    }

    auto setSaveable( const std::string& ident, bool state ) -> void;
    auto load(const std::string& path) -> bool { return loadEx(path, -1, ':'); }
    auto loadEx(const std::string& path, int depth = 0, const char separator = '=') -> bool;
    auto save(const std::string& path) -> bool;
    auto getList() -> std::vector<Setting*>& { return list; }
    auto getReferences() -> std::vector<std::string>& { return references; }
    auto getPath() -> std::string& { return path; }
    
    auto setGuid(void* guid) -> void { this->guid = guid; }
    auto getGuid() -> void* { return guid; }
    
    ~Settings();

private:
    auto set(type_info<bool> t, const std::string& ident, bool value, bool saveable) -> void;
    auto set(type_info<int> t, const std::string& ident, int value, bool saveable) -> void;
    auto set(type_info<unsigned> t, const std::string& ident, unsigned value, bool saveable) -> void;
    auto set(type_info<float> t, const std::string& ident, float value, bool saveable) -> void;
    auto set(type_info<double> t, const std::string& ident, double value, bool saveable) -> void;
    auto set(type_info<std::string> t, const std::string& ident, std::string value, bool saveable) -> void;

    auto get(type_info<bool> t, const std::string& ident, bool defaultValue) -> bool;
    auto get(type_info<int> t, const std::string& ident, int defaultValue) -> int;
    auto get(type_info<unsigned> t, const std::string& ident, unsigned defaultValue) -> unsigned;
    auto get(type_info<float> t, const std::string& ident, float defaultValue) -> float;
    auto get(type_info<double> t, const std::string& ident, double defaultValue) -> double;
    auto get(type_info<std::string> t, const std::string& ident, std::string defaultValue) -> std::string;

    auto stripCommentsAndDetectIncludes(std::string& line) -> bool;

    std::vector<std::string> references;
    std::vector<std::string> brokenPaths;
    std::string path;
    std::vector<Setting*> list;
    void* guid = nullptr;
    bool errorDepth = false;
};

//helpers
struct String {
    static auto toLowerCase( std::string& str ) -> std::string&;
    static auto toUpperCase( std::string& str ) -> std::string&;
    static auto trim(std::string& str) -> std::string&;
    static auto delSpaces(std::string& str) -> std::string&;
    static auto capitalize(std::string& str) -> std::string&;
    static auto split(const std::string& str, char delimiter, bool trimParts = true) -> std::vector<std::string>;
    static auto explode(std::string str, std::string delimiter) -> std::vector<std::string>;
    static auto unsplit( const std::vector<std::string>& parts, std::string delimiter ) -> std::string;
    static auto foundSubStr(std::string& str, std::string subStr) -> bool;
    static auto findString(const std::string& strHaystack, const std::string& strNeedle) -> bool; // ignore case
    static auto endsWith(std::string& str, std::string suffix) -> bool;
    static auto startsWith(const std::string& str, const std::string& prefix) -> bool;
    static auto removeQuote(std::string& str, bool oneSideOnly = false) -> std::string&;
    static auto remove(std::string& str, const std::vector<std::string>& subStr) -> std::string&;
    static auto replace(std::string& str, const std::string& search, const std::string& replace) -> std::string&;
    // replaces each {search, replacement} pair, e.g. substitute("%n% files", {{"%n%", "3"}})
    static auto substitute(std::string text, const std::vector<std::vector<std::string>>& replaces) -> std::string;
    static auto isNumber(const std::string& str) -> bool;
    static auto isFloatNumber(const std::string& str) -> bool;
	static auto convertToNumber(std::string str, int defaultVal = 0) -> int;
    static auto convertToHex( unsigned val, int length = -1 ) -> std::string;
    static auto convertIntToHex( int number, bool prepend_0x = true ) -> std::string;
    static auto convertHexToInt( std::string hex, int defaultValueByFailure = 0 ) -> int;
    static auto formatFloatingPoint(double value, uint8_t roundDecimal = 0, bool cutTrailingZero = false) -> std::string;
    static auto countDecimalPlaces(double value, int& places) -> int;
    static auto prependZero( std::string str, unsigned width ) -> std::string;
    static auto prependLeft( std::string str, char placeHolder, unsigned width ) -> std::string;
    static auto removeDuplicates( std::vector<std::string>& strs ) -> void;
    static auto convertDoubleToString(double value, unsigned precision = 18) -> std::string;
    static auto findOccurencesOf( std::string str, std::string subStr ) -> unsigned;
    static auto getFileName(std::string path, bool removeExtension = false) -> std::string;
    static auto getFileNameA(std::string path, bool removeExtension = false) -> std::string;
    static auto getExtension(const std::string& str, const std::string& defaultExt, int maxParts = 1, int maxPartSize = 3) -> std::string;
    static auto removeExtension(std::string str, int maxParts = 1, int maxPartSize = 3) -> std::string;
    static auto sgets(char* buf, unsigned& bufSize, unsigned& n, char** str) -> char*;
    static auto getDomain(const std::string& str, std::string& path) -> std::string;
    static auto equalFromEnd(const std::string& a, const std::string& b) -> bool;

    template<typename T> static auto addThousandSeparator(T digit) -> std::string {
        return addThousandSeparator( std::to_string( digit ) );
    }
    static auto addThousandSeparator(std::string digit) -> std::string;
    String() = delete;
};

struct Vector {
	template<typename T>
    static auto eraseVectorElement(std::vector<T>& v, T element) -> bool {
		for (unsigned i = 0; i < v.size(); i++)
			if (v[i] == element) return v.erase(v.begin() + i), true;
		return false;
	}
    
	template<typename T>
    static auto eraseVectorPos(std::vector<T>& v, unsigned position, unsigned length=1) -> bool {
		if (v.size() < (position + length)) return false;
		for (unsigned l = 0; l < length; l++) v.erase(v.begin() + position);
		return true;
	}
    template<typename T>
    static auto find(std::vector<T>& v, T element) -> bool {        
        return std::find(v.begin(), v.end(), element) != v.end();
    }
    template<typename T>
    static auto find(const std::vector<T*>& v, const T& element) -> bool {
	    return std::find_if(v.begin(), v.end(),
            [&element](const T* ptr) {
                return ptr != nullptr && *ptr == element;
            }) != v.end();
	}
    template<typename T>
    static auto findPos(std::vector<T>& v, T element) -> int {
		for (unsigned i = 0; i < v.size(); i++)
			if (v[i] == element) return i;
		return -1;
	}
    template<typename T>
    static auto combine(std::vector<T>& target, const std::vector<T>& source, bool prepend = false) -> void {
        target.insert( prepend ? target.begin() : target.end(), source.begin(), source.end() );
    }
    template<typename T>
    static auto combine(std::vector<T*>& target, const std::vector<T*>& source, bool prepend = false) -> void {
	    target.insert( prepend ? target.begin() : target.end(), source.begin(), source.end() );
	}
    template <typename T> 
    static auto concat(std::vector<T>& v1, std::vector<T>& v2) -> std::vector<T> {
        std::vector<T> concated = std::vector<T>();
        copy(v1.begin(), v1.end(), back_inserter( concated ));
        copy(v2.begin(), v2.end(), back_inserter( concated ));
        return concated;
    }
    template <typename T> 
    static auto insert(std::vector<T>& v, T value, unsigned pos) -> void {
        auto itPos = v.begin() + ( (pos >= v.size()) ? v.size() : pos);
        v.insert( itPos, value );
    }
    template <typename T>
    static auto getElements(const std::vector<T>& v, unsigned count) -> std::vector<T> {
	    std::vector<T> crop(v.begin(), v.begin() + std::min<size_t>(count, v.size()));
	    return crop;
	}
    Vector() = delete;
};

struct Utf8 {
    // returns utf8 length of each scancode
    static auto encode( unsigned code, std::vector<uint8_t>& out ) -> unsigned;
    // returns scancode
    static auto decode( std::string text, unsigned& pos ) -> unsigned;
};

}
