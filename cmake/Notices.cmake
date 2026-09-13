function(audient_register_notices_target)
    add_custom_target(generate_notices ALL
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/NOTICE.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateNotices.cmake"
        COMMENT "Generating third-party notices")
endfunction()

audient_register_notices_target()