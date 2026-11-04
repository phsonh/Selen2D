include_guard(GLOBAL)

function(engine_stage_release)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Release staging currently requires a Windows x64 build")
    endif()
    if(NOT TARGET engine_app OR NOT TARGET DependencyDXC)
        message(FATAL_ERROR "Release staging requires engine_app and the static DependencyDXC target")
    endif()
    get_target_property(dxc_kind DependencyDXC TYPE)
    get_target_property(compiler_links engine_shader_compiler LINK_LIBRARIES)
    if(NOT dxc_kind STREQUAL "STATIC_LIBRARY" OR NOT "DependencyDXC" IN_LIST compiler_links)
        message(FATAL_ERROR "Release staging requires engine_shader_compiler to link static DependencyDXC")
    endif()
    if(TARGET stage_release)
        message(FATAL_ERROR "Release staging has already been registered")
    endif()

    # Ordinary builds prepare only their own runnable bin directory. They never
    # publish into the shared release directory or create a release ZIP.
    add_custom_target(stage_development ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/src/res/development/example"
            "$<TARGET_FILE_DIR:engine_app>/example"
        DEPENDS engine_app
        COMMENT "Preparing the example for the compiled development entry"
        VERBATIM)

    # This project uses single-configuration Ninja builds. Keep an explicit
    # failing target in Debug so a mistaken publish request cannot build or
    # copy anything into the official package.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_target(stage_release
            COMMAND "${CMAKE_COMMAND}" -E echo
                "stage_release requires a Release build; this configuration cannot publish"
            COMMAND "${CMAKE_COMMAND}" -E false
            VERBATIM)
        return()
    endif()

    string(TOLOWER "${ENGINE_PRODUCT_NAME}" release_product)
    if(NOT release_product MATCHES "^[a-z0-9][a-z0-9_-]*$")
        message(FATAL_ERROR "The product name must form a safe release directory name")
    endif()
    set(release_name "${release_product}-v${PROJECT_VERSION}-windows-x64")
    get_filename_component(linker_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(ENGINE_RELEASE_DUMPBIN NAMES dumpbin HINTS "${linker_directory}" REQUIRED)

    # Publishing is explicit and Release-only. Source Python tools and private
    # launch headers are never distributed here.
    add_custom_target(stage_release
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
            "-DENGINE_EXECUTABLE=$<TARGET_FILE:engine_app>"
            "-DLEGACY_PACK_FILENAME=${ENGINE_PACK_NAME}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DRELEASE_NAME=${release_name}"
            "-DDESTINATION=${PROJECT_SOURCE_DIR}/releases/${release_name}"
            "-DCONFIGURATION=$<CONFIG>"
            "-DDUMPBIN=${ENGINE_RELEASE_DUMPBIN}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CopyRelease.cmake"
        DEPENDS engine_app
        COMMENT "Staging the executable, Lua example and documentation; creating the release ZIP"
        VERBATIM)
endfunction()
