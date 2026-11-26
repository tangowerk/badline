
add_library(driver STATIC)

set(headers
        audio/openal.cpp
        audio/pulseaudio.cpp
        input/udev.cpp
        input/xcb.cpp
        input/xlib.cpp
        video/opengl3/glx.cpp
)

set(headers ${headers}
        driver.h
        video/opengl3/bind.h
        video/opengl3/dragndropOverlay.h
        video/opengl3/gl3.cpp
        video/opengl3/progress.h
        video/opengl3/screenText.h
        video/opengl3/shaders.h
        video/opengl3/types.h
        video/opengl3/utility.h
        video/thread/renderThread.h
        video/dragnDropOverlay.h
        video/freetype.h
        video/viewport.h
        tools/chronos.h
        tools/crc32.h
        tools/glslang.h
        tools/hid.h
        tools/image.h
        tools/md5.h
        tools/ShaderCache.h
        tools/shaderpass.h
        tools/spirvReflection.h
        tools/threadPriority.h
        tools/tools.h
        tools/utf8.h
)

set_source_files_properties(driver ${headers}
        PROPERTIES HEADER_FILE_ONLY TRUE)

set(srcs
        driver.cpp
        tools/spirvReflection.cpp
        tools/ShaderCache.cpp
        video/thread/renderThread.cpp
)

SET_SOURCE_FILES_PROPERTIES(driver.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/glslang;")

SET_SOURCE_FILES_PROPERTIES(driver.cpp
        PROPERTIES COMPILE_FLAGS "-DDRV_GLX -DDRV_PULSEAUDIO -DDRV_XLIB -DDRV_XCB -DDRV_UDEV")

set_property(SOURCE driver.cpp APPEND_STRING PROPERTY INCLUDE_DIRECTORIES "${GTK_INCLUDE_DIRS};")

if(FREETYPE2_FOUND)
    set_property(SOURCE driver.cpp APPEND_STRING PROPERTY INCLUDE_DIRECTORIES "${FREETYPE2_INCLUDE_DIRS};")
    set_property(SOURCE driver.cpp APPEND_STRING PROPERTY COMPILE_FLAGS " -DDRV_FREETYPE ")
    target_link_libraries(driver PRIVATE ${FREETYPE2_LIBRARIES})
    target_link_directories(driver PRIVATE ${FREETYPE2_LIBRARY_DIRS})
endif()

if(OPENAL_FOUND)
    set_property(SOURCE driver.cpp APPEND_STRING PROPERTY INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIRS};")
    set_property(SOURCE driver.cpp APPEND_STRING PROPERTY COMPILE_FLAGS " -DDRV_OPENAL ")
    target_link_libraries(driver PRIVATE ${OPENAL_LIBRARY})
    target_link_directories(driver PRIVATE ${OPENAL_LIBRARY_DIRS})
endif()

target_link_directories(driver PUBLIC ${X11_LIBRARY_DIRS})
target_link_directories(driver PUBLIC ${XCB_LIBRARY_DIRS})
target_link_libraries(driver PUBLIC ${X11_LIBRARIES})
target_link_libraries(driver PUBLIC ${XCB_LIBRARIES})

# use -Wl,--verbose for detailed linker problems
target_link_libraries(driver PUBLIC GL pulse udev)

target_sources(driver PRIVATE ${srcs})

target_link_libraries(driver PRIVATE glslang spirv)