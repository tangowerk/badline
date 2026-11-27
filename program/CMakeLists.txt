
add_executable(${PROJECT_NAME})

set(headers
        program.h
        audio/mixer/drive.h
        audio/resampler/cosine.h
        audio/resampler/cosineSSE.h
        audio/resampler/data.h
        audio/resampler/sinc.h
        audio/manager.h
        cmd/cmd.h
        config/layouts/drivers.h
        config/layouts/settings.h
        config/archiveViewer.h
        config/config.h
        config/slider.h
        emuconfig/config.h
        emuconfig/layouts/model.h
        emuconfig/layouts/presentation.h
        firmware/manager.h
        input/capture.cpp
        input/global.cpp
        input/hotkeys.cpp
        input/manager.h
        input/mapping.cpp
        media/autoloader.h
        media/fileloader.h
        media/layout.cpp
        media/media.h
        media/swapper.cpp
        media/recentFiles.h
        states/states.h
        thread/emuThread.h
        tools/chronos.h
        tools/circularBuffer.h
        tools/dataStorage.h
        tools/filepool.h
        tools/filesetting.h
        tools/gaussianBlur.h
        tools/logger.h
        tools/mathVector.h
        tools/shortcuts.h
        tools/sincFirFilter.h
        tools/httpClient.h
        tools/socket.h
        video/luts.cpp
        video/manager.h
        video/palette.h
        video/scVideo.h
        video/shaderParser.h
        video/sync.cpp
        view/message.h
        view/placeholder.cpp
        view/status.h
        view/view.h
        video.cpp
        helper/fileHelper.h
        helper/miscHelper.h
        helper/settingsHelper.h
        monitor/binaryMonitor.h
        mcp/json.h
        mcp/screen.h
        mcp/server.h
        video/screenshot.h
        window/sdlWindow.h
        firmware/roms.h
        overlay/font.h
        overlay/ui.h
        overlay/overlay.h
        overlay/media.h
)

set_source_files_properties(${PROJECT_NAME} ${headers}
        PROPERTIES HEADER_FILE_ONLY TRUE)

set(srcs
        program.cpp
        thread/emuThread.cpp
        input/manager.cpp
        view/view.cpp
        debugger/debugger.cpp
        debugger/cpuDebugger.cpp
        debugger/memDebugger.cpp
        debugger/ciaDebugger.cpp
        debugger/viaDebugger.cpp
        debugger/videoDebugger.cpp
        debugger/sidDebugger.cpp
        debugger/dmaDebugger.cpp
        debugger/conditionViewDebugger.cpp
        debugger/watcherHelper.cpp
        view/status.cpp
        config/config.cpp
        config/layouts/drivers.cpp
        config/layouts/settings.cpp
        emuconfig/config.cpp
        emuconfig/layouts/model.cpp
        emuconfig/layouts/presentation.cpp
        emuconfig/layouts/audio.cpp
        emuconfig/layouts/configurations.cpp
        emuconfig/layouts/firmware.cpp
        emuconfig/layouts/geometry.cpp
        emuconfig/layouts/input.cpp
        emuconfig/layouts/misc.cpp
        emuconfig/layouts/palette.cpp
        emuconfig/layouts/system.cpp
        media/media.cpp
        media/autoloader.cpp
        media/fileloader.cpp
        media/recentFiles.cpp
        config/archiveViewer.cpp
        states/states.cpp
        audio/manager.cpp
        audio/resampler/cosine.cpp
        audio/resampler/cosineSSE.cpp
        audio/resampler/sinc.cpp
        audio/mixer/drive.cpp
        firmware/manager.cpp
        cmd/cmd.cpp
        video/palette.cpp
        video/manager.cpp
        video/shaderParser.cpp
        video/scVideo.cpp
        tools/httpClient.cpp
        tools/diskGuesser.cpp
        tools/socket.cpp
        helper/fileHelper.cpp
        helper/miscHelper.cpp
        helper/settingsHelper.cpp
        monitor/binaryMonitor.cpp
        mcp/debug.cpp
        mcp/json.cpp
        mcp/screen.cpp
        mcp/server.cpp
        mcp/tools.cpp
        window/sdlWindow.cpp
        firmware/roms.cpp
        overlay/font.cpp
        overlay/ui.cpp
        overlay/overlay.cpp
        overlay/media.cpp
        
        ${headers}
)


SET_SOURCE_FILES_PROPERTIES(program.cpp
        PROPERTIES COMPILE_FLAGS "-DAPP_NAME=\"\\\"${AppName}\\\"\" -DTRANSLATION_FOLDER=\"\\\"${translationFolder}/\\\"\" -DDATA_FOLDER=\"\\\"${dataFolder}/\\\"\" -DPRESET_FOLDER=\"\\\"${presetFolder}/\\\"\" -DIMAGES_FOLDER=\"\\\"${imagesFolder}/\\\"\" -DSOUND_FOLDER=\"\\\"${soundFolder}/\\\"\" ")

target_link_libraries(${PROJECT_NAME} PUBLIC pthread rt dl -no-pie)
pkg_check_modules(CURL REQUIRED libcurl)
target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PUBLIC ${CURL_LIBRARIES})

target_sources(${PROJECT_NAME} PUBLIC ${srcs} )

# overlay UI: glyph atlas from the C64 fonts
if(NOT FREETYPE2_FOUND)
    message(FATAL_ERROR "freetype2 is required (overlay UI)")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${FREETYPE2_INCLUDE_DIRS} ${SDL3_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PUBLIC ${FREETYPE2_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PUBLIC ${FREETYPE2_LIBRARIES})

target_link_libraries(${PROJECT_NAME} PUBLIC guikit driver coreC64 icons)
