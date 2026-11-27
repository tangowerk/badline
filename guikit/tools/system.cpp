
// XDG base directory spec (also isolates test runs)
auto System::getUserDataFolder(std::string appIdent) -> std::string {
    std::string out;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        out = File::beautifyPath(xdg);
        if (out.back() != '/') out += "/";
    } else {
        struct passwd* userinfo = getpwuid(getuid());
        out = userinfo ? File::beautifyPath(userinfo->pw_dir) : "";
        if (out.length() == 0) out = "./";
        out += ".config/";
    }
    if (!appIdent.empty())
        out += appIdent + "/";
    return out;
}

static auto getResourceFolderBase(const std::string& sub) -> std::string {
    std::string resPath = "";
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX - 1);
    if (count > 0) {
        result[count] = 0;
        resPath = File::getPath(std::string(result));
        resPath += "../share/" + sub;

        if (File::isDir(resPath))
            return resPath;
    }

    for (auto& path : {"/usr/local", "/usr", "/usr/pkg"}) {
        resPath = std::string(path) + "/share/" + sub;
        if (File::isDir(resPath))
            return resPath;
    }

    struct passwd* userinfo = getpwuid(getuid());
    resPath = userinfo ? File::beautifyPath(userinfo->pw_dir) : "";
    if (resPath.length() == 0)
        resPath = "./";

    return resPath + ".local/share/" + sub;
}

auto System::getResourceFolder(std::string appIdent) -> std::string {
    static std::string resPath = "";
    if (resPath == "")
        resPath = getResourceFolderBase(appIdent);
    return File::beautifyPath(resPath);
}

auto System::getWorkingDirectory() -> std::string {
    char cwd[PATH_MAX] = "";
    if (getcwd(cwd, sizeof(cwd)) != NULL)
        return File::beautifyPath((std::string)cwd);
    return "./";
}

auto System::sleep(unsigned milliSeconds) -> void {
    usleep( milliSeconds * 1000 );
}

auto System::getOSLang() -> Language {
    // a single category returns the plain locale name, e.g. "fr_FR.UTF-8"
    // (LC_ALL gives a composite string as soon as LC_NUMERIC differs)
    auto result = setlocale(LC_MESSAGES, NULL);
    std::string str = result ? result : "";

    String::toLowerCase( str );

    if (str.find("de") != std::string::npos)
        return Language::DE;
    if (str.find("fr") != std::string::npos)
        return Language::FR;
    if (str.find("us") != std::string::npos)
        return Language::US;

    return Language::UK;
}

auto System::printToCmd( std::string str ) -> void {
    fprintf(stdout, "%s", str.c_str() );
}

auto ThreadPriority::setPriority( Mode mode, float minProcessingTimeInMilliSeconds, float maxProcessingTimeInMilliSeconds ) -> bool {
    const int policy = SCHED_RR;
    const int minPrio = sched_get_priority_min(policy);
    const int maxPrio = sched_get_priority_max(policy);
    sched_param sch_params;

    switch(mode) {
        default:
        case Mode::Normal:
            sch_params.sched_priority = (minPrio + maxPrio) / 2;
            break;
        case Mode::High:
            sch_params.sched_priority = maxPrio - 3;
            break;
        case Mode::Realtime:
            sch_params.sched_priority = maxPrio - 1;
            break;
    }

    return pthread_setschedparam( pthread_self(), policy, &sch_params) == 0;
}
