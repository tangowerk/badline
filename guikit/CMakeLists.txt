
add_library(guikit STATIC)

set(headers

        api.h
        tools/crc32.h
        tools/image.cpp
        tools/setting.cpp
        tools/string.cpp
        tools/utf8.cpp
        tools/system.cpp
        tools/decode/gzip.h
        tools/decode/images.h
        tools/decode/inflate.h
        tools/decode/lha.h
        tools/decode/png.h
        tools/decode/zip.h
        tools/encode/png.h
        tools/file/chunk.h
        tools/file/directory.h
        tools/file/file.cpp
        tools/file/tar.h
)

SET_SOURCE_FILES_PROPERTIES(tools/decode/lha.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/lha")

SET_SOURCE_FILES_PROPERTIES(tools/decode/images.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/stb")

SET_SOURCE_FILES_PROPERTIES(tools/resize.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/stb")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    SET_SOURCE_FILES_PROPERTIES(tools/decode/images.cpp
            PROPERTIES COMPILE_FLAGS "-Wno-stringop-overflow")
endif()

SET_SOURCE_FILES_PROPERTIES(tools/encode/images.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/stb")

set_source_files_properties(guikit ${headers}
        PROPERTIES HEADER_FILE_ONLY TRUE)

target_sources(guikit PRIVATE
        tools/decode/lha.cpp
        tools/decode/images.cpp
        tools/encode/images.cpp
        tools/resize.cpp
        api.cpp
        ${headers}
)

# the library lha is built in "deps/CMakeLists.txt"
target_link_libraries(guikit PRIVATE lha)


# link guikit as library: target_link_libraries( APP PUBLIC guikit )
