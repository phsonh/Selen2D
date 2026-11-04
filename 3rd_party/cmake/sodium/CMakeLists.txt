# libsodium 1.0.22, unchanged official source, ISC license. Use the same C source
# list and static definitions as upstream's MSVC project; no DLL or Python step.
if(NOT MSVC OR NOT WIN32)
    message(FATAL_ERROR "This libsodium wrapper currently supports the engine's Windows/MSVC toolchain")
endif()
set(dependency_sodium_root "${CMAKE_CURRENT_LIST_DIR}/../../libsodium")
file(STRINGS "${dependency_sodium_root}/builds/msvc/vs2022/libsodium/libsodium.vcxproj"
    dependency_sodium_project_sources REGEX "<ClCompile Include=")
set(dependency_sodium_sources)
foreach(dependency_sodium_line IN LISTS dependency_sodium_project_sources)
    string(REGEX REPLACE ".*Include=\"([^\"]+)\".*" "\\1" dependency_sodium_relative "${dependency_sodium_line}")
    string(REPLACE "\\" "/" dependency_sodium_relative "${dependency_sodium_relative}")
    string(REGEX REPLACE "^(\.\./)+" "" dependency_sodium_relative "${dependency_sodium_relative}")
    list(APPEND dependency_sodium_sources "${dependency_sodium_root}/${dependency_sodium_relative}")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/sodium")
configure_file("${dependency_sodium_root}/builds/msvc/version.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sodium/version.h" COPYONLY)
add_library(DependencySodium STATIC ${dependency_sodium_sources})
target_include_directories(DependencySodium PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include" "${dependency_sodium_root}/src/libsodium/include"
    "${dependency_sodium_root}/src/libsodium/include/sodium"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include/sodium")
target_compile_definitions(DependencySodium PUBLIC SODIUM_STATIC
    PRIVATE NATIVE_LITTLE_ENDIAN inline=__inline _CRT_SECURE_NO_WARNINGS)
target_compile_options(DependencySodium PRIVATE /utf-8 /wd4146 /wd4244)
target_link_libraries(DependencySodium PUBLIC advapi32)
set_target_properties(DependencySodium PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
