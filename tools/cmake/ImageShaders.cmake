# Native build tools only. GPU/backend-specific bytecode is selected at this
# boundary; Graphics commands and public APIs use SDL_GPU types exclusively.
# Migrate the former host-tool key once; it is unrelated to runtime DXC builds.
if(DEFINED SOLONGO_DXC)
    if(DEFINED ENGINE_DXC_EXECUTABLE)
        message(WARNING "SOLONGO_DXC is deprecated and ignored because ENGINE_DXC_EXECUTABLE is set; removing the old key.")
    elseif(NOT "${SOLONGO_DXC}" STREQUAL "" AND NOT "${SOLONGO_DXC}" MATCHES "(^|-)NOTFOUND$"
            AND EXISTS "${SOLONGO_DXC}" AND NOT IS_DIRECTORY "${SOLONGO_DXC}")
        set(ENGINE_DXC_EXECUTABLE "${SOLONGO_DXC}" CACHE FILEPATH "Host DXC shader compiler")
        message(WARNING "SOLONGO_DXC is deprecated; migrated its value to ENGINE_DXC_EXECUTABLE and removed the old key.")
    else()
        message(WARNING "SOLONGO_DXC is deprecated and has no usable file; removing it and discovering ENGINE_DXC_EXECUTABLE.")
    endif()
    unset(SOLONGO_DXC CACHE)
    unset(SOLONGO_DXC)
endif()
if(DEFINED ENGINE_DXC_EXECUTABLE)
    if("${ENGINE_DXC_EXECUTABLE}" STREQUAL "" OR "${ENGINE_DXC_EXECUTABLE}" MATCHES "(^|-)NOTFOUND$")
        unset(ENGINE_DXC_EXECUTABLE CACHE)
        unset(ENGINE_DXC_EXECUTABLE)
    elseif(NOT EXISTS "${ENGINE_DXC_EXECUTABLE}" OR IS_DIRECTORY "${ENGINE_DXC_EXECUTABLE}")
        message(FATAL_ERROR "ENGINE_DXC_EXECUTABLE must name an existing compiler file: ${ENGINE_DXC_EXECUTABLE}")
    endif()
endif()
if(WIN32)
    file(GLOB dxc_hints "$ENV{WindowsSdkDir}/bin/*/x64")
    list(SORT dxc_hints COMPARE NATURAL ORDER DESCENDING)
    find_program(ENGINE_DXC_EXECUTABLE dxc HINTS ${dxc_hints} REQUIRED)
    set(image_shader_format SDL_GPU_SHADERFORMAT_DXIL)
    set(image_shader_options)
elseif(APPLE)
    message(FATAL_ERROR "Built-in Image Metal bytecode generation is not implemented yet")
else()
    find_program(ENGINE_DXC_EXECUTABLE dxc REQUIRED)
    set(image_shader_format SDL_GPU_SHADERFORMAT_SPIRV)
    set(image_shader_options -spirv)
endif()
set(ENGINE_DXC_EXECUTABLE "${ENGINE_DXC_EXECUTABLE}" CACHE FILEPATH "Host DXC shader compiler")
set_property(CACHE ENGINE_DXC_EXECUTABLE PROPERTY TYPE FILEPATH)
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
        COMMAND "${ENGINE_DXC_EXECUTABLE}" -T ${profile} -E main -O3 -WX ${image_shader_options}
            -Fo "${image_shader_dir}/${stage}.bin" "${image_shader_source}/image.${stage}.hlsl"
        DEPENDS "${image_shader_source}/image.${stage}.hlsl" "${image_shader_source}/image_sampling.hlsli" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${image_shader_dir}/ImageShaders.h"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${image_shader_dir}/vert.bin" "-DFRAGMENT=${image_shader_dir}/frag.bin"
        "-DOUTPUT=${image_shader_dir}/ImageShaders.h" "-DFORMAT=${image_shader_format}"
        -P "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    DEPENDS "${image_shader_dir}/vert.bin" "${image_shader_dir}/frag.bin" "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    VERBATIM)
add_custom_command(OUTPUT "${image_shader_dir}/sprite.vert.bin"
    COMMAND "${ENGINE_DXC_EXECUTABLE}" -T vs_6_0 -E main -O3 -WX ${image_shader_options}
        -Fo "${image_shader_dir}/sprite.vert.bin" "${image_shader_source}/image_sprite.vert.hlsl"
    DEPENDS "${image_shader_source}/image_sprite.vert.hlsl" VERBATIM)
add_custom_command(OUTPUT "${image_shader_dir}/SpriteShaders.h"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX=${image_shader_dir}/sprite.vert.bin" "-DFRAGMENT=${image_shader_dir}/frag.bin"
        "-DOUTPUT=${image_shader_dir}/SpriteShaders.h" "-DFORMAT=${image_shader_format}" "-DNAMESPACE=sprite_shader_bytecode"
        -P "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    DEPENDS "${image_shader_dir}/sprite.vert.bin" "${image_shader_dir}/frag.bin" "${PROJECT_SOURCE_DIR}/tools/cmake/EmbedShaders.cmake"
    VERBATIM)
