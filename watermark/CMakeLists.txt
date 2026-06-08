# Watermark API shared library.
# Links against the internal opus static library, exports only the
# WatermarkEncode / WatermarkDecode symbols.

add_library(opuswatermark SHARED
    watermark_opus.c
)

target_link_libraries(opuswatermark PRIVATE opus)

target_include_directories(opuswatermark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../silk
        ${CMAKE_CURRENT_SOURCE_DIR}/../celt
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(opuswatermark PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../include/watermark.h"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(TARGETS opuswatermark
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
