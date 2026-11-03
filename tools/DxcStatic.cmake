include_guard(GLOBAL)

function(engine_add_static_dxc)
    if(TARGET DependencyDXC)
        return()
    endif()
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The static runtime shader compiler requires Windows MSVC x64")
    endif()
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja" OR NOT CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "The static runtime shader compiler requires a single-configuration Ninja build")
    endif()
    get_filename_component(project_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(source "${project_root}/3rd_party/dxc/source")
    set(integration "${project_root}/3rd_party/cmake/dxc")
    if(NOT EXISTS "${source}/LICENSE.TXT" OR NOT EXISTS "${source}/tools/clang/tools/dxcompiler/DXCompiler.cpp")
        message(FATAL_ERROR "Pinned DXC source is missing; builds never download runtime dependencies")
    endif()
    set(ENGINE_DXC_BINARY_DIR "${CMAKE_BINARY_DIR}/dependencies/dxc" CACHE PATH "Isolated static DXC build directory for this configuration")
    set(bundle "${ENGINE_DXC_BINARY_DIR}/static/dxc-static.lib")
    include(ExternalProject)
    ExternalProject_Add(engine_dxc_build
        SOURCE_DIR "${source}"
        BINARY_DIR "${ENGINE_DXC_BINARY_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${ENGINE_DXC_BINARY_DIR}"
            -G Ninja -C "${integration}/Options.cmake"
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -DENGINE_DXC_BUILD_PROBE=OFF
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "${ENGINE_DXC_BINARY_DIR}" --target engine_dxc_bundle --parallel 4
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS "${bundle}"
        INSTALL_COMMAND ""
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE)
    add_library(DependencyDXC STATIC IMPORTED GLOBAL)
    set_target_properties(DependencyDXC PROPERTIES
        IMPORTED_LOCATION "${bundle}"
        INTERFACE_INCLUDE_DIRECTORIES "${source}/include;${integration}"
        INTERFACE_COMPILE_DEFINITIONS "DXC_API_IMPORT="
        INTERFACE_LINK_LIBRARIES "ole32;oleaut32;advapi32;uuid;user32;shell32;shlwapi;psapi")
    add_dependencies(DependencyDXC engine_dxc_build)
endfunction()
