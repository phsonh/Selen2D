add_library(engine_collision STATIC core/modules/Object/Collision/Collision.cpp)
target_include_directories(engine_collision PUBLIC .)

add_library(engine_object STATIC
    core/modules/Object/Object.cpp
    core/modules/Object/Motion.cpp
    core/modules/Object/ObjectSystem.cpp
    core/modules/Object/ObjectPool.cpp)
target_include_directories(engine_object PUBLIC .)
target_link_libraries(engine_object PUBLIC engine_collision)

add_library(engine_input_state STATIC core/modules/Input/InputState.cpp core/modules/Input/InputNames.cpp)
target_include_directories(engine_input_state PUBLIC .)
target_link_libraries(engine_input_state PUBLIC SDL3::SDL3-static)

add_library(engine_text STATIC core/modules/Text/Text.cpp)
target_include_directories(engine_text PUBLIC .)
target_link_libraries(engine_text PUBLIC DependencyJsonHeaders)

# Logging has no Engine Host dependency; standalone GPU services can report
# failures without pulling engine_core (and creating a Renderer/Core cycle).
add_library(engine_log STATIC core/runtime/Log/Log.cpp core/runtime/Log/Console.cpp)
target_include_directories(engine_log PUBLIC .)

add_library(engine_file STATIC core/modules/File/File.cpp core/modules/File/Vfs.cpp
    core/modules/File/Path.cpp core/modules/File/NativeIo.cpp core/modules/File/ReadSource.cpp
    core/modules/File/Package.cpp core/modules/File/PackageCrypto.cpp)
target_include_directories(engine_file PUBLIC .)
target_link_libraries(engine_file PUBLIC SDL3::SDL3-static DependencyZstd DependencyUtf8proc DependencySodium)

add_library(engine_lua_scripts STATIC core/runtime/Lua/VfsScripts.cpp
    core/runtime/Lua/TrustedScripts.cpp core/runtime/Lua/VfsIo.cpp)
target_include_directories(engine_lua_scripts PUBLIC .)
target_link_libraries(engine_lua_scripts PUBLIC engine_file LuaJIT::LuaJIT)

# Resource coordination stays independent of Lua, Object, GPU and Audio.
# Concrete asynchronous providers are wired in subsequent implementation stages.
add_library(engine_resource STATIC core/modules/Resource/Resource.cpp)
target_include_directories(engine_resource PUBLIC .)

add_library(engine_jobs STATIC core/runtime/Jobs/Executor.cpp)
target_include_directories(engine_jobs PUBLIC .)
target_link_libraries(engine_jobs PUBLIC Threads::Threads)

