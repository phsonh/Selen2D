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

    string(TOLOWER "${ENGINE_PRODUCT_NAME}" release_product)
    if(NOT release_product MATCHES "^[a-z0-9][a-z0-9_-]*$")
        message(FATAL_ERROR "The product name must form a safe release directory name")
    endif()
    set(release_name "${release_product}-v${PROJECT_VERSION}-windows-x64")
    get_filename_component(linker_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(ENGINE_RELEASE_DUMPBIN NAMES dumpbin HINTS "${linker_directory}" REQUIRED)

    # Always refresh the development shell and docs, even without relinking.
    # The packer is an optional tool and is neither built nor distributed here.
    add_custom_target(stage_release ALL
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
