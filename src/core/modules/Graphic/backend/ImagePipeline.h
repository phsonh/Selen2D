#pragma once
#include "core/modules/Graphic/render/Image.h"
#include <memory>

namespace core::modules::Graphic::backend {
// The source shader already emits associated/PMA RGB. This describes only
// framebuffer composition; the Mul/Add tint prefix is a shader input.
SDL_GPUColorTargetBlendState MakeBlendState(Render::Blend);
// Fixed Image pipeline cache. SDL_GPU remains the only graphics abstraction;
// compiled binary format selection stays here/build scripts, not in Renderer.
class ImagePipeline {
public:
    explicit ImagePipeline(SDL_GPUDevice*);
    ~ImagePipeline();
    ImagePipeline(const ImagePipeline&)=delete;
    ImagePipeline& operator=(const ImagePipeline&)=delete;
    SDL_GPUGraphicsPipeline* Get(SDL_GPUTextureFormat, Render::Blend,
        SDL_GPUTextureFormat depth = SDL_GPU_TEXTUREFORMAT_INVALID, bool depthEnabled = false);
    SDL_GPUSampler* Sampler(Render::Filter, Render::Address);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
