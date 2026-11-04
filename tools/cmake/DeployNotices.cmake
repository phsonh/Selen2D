include_guard(GLOBAL)
function(engine_deploy_notices target)
    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
            "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
            -P "${PROJECT_SOURCE_DIR}/tools/cmake/CopyNotices.cmake"
        COMMENT "Collecting project and source dependency licenses inside the build output" VERBATIM)
endfunction()
