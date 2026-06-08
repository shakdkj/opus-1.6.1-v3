# Unified test executable for watermark codec evaluation.
# Links against the opuswatermark shared library.

add_executable(test_watermark
    test_watermark.c
)

target_link_libraries(test_watermark PRIVATE opuswatermark)

target_include_directories(test_watermark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set_target_properties(test_watermark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test
)
