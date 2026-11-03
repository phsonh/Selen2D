#pragma once
#include <SDL3/SDL_gpu.h>

namespace core::modules::Graphic::backend {
    struct DeviceOptions {
        const char* driver{};
        SDL_GPUShaderFormat shader_formats{};
        bool debug{};
    };
    // Platform selection stays below the portable Graphic module. An explicitly
    // requested driver is never retried with a different API after failure.
    DeviceOptions DefaultDeviceOptions();
    SDL_GPUDevice* CreateDevice(const DeviceOptions& options);
}
