#include "Device.h"
#include "core/runtime/Log/Log.h"
#include <SDL3/SDL_error.h>
#include <string>
#include <string_view>

namespace core::modules::Graphic::backend {
    DeviceOptions DefaultDeviceOptions() {
        DeviceOptions options;
        #if defined(_WIN32)
        options.driver = "direct3d12";
        options.shader_formats = SDL_GPU_SHADERFORMAT_DXIL;
        #elif defined(__APPLE__)
        options.driver = "metal";
        options.shader_formats = SDL_GPU_SHADERFORMAT_METALLIB;
        #else
        options.driver = "vulkan";
        options.shader_formats = SDL_GPU_SHADERFORMAT_SPIRV;
        #endif
        #ifndef NDEBUG
        options.debug = true;
        #endif
        return options;
    }
    SDL_GPUDevice* CreateDevice(const DeviceOptions& options) {
        const std::string requested = options.driver ? options.driver : "SDL automatic selection";
        SDL_GPUDevice* device = SDL_CreateGPUDevice(options.shader_formats, options.debug, options.driver);
        if (!device) {
            const std::string error = SDL_GetError();
            core::runtime::Log::Error("SDL_GPU initialization failed for requested driver '" + requested +
                                     "': " + error + "; no fallback attempted");
            return nullptr;
        }
        const char* actual = SDL_GetGPUDeviceDriver(device);
        if (!actual || (options.driver && std::string_view(actual) != options.driver)) {
            core::runtime::Log::Error("SDL_GPU driver mismatch: requested '" + requested + "', actual '" +
                                     (actual ? actual : "unknown") + "'; refusing implicit fallback");
            SDL_DestroyGPUDevice(device);
            SDL_SetError("SDL_GPU driver does not match the requested backend");
            return nullptr;
        }
        core::runtime::Log::Info("SDL_GPU initialized: requested='" + requested + "', actual='" + actual + "'");
        return device;
    }
}