# CPU image preparation has no Object, Lua, SDL_GPU device or Renderer dependency.
add_library(engine_png STATIC core/modules/Graphic/image/PngDecode.cpp)
target_include_directories(engine_png PUBLIC . PRIVATE ../3rd_party/stb_image)
add_library(engine_atlas STATIC core/modules/Graphic/image/Atlas.cpp core/modules/Graphic/image/MipChain.cpp)
target_include_directories(engine_atlas PUBLIC .)
add_library(engine_upload STATIC core/modules/Graphic/upload/Upload.cpp core/modules/Graphic/upload/SdlTransfer.cpp)
target_include_directories(engine_upload PUBLIC .)
target_link_libraries(engine_upload PUBLIC SDL3::SDL3-static)
include("${PROJECT_SOURCE_DIR}/tools/cmake/ImageShaders.cmake")
include("${PROJECT_SOURCE_DIR}/tools/cmake/CanvasViewShaders.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/image_sampling.hlsli")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/image_sampling.hlsli" ENGINE_IMAGE_SAMPLING_SOURCE)
configure_file(core/modules/Graphic/shader/ImageSamplingSource.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ImageSamplingSource.h" @ONLY)
add_library(engine_shader_compiler STATIC core/modules/Graphic/shader/Model.cpp core/modules/Graphic/shader/Compiler.cpp
    core/modules/Graphic/shader/Sampling.cpp)
target_include_directories(engine_shader_compiler PUBLIC . PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(engine_shader_compiler PUBLIC engine_file DependencyDXC)
add_library(engine_shader_programs STATIC core/modules/Graphic/shader/Program.cpp)
target_include_directories(engine_shader_programs PUBLIC .)
target_link_libraries(engine_shader_programs PUBLIC engine_shader_compiler SDL3::SDL3-static)
add_library(engine_shader_provider STATIC core/modules/Resource/ShaderProvider.cpp)
target_include_directories(engine_shader_provider PUBLIC .)
target_link_libraries(engine_shader_provider PUBLIC engine_resource engine_jobs engine_shader_programs)
add_library(engine_geometry STATIC core/modules/Graphic/geometry/Quad.cpp core/modules/Graphic/geometry/Ribbon.cpp
    core/modules/Graphic/geometry/Spatial.cpp core/modules/Graphic/geometry/Mesh.cpp)
target_include_directories(engine_geometry PUBLIC .)
add_library(engine_render_values STATIC core/modules/Graphic/render/Image.cpp)
target_include_directories(engine_render_values PUBLIC .)
target_link_libraries(engine_render_values PUBLIC SDL3::SDL3-static)
add_library(engine_shader_views STATIC
    "${canvas_view_shader_dir}/CanvasViewShaders.h"
    core/modules/Graphic/shader/ImageViews.cpp
    core/modules/Graphic/shader/CanvasViews.cpp
    core/modules/Graphic/shader/MeshBuffers.cpp)
target_include_directories(engine_shader_views PUBLIC . PRIVATE "${canvas_view_shader_dir}")
target_link_libraries(engine_shader_views PUBLIC engine_render_values engine_geometry)
add_library(engine_shader_builtins STATIC core/modules/Graphic/shader/Builtins.cpp)
target_include_directories(engine_shader_builtins PUBLIC .)
target_link_libraries(engine_shader_builtins PUBLIC engine_geometry engine_shader_compiler)
add_library(engine_graphic_values STATIC lua_bind/modules/Graphic/Mesh.cpp lua_bind/modules/Graphic/ShaderParams.cpp)
target_include_directories(engine_graphic_values PUBLIC .)
target_link_libraries(engine_graphic_values PUBLIC engine_geometry engine_shader_compiler LuaJIT::LuaJIT)
add_library(engine_readback STATIC core/modules/Graphic/readback/Readback.cpp)
target_include_directories(engine_readback PUBLIC .)
target_link_libraries(engine_readback PUBLIC SDL3::SDL3-static)
add_library(engine_renderer STATIC
    "${image_shader_dir}/ImageShaders.h"
    "${image_shader_dir}/SpriteShaders.h"
    core/modules/Graphic/backend/ImagePipeline.cpp
    core/modules/Graphic/backend/ShaderPipeline.cpp
    core/modules/Graphic/canvas/Canvas.cpp
    core/modules/Graphic/render/GlyphTextures.cpp
    core/modules/Graphic/render/ShaderCommands.cpp
    core/modules/Graphic/render/Renderer.cpp)
target_include_directories(engine_renderer PUBLIC . PRIVATE "${image_shader_dir}")
target_link_libraries(engine_renderer PUBLIC SDL3::SDL3-static engine_geometry engine_render_values engine_shader_views engine_shader_programs engine_shader_builtins engine_readback engine_log engine_text_layout)
add_library(engine_image_sources STATIC core/modules/Resource/ImageSources.cpp)
target_include_directories(engine_image_sources PUBLIC .)
target_link_libraries(engine_image_sources PUBLIC engine_jobs engine_png engine_file)

# Bridge only: neither the pure registry nor Graphic depends on the other.
add_library(engine_image_provider STATIC core/modules/Resource/ImageProvider.cpp)
target_include_directories(engine_image_provider PUBLIC .)
target_link_libraries(engine_image_provider PUBLIC engine_resource engine_image_sources engine_atlas engine_upload)

add_subdirectory(core/modules/Audio)

add_library(engine_font STATIC core/modules/Resource/Font.cpp)
target_include_directories(engine_font PUBLIC .)
target_link_libraries(engine_font PUBLIC engine_resource engine_file engine_jobs PRIVATE DependencyFreeType)
add_library(engine_text_layout STATIC core/modules/Graphic/text/Text.cpp core/modules/Graphic/text/FontFace.cpp
    core/modules/Graphic/text/Unicode.cpp)
target_include_directories(engine_text_layout PUBLIC .)
target_link_libraries(engine_text_layout PUBLIC engine_font PRIVATE DependencyFreeType DependencyHarfBuzz DependencySheenBidi DependencyUnibreak)
add_library(engine_cpu_provider STATIC core/modules/Resource/CpuProvider.cpp)
target_include_directories(engine_cpu_provider PUBLIC .)
target_link_libraries(engine_cpu_provider PUBLIC engine_resource engine_file engine_jobs engine_audio engine_font)

add_library(engine_clock STATIC core/runtime/Clock/Clock.cpp)
target_include_directories(engine_clock PUBLIC .)
add_library(engine_time STATIC core/modules/Time/Time.cpp)
target_include_directories(engine_time PUBLIC .)

add_library(engine_core STATIC
    engine/Engine.cpp
    engine/ResourceHost.cpp
    core/runtime/SDL/SDL.cpp
    core/runtime/Event/Event.cpp
    core/modules/Input/Input.cpp
    core/modules/Input/Gamepad.cpp
    core/runtime/Lua/Lua.cpp
    core/modules/Window/Window.cpp
    core/modules/Graphic/Graphic.cpp
    core/modules/Graphic/backend/Device.cpp
    core/modules/Math/Math.cpp
    core/modules/Debug/Debug.cpp
    lua_bind/modules/Window/Window.cpp
    lua_bind/runtime/Runtime.cpp
    lua_bind/modules/Time/Time.cpp
    lua_bind/modules/Input/Input.cpp
    lua_bind/modules/Math/Math.cpp
    lua_bind/modules/File/File.cpp
    lua_bind/modules/Text/Text.cpp
    lua_bind/modules/Audio/Audio.cpp
    lua_bind/modules/Object/Object.cpp
    lua_bind/modules/Resource/Resource.cpp
    lua_bind/modules/Graphic/Graphic.cpp
    lua_bind/modules/Graphic/TextBinding.cpp
    lua_bind/modules/Debug/Debug.cpp)
target_include_directories(engine_core PUBLIC . PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(engine_core PUBLIC SDL3::SDL3-static LuaJIT::LuaJIT engine_object engine_input_state engine_file engine_lua_scripts engine_text engine_audio engine_image_provider engine_cpu_provider engine_shader_provider engine_renderer engine_graphic_values engine_clock engine_time engine_log)

option(ENGINE_SPRITE_PROFILE "Enable internal sprite benchmark CSV instrumentation" OFF)
if(ENGINE_SPRITE_PROFILE)
    target_compile_definitions(engine_renderer PRIVATE ENGINE_SPRITE_PROFILE=1)
    target_compile_definitions(engine_core PRIVATE ENGINE_SPRITE_PROFILE=1)
endif()

set(ENGINE_EXECUTABLE "${ENGINE_RUNTIME_NAME}${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(res/windows/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/engine-version.rc" @ONLY)
set(ENGINE_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/windows/selen2d-logo.ico")
configure_file(res/windows/app-icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app-icon.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/app-icon.rc"
    PROPERTIES OBJECT_DEPENDS "${ENGINE_ICON_FILE}")
add_executable(engine_app main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/engine-version.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app-icon.rc")
if(WIN32)
    set_target_properties(engine_app PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_include_directories(engine_app PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_library(engine_startup STATIC engine/Startup.cpp)
include("${PROJECT_SOURCE_DIR}/tools/cmake/LaunchConfig.cmake")
engine_configure_launch(engine_startup)
target_include_directories(engine_startup PUBLIC . PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(engine_startup PUBLIC engine_file)
if(WIN32)
    target_sources(engine_startup PRIVATE engine/UserDataRoot.cpp)
    target_link_libraries(engine_startup PRIVATE shell32 ole32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(engine_startup PRIVATE engine/UserDataRoot.mm)
    target_link_libraries(engine_startup PRIVATE "-framework Foundation")
else()
    target_sources(engine_startup PRIVATE engine/UserDataRoot.cpp)
endif()
target_link_libraries(engine_app PRIVATE engine_core engine_startup)
set_target_properties(engine_app PROPERTIES
    OUTPUT_NAME "${ENGINE_RUNTIME_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include("${PROJECT_SOURCE_DIR}/tools/cmake/DeployNotices.cmake")
engine_deploy_notices(engine_app)
if(MSVC)
    target_compile_options(engine_startup PRIVATE /W4 /utf-8)
    target_compile_options(engine_shader_compiler PRIVATE /W4 /utf-8)
    target_compile_options(engine_shader_programs PRIVATE /W4 /utf-8)
    target_compile_options(engine_shader_provider PRIVATE /W4 /utf-8)
    set_source_files_properties(core/modules/Math/Math.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
    target_compile_options(engine_text PRIVATE /W4 /utf-8)
    target_compile_options(engine_log PRIVATE /W4 /utf-8)
    target_compile_options(engine_readback PRIVATE /W4 /utf-8)
    target_compile_options(engine_clock PRIVATE /W4 /utf-8)
    target_compile_options(engine_time PRIVATE /W4 /utf-8)
    target_compile_options(engine_core PRIVATE /W4 /utf-8)
    target_compile_options(engine_object PRIVATE /W4 /utf-8)
    target_compile_options(engine_collision PRIVATE /W4 /utf-8)
    target_compile_options(engine_input_state PRIVATE /W4 /utf-8)
    target_compile_options(engine_file PRIVATE /W4 /utf-8)
    target_compile_options(engine_resource PRIVATE /W4 /utf-8)
    target_compile_options(engine_font PRIVATE /W4 /utf-8)
    target_compile_options(engine_text_layout PRIVATE /W4 /utf-8)
    target_compile_options(engine_cpu_provider PRIVATE /W4 /utf-8)
    target_compile_options(engine_jobs PRIVATE /W4 /utf-8)
    target_compile_options(engine_png PRIVATE /W4 /utf-8)
    target_compile_options(engine_atlas PRIVATE /W4 /utf-8)
    target_compile_options(engine_upload PRIVATE /W4 /utf-8)
    target_compile_options(engine_renderer PRIVATE /W4 /utf-8)
    target_compile_options(engine_geometry PRIVATE /W4 /utf-8)
    target_compile_options(engine_render_values PRIVATE /W4 /utf-8)
    target_compile_options(engine_shader_views PRIVATE /W4 /utf-8)
    target_compile_options(engine_shader_builtins PRIVATE /W4 /utf-8)
    target_compile_options(engine_graphic_values PRIVATE /W4 /utf-8)
    target_compile_options(engine_image_sources PRIVATE /W4 /utf-8)
    target_compile_options(engine_image_provider PRIVATE /W4 /utf-8)
    target_compile_options(engine_app PRIVATE /W4 /utf-8)
else()
    set_source_files_properties(core/modules/Math/Math.cpp PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()
