if(NOT MSVC)
    message(FATAL_ERROR "The current LuaJIT build requires MSVC")
endif()
set(task_luajit_source "${CMAKE_CURRENT_LIST_DIR}/../../LuaJIT")
set(task_luajit_work "${CMAKE_CURRENT_BINARY_DIR}/source")
set(task_luajit_arguments static)
set(task_luajit_crt /MT)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(PREPEND task_luajit_arguments debug)
    set(task_luajit_crt /MTd)
endif()
file(MAKE_DIRECTORY "${task_luajit_work}/src")
file(GLOB_RECURSE task_luajit_inputs CONFIGURE_DEPENDS
    "${task_luajit_source}/src/*.c" "${task_luajit_source}/src/*.h" "${task_luajit_source}/src/*.hpp"
    "${task_luajit_source}/src/*.lua" "${task_luajit_source}/dynasm/*")
add_custom_command(
    OUTPUT "${task_luajit_work}/src/lua51.lib"
    BYPRODUCTS "${task_luajit_work}/src/luajit.h"
        "${task_luajit_work}/src/luajit_relver.txt" "${task_luajit_work}/src/jit/vmdef.lua"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${task_luajit_source}" "${task_luajit_work}"
    COMMAND "${CMAKE_COMMAND}" -E env "CL=${task_luajit_crt}" cmd /c msvcbuild.bat ${task_luajit_arguments}
    WORKING_DIRECTORY "${task_luajit_work}/src"
    DEPENDS ${task_luajit_inputs} "${task_luajit_source}/src/msvcbuild.bat" "${task_luajit_source}/.relver"
    COMMENT "Build vendored LuaJIT in the build tree"
    VERBATIM)
add_custom_target(DependencyLuaJITBuild DEPENDS "${task_luajit_work}/src/lua51.lib")
add_library(DependencyLuaJIT STATIC IMPORTED GLOBAL)
set_target_properties(DependencyLuaJIT PROPERTIES
    IMPORTED_LOCATION "${task_luajit_work}/src/lua51.lib"
    # lua.hpp includes luajit.h, which msvcbuild generates in this build copy.
    # Never rely on an ignored generated header left in the source checkout.
    INTERFACE_INCLUDE_DIRECTORIES "${task_luajit_work}/src")
add_dependencies(DependencyLuaJIT DependencyLuaJITBuild)
add_library(LuaJIT::LuaJIT ALIAS DependencyLuaJIT)
