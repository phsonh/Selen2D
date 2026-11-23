# Private depth preservation pass. Reuses the built-in shader host compiler;
# Canvas resize never invokes the runtime DXC compiler.
set(depth_copy_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/depth_copy")
file(MAKE_DIRECTORY "${depth_copy_shader_dir}")
foreach(stage IN ITEMS vert frag)
    if(stage STREQUAL "vert")
        set(profile vs_6_0)
    else()
        set(profile ps_6_0)
    endif()
    add_custom_command(OUTPUT "${depth_copy_shader_dir}/${stage}.bin"
        COMMAND "${ENGINE_DXC_EXECUTABLE}" -T ${profile} -E main -O3 -WX ${image_shader_options}
            -Fo "${depth_copy_shader_dir}/${stage}.bin" "${image_shader_source}/depth_copy.${stage}.hlsl"
        DEPENDS "${image_shader_source}/depth_copy.${stage}.hlsl" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${depth_copy_shader_dir}/DepthCopyShaders.h"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${depth_copy_shader_dir}/vert.bin" "-DFRAGMENT=${depth_copy_shader_dir}/frag.bin"
        "-DOUTPUT=${depth_copy_shader_dir}/DepthCopyShaders.h" "-DFORMAT=${image_shader_format}"
        -DNAMESPACE=depth_copy_shader_bytecode -P "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    DEPENDS "${depth_copy_shader_dir}/vert.bin" "${depth_copy_shader_dir}/frag.bin" "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    VERBATIM)
