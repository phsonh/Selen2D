include_guard(GLOBAL)

function(engine_stage_release)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Release staging currently requires a Windows x64 build")
    endif()
    if(NOT TARGET engine_app OR NOT TARGET DependencyDXC OR NOT TARGET engine_dxc_build)
        message(FATAL_ERROR "Release staging requires engine_app and the DXC runtime build targets")
    endif()
    get_target_property(dxc_kind DependencyDXC TYPE)
    get_target_property(dxc_runtime_file DependencyDXC ENGINE_DXC_RUNTIME_FILE)
    if(NOT dxc_kind STREQUAL "INTERFACE_LIBRARY" OR NOT IS_ABSOLUTE "${dxc_runtime_file}")
        message(FATAL_ERROR "Release staging requires an absolute runtime artifact from DependencyDXC")
    endif()
    if(TARGET stage_release)
        message(FATAL_ERROR "Release staging has already been registered")
    endif()

    get_filename_component(linker_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(ENGINE_RELEASE_DUMPBIN NAMES dumpbin HINTS "${linker_directory}" REQUIRED)
    string(SHA256 runtime_context "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${runtime_context}" 0 16 runtime_context)
    set(runtime_report "${PROJECT_SOURCE_DIR}/build/release-work/runtime-${CMAKE_BUILD_TYPE}-${runtime_context}")
    add_custom_command(OUTPUT "${runtime_report}/dxcompiler.json"
        COMMAND "${CMAKE_COMMAND}"
            "-DDXC_RUNTIME_FILE=${dxc_runtime_file}"
            "-DDUMPBIN=${ENGINE_RELEASE_DUMPBIN}"
            "-DREPORT_DIRECTORY=${runtime_report}"
            "-DCONFIGURATION=${CMAKE_BUILD_TYPE}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VerifyDxcRuntime.cmake"
        DEPENDS "${dxc_runtime_file}" "${ENGINE_RELEASE_DUMPBIN}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VerifyDxcRuntime.cmake"
        COMMENT "Verifying the source-built DXC runtime DLL"
        VERBATIM)
    add_custom_target(stage_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${dxc_runtime_file}" "$<TARGET_FILE_DIR:engine_app>/dxcompiler.dll"
        DEPENDS engine_app engine_dxc_build "${dxc_runtime_file}" "${runtime_report}/dxcompiler.json"
        COMMENT "Updating the current configuration's runtime DLL"
        VERBATIM)

    # Ordinary builds prepare only their own runnable bin directory. They never
    # publish into the shared release directory or create a release ZIP.
    add_custom_target(stage_development ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/src/res/development/main.lua"
            "$<TARGET_FILE_DIR:engine_app>/main.lua"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/src/res/development/example"
            "$<TARGET_FILE_DIR:engine_app>/example"
        DEPENDS stage_runtime engine_app_notices
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

    # Never package the user's current project. Official packages require a
    # separate public-release configuration validated by LaunchConfig.cmake.
    if(NOT ENGINE_PUBLIC_RELEASE)
        add_custom_target(stage_release
            COMMAND "${CMAKE_COMMAND}" -E echo
                "stage_release requires the isolated public build: tools/build.ps1 -Configuration Release -PublicRelease -StageRelease"
            COMMAND "${CMAKE_COMMAND}" -E false
            VERBATIM)
        return()
    endif()

    string(TOLOWER "${ENGINE_PRODUCT_NAME}" release_product)
    if(NOT release_product MATCHES "^[a-z0-9][a-z0-9_-]*$")
        message(FATAL_ERROR "The product name must form a safe release directory name")
    endif()
    set(release_name "${release_product}-v${PROJECT_VERSION}-windows-x64")

    # Publishing is explicit and Release-only. Source Python tools and private
    # launch headers are never distributed here.
    add_custom_target(stage_release
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
            "-DENGINE_EXECUTABLE=$<TARGET_FILE:engine_app>"
            "-DDXC_RUNTIME_FILE=${dxc_runtime_file}"
            "-DLEGACY_PACK_FILENAME=${ENGINE_PACK_NAME}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DRELEASE_NAME=${release_name}"
            "-DDESTINATION=${PROJECT_SOURCE_DIR}/releases/${release_name}"
            "-DCONFIGURATION=$<CONFIG>"
            "-DDUMPBIN=${ENGINE_RELEASE_DUMPBIN}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CopyRelease.cmake"
        DEPENDS engine_app engine_dxc_build "${dxc_runtime_file}" stage_runtime engine_app_notices
        COMMENT "Staging the runtime directory, Lua example and documentation; creating the release ZIP"
        VERBATIM)
endfunction()
