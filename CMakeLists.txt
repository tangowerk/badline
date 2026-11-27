cmake_minimum_required(VERSION 3.14)

project(denise LANGUAGES CXX)

set(AppName "Denise")
set(translationFolder "translation")
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
include(data/FindLame.cmake)
find_package(PulseAudio REQUIRED)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(X11 REQUIRED x11 xfixes xext)
pkg_check_modules(SDL3 REQUIRED sdl3)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

add_subdirectory(data)
add_subdirectory(deps)
add_subdirectory(guikit)
add_subdirectory(driver)
add_subdirectory(emulation/cia)
add_subdirectory(emulation/wdc/65C02)
add_subdirectory(emulation/wdc/65816)
add_subdirectory(emulation/libc64)
add_subdirectory(program)

# dev builds: the binary looks for its resources in ../share/<project> (see getResourceFolder)
set(devShare "${CMAKE_BINARY_DIR}/share/${PROJECT_NAME}")
file(MAKE_DIRECTORY "${devShare}")
foreach(folder ${translationFolder} ${dataFolder} ${fontFolder} ${presetFolder} ${imagesFolder} ${soundFolder})
    if(NOT EXISTS "${devShare}/${folder}")
        file(CREATE_LINK "${CMAKE_SOURCE_DIR}/data/${folder}" "${devShare}/${folder}" SYMBOLIC)
    endif()
endforeach()

install(DIRECTORY data/${translationFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME})
install(DIRECTORY data/${dataFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME})
install(DIRECTORY data/${fontFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME} FILES_MATCHING PATTERN "*.ttf")
install(DIRECTORY data/${soundFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME})
install(DIRECTORY data/${presetFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME})
install(DIRECTORY data/${imagesFolder} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME})

install(FILES data/${PROJECT_NAME}.appdata.xml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/appdata)
install(FILES data/img/${PROJECT_NAME}.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/256x256/apps)
install(FILES data/img/${PROJECT_NAME}_48.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/48x48/apps RENAME ${PROJECT_NAME}.png)
install(FILES data/img/${PROJECT_NAME}_48.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons RENAME ${PROJECT_NAME}.png)
install(FILES data/img/${PROJECT_NAME}_32.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/32x32/apps RENAME ${PROJECT_NAME}.png)
install(FILES data/${PROJECT_NAME}.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ OWNER_EXECUTE GROUP_EXECUTE WORLD_EXECUTE)
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
