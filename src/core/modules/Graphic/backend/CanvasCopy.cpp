#include "CanvasCopy.h"
#include "DepthCopyShaders.h"
#include <SDL3/SDL_error.h>
#include <limits>
#include <stdexcept>
#include <string>

namespace core::modules::Graphic::backend {
struct DepthCopy::Impl {
    SDL_GPUDevice* device;
    SDL_GPUShader* vertex{};
    SDL_GPUShader* fragment{};
    SDL_GPUGraphicsPipeline* pipeline{};
    SDL_GPUSampler* sampler{};
    explicit Impl(SDL_GPUDevice* value) : device(value) {}
    ~Impl() {
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (fragment) SDL_ReleaseGPUShader(device, fragment);
        if (vertex) SDL_ReleaseGPUShader(device, vertex);
    }
    void Init() {
        if (!device) throw std::invalid_argument("Canvas depth copy requires a GPU device");
        if (!(SDL_GetGPUShaderFormats(device) & depth_copy_shader_bytecode::format))
            throw std::runtime_error("Canvas depth copy bytecode is unavailable for the active SDL_GPU device");
        SDL_GPUShaderCreateInfo shader{};
        shader.entrypoint = "main";
        shader.format = depth_copy_shader_bytecode::format;
        shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        shader.code = depth_copy_shader_bytecode::vertex;
        shader.code_size = sizeof(depth_copy_shader_bytecode::vertex);
        vertex = SDL_CreateGPUShader(device, &shader);
        if (!vertex) throw std::runtime_error(std::string("Canvas depth copy vertex shader failed: ") + SDL_GetError());
        shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        shader.num_samplers = 1;
        shader.code = depth_copy_shader_bytecode::fragment;
        shader.code_size = sizeof(depth_copy_shader_bytecode::fragment);
        fragment = SDL_CreateGPUShader(device, &shader);
        if (!fragment) throw std::runtime_error(std::string("Canvas depth copy fragment shader failed: ") + SDL_GetError());

        SDL_GPUGraphicsPipelineCreateInfo create{};
        create.vertex_shader = vertex;
        create.fragment_shader = fragment;
        create.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        create.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        create.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        create.rasterizer_state.enable_depth_clip = true;
        create.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        create.target_info.has_depth_stencil_target = true;
        create.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        create.depth_stencil_state.enable_depth_test = true;
        create.depth_stencil_state.enable_depth_write = true;
        create.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &create);
        if (!pipeline) throw std::runtime_error(std::string("Canvas depth copy pipeline failed: ") + SDL_GetError());

        // SDL exposes sampled textures through texture/sampler pairs. The
        // fragment shader uses integer Load, so this sampler never filters.
        SDL_GPUSamplerCreateInfo sample{};
        sample.min_filter = sample.mag_filter = SDL_GPU_FILTER_NEAREST;
        sample.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sample.address_mode_u = sample.address_mode_v = sample.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SDL_CreateGPUSampler(device, &sample);
        if (!sampler) throw std::runtime_error(std::string("Canvas depth copy sampler failed: ") + SDL_GetError());
    }
};

DepthCopy::DepthCopy(SDL_GPUDevice* device) : impl(std::make_unique<Impl>(device)) { impl->Init(); }
DepthCopy::~DepthCopy() = default;

void DepthCopy::Encode(SDL_GPUCommandBuffer* command, SDL_GPUTexture* source,
    SDL_GPUTexture* destination, Uint32 width, Uint32 height) {
    if (!command || !source || !destination || source == destination || !width || !height ||
        width > static_cast<Uint32>((std::numeric_limits<int>::max)()) ||
        height > static_cast<Uint32>((std::numeric_limits<int>::max)()))
        throw std::invalid_argument("Canvas depth copy requires distinct attachments and a nonempty bounded overlap");
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = destination;
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = false;
    auto* pass = SDL_BeginGPURenderPass(command, nullptr, 0, &depth);
    if (!pass) throw std::runtime_error(std::string("Canvas depth copy render pass failed: ") + SDL_GetError());
    SDL_BindGPUGraphicsPipeline(pass, impl->pipeline);
    SDL_GPUTextureSamplerBinding binding{source, impl->sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_GPUViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{0, 0, static_cast<int>(width), static_cast<int>(height)};
    SDL_SetGPUScissor(pass, &scissor);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}
}
