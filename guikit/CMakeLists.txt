
add_library(guikit STATIC)

target_compile_definitions(guikit PUBLIC GUIKIT_GTK)

SET_SOURCE_FILES_PROPERTIES(api.cpp PROPERTIES INCLUDE_DIRECTORIES "${GTK_INCLUDE_DIRS}")

set(headers
    gtk/widgets/button.cpp
    gtk/widgets/checkbox.cpp
    gtk/widgets/checkbutton.cpp
    gtk/widgets/combobutton.cpp
    gtk/widgets/frame.cpp
    gtk/widgets/hyperlink.cpp
    gtk/widgets/imageView.cpp
    gtk/widgets/label.cpp
    gtk/widgets/lineedit.cpp
    gtk/widgets/listview.cpp
    gtk/widgets/multilineedit.cpp
    gtk/widgets/progressbar.cpp
    gtk/widgets/radiobox.cpp
    gtk/widgets/slider.cpp
    gtk/widgets/squareCanvas.cpp
    gtk/widgets/stepbutton.cpp
    gtk/widgets/tabframe.cpp
    gtk/widgets/treeview.cpp
    gtk/widgets/viewport.cpp
    gtk/widgets/widget.cpp
    gtk/browserWindow.cpp
    gtk/display.cpp
    gtk/interProcess.cpp
    gtk/main.cpp
    gtk/main.h
    gtk/menu.cpp
    gtk/messageWindow.cpp
    gtk/statusbar.cpp
    gtk/tools.cpp
)

set(headers ${headers}
        api.h
        tools/crc32.h
        tools/image.cpp
        tools/layout.cpp
        tools/setting.cpp
        tools/string.cpp
        tools/translate.cpp
        tools/ttf.cpp
        tools/utf8.cpp
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
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/deps/stb;${CMAKE_SOURCE_DIR}/deps/cgif")

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

# the libraries lha and gif are built in "deps/CMakeLists.txt"
target_link_libraries(guikit PRIVATE lha gif)

target_link_libraries(guikit PUBLIC fontconfig Xrandr ${X11_LIBRARIES})

target_link_directories(guikit PUBLIC ${GTK_LIBRARY_DIRS})
target_link_libraries(guikit PUBLIC ${GTK_LIBRARIES})

# link guikit as library: target_link_libraries( APP PUBLIC guikit )
