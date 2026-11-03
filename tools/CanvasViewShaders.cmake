# Private PMA-to-Straight material view. Shares the build-time compiler/backend
# selection with Image shaders; no runtime file or native D3D API dependency.
set(canvas_view_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/canvas_view")
file(MAKE_DIRECTORY "${canvas_view_shader_dir}")
foreach(stage IN ITEMS vert frag)
    if(stage STREQUAL "vert")
        set(profile vs_6_0)
    else()
        set(profile ps_6_0)
    endif()
    add_custom_command(OUTPUT "${canvas_view_shader_dir}/${stage}.bin"
        COMMAND "${SOLONGO_DXC}" -T ${profile} -E main -O3 -WX ${image_shader_options}
            -Fo "${canvas_view_shader_dir}/${stage}.bin" "${image_shader_source}/canvas_view.${stage}.hlsl"
        DEPENDS "${image_shader_source}/canvas_view.${stage}.hlsl" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${canvas_view_shader_dir}/CanvasViewShaders.h"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${canvas_view_shader_dir}/vert.bin" "-DFRAGMENT=${canvas_view_shader_dir}/frag.bin"
        "-DOUTPUT=${canvas_view_shader_dir}/CanvasViewShaders.h" "-DFORMAT=${image_shader_format}"
        -DNAMESPACE=canvas_view_shader_bytecode -P "${PROJECT_SOURCE_DIR}/tools/EmbedShaders.cmake"
    DEPENDS "${canvas_view_shader_dir}/vert.bin" "${canvas_view_shader_dir}/frag.bin" "${PROJECT_SOURCE_DIR}/tools/EmbedShaders.cmake"
    VERBATIM)
