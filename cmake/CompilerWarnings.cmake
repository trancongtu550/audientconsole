set(AUDIENT_WARNINGS_MSVC /W4 /permissive- /WX /Zc:__cplusplus /utf-8)

function(audient_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE ${AUDIENT_WARNINGS_MSVC})
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG>)
    endif()
endfunction()

function(audient_enable_asan target)
    if(AUDIENT_ENABLE_ASAN AND MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
    endif()
endfunction()

function(audient_enable_static_analysis target)
    if(AUDIENT_ENABLE_ANALYZE AND MSVC)
        target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
    endif()
endfunction()

function(audient_archive_symbols target)
    if(NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PDB_FILE:${target}>"
            "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>/"
        COMMENT "Archiving symbols for ${target}")
endfunction()

function(audient_configure_common_flags target)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF)
    audient_enable_warnings(${target})
    audient_enable_asan(${target})
    audient_enable_static_analysis(${target})
    target_compile_definitions(${target} PRIVATE AUDIENT_CONSOLE_BUILD_TYPE="$<CONFIG>")
endfunction()