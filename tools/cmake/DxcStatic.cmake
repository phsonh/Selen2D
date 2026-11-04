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
    get_filename_component(project_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(source "${project_root}/3rd_party/dxc/source")
    set(integration "${project_root}/3rd_party/cmake/dxc")
    if(NOT EXISTS "${source}/LICENSE.TXT" OR NOT EXISTS "${source}/tools/clang/tools/dxcompiler/DXCompiler.cpp")
        message(FATAL_ERROR "Pinned DXC source is missing; builds never download runtime dependencies")
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" dxc_python)
    set(ENGINE_DXC_BINARY_DIR "${CMAKE_BINARY_DIR}/dependencies/dxc" CACHE PATH "Isolated static DXC build directory for this configuration")
    set(ENGINE_DXC_JOBS 4 CACHE STRING "DXC build parallelism (1-4; bounded for LLVM memory use)")
    if(NOT ENGINE_DXC_JOBS MATCHES "^[1-4]$")
        message(FATAL_ERROR "ENGINE_DXC_JOBS must be an integer from 1 to 4")
    endif()
    set(bundle "${ENGINE_DXC_BINARY_DIR}/static/dxc-static.lib")
    set(state "${ENGINE_DXC_BINARY_DIR}/static/bundle-state.json")
    set(health "${ENGINE_DXC_BINARY_DIR}/static/bundle-health.stamp")
    set(stamp "${ENGINE_DXC_BINARY_DIR}/static/build.stamp")
    set(helper "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DxcBuild.py")
    # Ninja tracks the pinned source tree, including additions/removals. No
    # recursive content hashing and no sub-Ninja invocation on ordinary builds.
    file(GLOB_RECURSE dxc_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${source}/*" "${integration}/*")
    list(FILTER dxc_inputs EXCLUDE REGEX "/__pycache__/|\\.pyc$")
    set(source_manifest "${CMAKE_CURRENT_BINARY_DIR}/DxcSources.txt")
    file(GENERATE OUTPUT "${source_manifest}" CONTENT "${dxc_inputs}\n")
    set(toolchain_inputs "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}" "${CMAKE_AR}" "${CMAKE_LINKER}")
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # Integrate.cmake compiles this installed ATL source into the archive.
    get_filename_component(atl_source "${compiler_dir}/../../../atlmfc/src/atl/atls/atlbase.cpp" ABSOLUTE)
    if(EXISTS "${atl_source}")
        list(APPEND dxc_inputs "${atl_source}")
    endif()
    file(GLOB compiler_parts CONFIGURE_DEPENDS "${compiler_dir}/*.dll")
    list(APPEND toolchain_inputs ${compiler_parts})
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND toolchain_inputs "${CMAKE_TOOLCHAIN_FILE}")
    endif()
    list(REMOVE_DUPLICATES toolchain_inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${toolchain_inputs})
    set(context "${CMAKE_BUILD_TYPE}|${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SYSTEM_VERSION}|${CMAKE_MSVC_RUNTIME_LIBRARY}")
    foreach(input IN LISTS toolchain_inputs)
        if(EXISTS "${input}")
            file(SIZE "${input}" input_size)
            file(TIMESTAMP "${input}" input_time "%Y-%m-%dT%H:%M:%S.%fZ" UTC)
            string(APPEND context "|${input}:${input_size}:${input_time}")
        endif()
    endforeach()
    string(SHA256 context "${context}")
    set(build_script "${CMAKE_CURRENT_BINARY_DIR}/DxcBuild.cmake")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DxcBuild.cmake.in" "${build_script}" @ONLY)
    # Run only a cheap archive check each time. Ninja restat suppresses dependent
    # work when the health stamp is unchanged. Hash only a changed archive.
    add_custom_target(engine_dxc_health
        COMMAND "${dxc_python}" -B "${helper}" check "${bundle}" "${state}" "${health}"
        BYPRODUCTS "${health}"
        VERBATIM COMMENT "Checking static DXC archive metadata")
    add_custom_command(OUTPUT "${bundle}" "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -P "${build_script}"
        DEPENDS "${health}" "${build_script}" "${helper}" "${source_manifest}"
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" ${dxc_inputs} ${toolchain_inputs}
        VERBATIM USES_TERMINAL COMMENT "Updating the pinned static DXC implementation")
    add_custom_target(engine_dxc_build DEPENDS "${bundle}" "${stamp}")
    add_library(DependencyDXC STATIC IMPORTED GLOBAL)
    set_target_properties(DependencyDXC PROPERTIES
        IMPORTED_LOCATION "${bundle}"
        INTERFACE_INCLUDE_DIRECTORIES "${source}/include;${integration}"
        INTERFACE_COMPILE_DEFINITIONS "DXC_API_IMPORT="
        INTERFACE_LINK_LIBRARIES "ole32;oleaut32;advapi32;uuid;user32;shell32;shlwapi;psapi")
    add_dependencies(DependencyDXC engine_dxc_build)
endfunction()
