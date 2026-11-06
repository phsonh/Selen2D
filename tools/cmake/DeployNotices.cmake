include_guard(GLOBAL)
function(engine_deploy_notices target)
    # Revisit notices independently of EXE linking, including DLL-only updates.
    add_custom_target("${target}_notices" ALL
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
            "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
            -P "${PROJECT_SOURCE_DIR}/tools/cmake/CopyNotices.cmake"
        DEPENDS "${target}"
        COMMENT "Collecting project and source dependency licenses inside the build output" VERBATIM)
endfunction()
