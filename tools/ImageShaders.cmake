# Native build tools only. GPU/backend-specific bytecode is selected at this
# boundary; Graphics commands and public APIs use SDL_GPU types exclusively.
if(WIN32)
    file(GLOB dxc_hints "$ENV{WindowsSdkDir}/bin/*/x64")
    list(SORT dxc_hints COMPARE NATURAL ORDER DESCENDING)
    find_program(SOLONGO_DXC dxc HINTS ${dxc_hints} REQUIRED)
    set(image_shader_format SDL_GPU_SHADERFORMAT_DXIL)
    set(image_shader_options)
elseif(APPLE)
    message(FATAL_ERROR "Built-in Image Metal bytecode generation is not implemented yet")
else()
    find_program(SOLONGO_DXC dxc REQUIRED)
    set(image_shader_format SDL_GPU_SHADERFORMAT_SPIRV)
    set(image_shader_options -spirv)
endif()
set(image_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/image")
set(image_shader_source "${PROJECT_SOURCE_DIR}/src/res/shaders")
file(MAKE_DIRECTORY "${image_shader_dir}")
foreach(stage IN ITEMS vert frag)
    if(stage STREQUAL "vert")
        set(profile vs_6_0)
    else()
        set(profile ps_6_0)
    endif()
    add_custom_command(OUTPUT "${image_shader_dir}/${stage}.bin"
        COMMAND "${SOLONGO_DXC}" -T ${profile} -E main -O3 -WX ${image_shader_options}
            -Fo "${image_shader_dir}/${stage}.bin" "${image_shader_source}/image.${stage}.hlsl"
        DEPENDS "${image_shader_source}/image.${stage}.hlsl" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${image_shader_dir}/ImageShaders.h"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${image_shader_dir}/vert.bin" "-DFRAGMENT=${image_shader_dir}/frag.bin"
        "-DOUTPUT=${image_shader_dir}/ImageShaders.h" "-DFORMAT=${image_shader_format}"
        -P "${PROJECT_SOURCE_DIR}/tools/EmbedShaders.cmake"
    DEPENDS "${image_shader_dir}/vert.bin" "${image_shader_dir}/frag.bin" "${PROJECT_SOURCE_DIR}/tools/EmbedShaders.cmake"
    VERBATIM)
