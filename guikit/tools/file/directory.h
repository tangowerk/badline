
auto File::removeDirectory( const std::string& path ) -> void {
    struct dirent* entry = nullptr;
    DIR* subDir = nullptr;
    DIR* dir = opendir( path.c_str() );
    if (!dir)
        return;

    while((entry = readdir(dir)) != nullptr) {
        std::string _name = entry->d_name;
        if (!_name.size())
            continue;
        subDir = nullptr;
        FILE* file = nullptr;
        std::string absPath;

        if ((_name[0] != '.') || ((_name.size() > 1) && (_name[1] != '.'))) {
            absPath = path + "/" + _name;

            if((subDir = opendir( absPath.c_str() )) != nullptr) {
                closedir( subDir );
                File::removeDirectory( absPath );
            } else {
                if((file = fopen(absPath.c_str(), "r")) != nullptr) {
                    fclose(file);

                    unlink( absPath.c_str() );
                }
            }
        }
    }

    closedir( dir );
    remove( path.c_str() );
}