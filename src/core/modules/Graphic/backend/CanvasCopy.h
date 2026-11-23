#pragma once
#include <SDL3/SDL_gpu.h>
#include <memory>

namespace core::modules::Graphic::backend {
// Copies the overlapping D32 pixels without resizing/filtering. The caller
// initializes the destination to depth 1 and owns attachment/fence lifetime.
// Construct before publishing a resized Canvas; Encode creates no GPU objects.
class DepthCopy {
public:
    explicit DepthCopy(SDL_GPUDevice*);
    ~DepthCopy();
    DepthCopy(const DepthCopy&) = delete;
    DepthCopy& operator=(const DepthCopy&) = delete;
    void Encode(SDL_GPUCommandBuffer*, SDL_GPUTexture* source, SDL_GPUTexture* destination,
        Uint32 overlapWidth, Uint32 overlapHeight);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
