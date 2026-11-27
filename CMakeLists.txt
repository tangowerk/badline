cmake_minimum_required(VERSION 3.14)

project(badline LANGUAGES CXX)

set(AppName "Badline")
set(dataFolder "data")
set(fontFolder "fonts")
set(presetFolder "presets")
set(imagesFolder "images")
set(soundFolder "sounds")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED YES)

set(ltoSupported 0)

if(DEFINED ENABLE_LTO AND ENABLE_LTO AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4) # no 32 bit support
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ltoSupported)
endif()

message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")
if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
endif()

if(CMAKE_BUILD_TYPE STREQUAL Release)
    add_link_options( -s )
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3") # prevents NDEBUG, which slightly decrease execution speed
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

message(STATUS "Setting Install Folder to ${CMAKE_INSTALL_PREFIX}")

set(architecture)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(architecture "x86")
    message(STATUS "32 bit build")
else()
    set(architecture "x64")
endif()

find_package(Freetype)
find_package(PkgConfig REQUIRED)
find_package(OpenAL)
pkg_check_modules(FREETYPE2 freetype2)
find_package(PulseAudio REQUIRED)
# prefer the patched SDL3 from tools/build_sdl3.sh (builds/sdl3) when it exists
set(BADLINE_SDL3_PREFIX "${CMAKE_SOURCE_DIR}/builds/sdl3" CACHE PATH "SDL3 built by tools/build_sdl3.sh (used if present)")
if(EXISTS "${BADLINE_SDL3_PREFIX}/lib/pkgconfig/sdl3.pc")
    set(ENV{PKG_CONFIG_PATH} "${BADLINE_SDL3_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    if(NOT CMAKE_INSTALL_RPATH)
        set(CMAKE_INSTALL_RPATH "${BADLINE_SDL3_PREFIX}/lib")
    endif()
    message(STATUS "Using SDL3 from ${BADLINE_SDL3_PREFIX}")
else()
    message(WARNING "Patched SDL3 not found (run tools/build_sdl3.sh); the system SDL3 drops files from some Wayland drag sources")
endif()
pkg_check_modules(SDL3 REQUIRED sdl3)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

add_subdirectory(deps)
add_subdirectory(guikit)
add_subdirectory(driver)
add_subdirectory(emulation/cia)
add_subdirectory(emulation/libc64)
add_subdirectory(program)

# dev builds: the binary looks for its resources in ../share/<project> (see getResourceFolder)
set(devShare "${CMAKE_BINARY_DIR}/share/${PROJECT_NAME}")
file(MAKE_DIRECTORY "${devShare}")
foreach(folder ${dataFolder} ${fontFolder} ${presetFolder} ${imagesFolder} ${soundFolder})
    if(EXISTS "${CMAKE_SOURCE_DIR}/data/${folder}" AND NOT EXISTS "${devShare}/${folder}")
        file(CREATE_LINK "${CMAKE_SOURCE_DIR}/data/${folder}" "${devShare}/${folder}" SYMBOLIC)
    endif()
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/data/${dataFolder}") # currently empty (nothing shipped), still searched for ROMs
    install(DIRECTORY data/${dataFolder} DESTINATION share/${PROJECT_NAME})
endif()
install(DIRECTORY data/${fontFolder} DESTINATION share/${PROJECT_NAME} FILES_MATCHING PATTERN "*.ttf" PATTERN "*LICENSE*")
install(FILES LICENSE NOTICE.md README.md CHANGELOG.md DESTINATION share/doc/${PROJECT_NAME})
install(DIRECTORY data/${soundFolder} DESTINATION share/${PROJECT_NAME})
install(DIRECTORY data/${presetFolder} DESTINATION share/${PROJECT_NAME})
install(DIRECTORY data/${imagesFolder} DESTINATION share/${PROJECT_NAME})

install(FILES data/${PROJECT_NAME}.appdata.xml DESTINATION share/appdata)
install(FILES data/img/${PROJECT_NAME}.png DESTINATION share/icons/hicolor/256x256/apps)
install(FILES data/img/${PROJECT_NAME}_48.png DESTINATION share/icons/hicolor/48x48/apps RENAME ${PROJECT_NAME}.png)
install(FILES data/img/${PROJECT_NAME}_48.png DESTINATION share/icons RENAME ${PROJECT_NAME}.png)
install(FILES data/img/${PROJECT_NAME}_32.png DESTINATION share/icons/hicolor/32x32/apps RENAME ${PROJECT_NAME}.png)
install(FILES data/${PROJECT_NAME}.desktop DESTINATION share/applications PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ OWNER_EXECUTE GROUP_EXECUTE WORLD_EXECUTE)
install(CODE "execute_process(COMMAND update-icon-caches ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor)")

if(INSTALL_FILE_ASSOCIATIONS STREQUAL 1)
    install(FILES data/application-x-${PROJECT_NAME}.xml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/mime/packages)
    install(CODE "execute_process(COMMAND update-mime-database ${CMAKE_INSTALL_PREFIX}/share/mime)")
    install(CODE "execute_process(COMMAND update-desktop-database ${CMAKE_INSTALL_PREFIX}/share/applications)")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

add_custom_target(install_${PROJECT_NAME}
        "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Installing ${PROJECT_NAME}")

add_custom_target("uninstall" COMMENT "Uninstall installed files")
add_custom_command(
        TARGET "uninstall"
        POST_BUILD
        COMMENT "Uninstall files with install_manifest.txt"
        COMMAND xargs rm -vf < install_manifest.txt || echo Nothing to be uninstalled!
)
