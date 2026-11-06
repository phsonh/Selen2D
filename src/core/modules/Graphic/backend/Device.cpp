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
    SDL_GPUDevice* CreateDevice(const DeviceOptions& options, std::string* error) {
        if(error) error->clear();
        const std::string requested = options.driver ? options.driver : "SDL automatic selection";
        SDL_GPUDevice* device = SDL_CreateGPUDevice(options.shader_formats, options.debug, options.driver);
        if (!device) {
            const std::string message = "SDL_CreateGPUDevice failed for requested driver '" + requested +
                                       "': " + SDL_GetError() + "; no fallback attempted";
            if(error) *error=message;
            else core::runtime::Log::Error(message);
            return nullptr;
        }
        const char* actual = SDL_GetGPUDeviceDriver(device);
        if (!actual || (options.driver && std::string_view(actual) != options.driver)) {
            const std::string message="SDL_GPU driver mismatch: requested '" + requested + "', actual '" +
                                      (actual ? actual : "unknown") + "'; refusing implicit fallback";
            if(error) *error=message;
            else core::runtime::Log::Error(message);
            SDL_DestroyGPUDevice(device);
            SDL_SetError("SDL_GPU driver does not match the requested backend");
            return nullptr;
        }
        core::runtime::Log::Info("SDL_GPU initialized: requested='" + requested + "', actual='" + actual + "'");
        return device;
    }
}
