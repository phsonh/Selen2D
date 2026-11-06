# Public validation tools are opt-in and never part of the runtime package.
add_executable(engine_runtime_check dxc_probe.cpp)
target_link_libraries(engine_runtime_check PRIVATE engine_shader_compiler engine_log)
target_compile_options(engine_runtime_check PRIVATE /W4 /utf-8)
set_target_properties(engine_runtime_check PROPERTIES
    OUTPUT_NAME "dxc-runtime-check"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
