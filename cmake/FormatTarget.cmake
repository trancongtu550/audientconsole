find_program(AUDIENT_CLANG_FORMAT_EXECUTABLE clang-format)
find_program(AUDIENT_CLANG_TIDY_EXECUTABLE clang-tidy)

file(GLOB_RECURSE AUDIENT_FORMAT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench/*.h")

if(AUDIENT_CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
        COMMAND "${AUDIENT_CLANG_FORMAT_EXECUTABLE}" -i ${AUDIENT_FORMAT_SOURCES}
        COMMENT "Formatting project sources with clang-format")
    add_custom_target(format-check
        COMMAND "${AUDIENT_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${AUDIENT_FORMAT_SOURCES}
        COMMENT "Checking formatting with clang-format")
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found; format target skipped")
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found; format-check target skipped")
endif()

if(AUDIENT_CLANG_TIDY_EXECUTABLE)
    add_custom_target(lint
        COMMAND "${AUDIENT_CLANG_TIDY_EXECUTABLE}" -p "${CMAKE_CURRENT_BINARY_DIR}" ${AUDIENT_FORMAT_SOURCES}
        COMMENT "Linting project sources with clang-tidy")
else()
    add_custom_target(lint
        COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found; lint target skipped")
endif()