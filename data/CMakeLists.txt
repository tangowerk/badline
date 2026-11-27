
add_library(icons OBJECT)

set(headers icons.h)

set_source_files_properties(icons ${headers}
        PROPERTIES HEADER_FILE_ONLY TRUE)

target_sources(icons PRIVATE
        icons/icons.cpp
        ${headers}
)
