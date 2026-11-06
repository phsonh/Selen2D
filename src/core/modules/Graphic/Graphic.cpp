#include "Graphic.h"
#include "backend/Device.h"
#include "upload/Upload.h"
#include "render/Renderer.h"
#include "shader/Program.h"
#include "core/runtime/Log/Log.h"
#include "core/modules/Window/Window.h"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace core::modules::Graphic {
namespace {
bool initialized{};
SDL_GPUPresentMode presentMode{SDL_GPU_PRESENTMODE_VSYNC};
std::optional<SDL_GPUPresentMode> pendingPresentMode;
SDL_Window* window{};
SDL_GPUDevice* device{};
std::unique_ptr<Upload::Queue> uploads;
std::unique_ptr<Render::Renderer> renderer;
std::unique_ptr<Shader::ProgramStore> programs;
void ApplyPresentation(SDL_GPUPresentMode mode) {
    // Publish the actual mode only after SDL accepts the swapchain change.
    if(!SDL_SetGPUSwapchainParameters(device,window,SDL_GPU_SWAPCHAINCOMPOSITION_SDR,mode))
        throw std::runtime_error(std::string("Window.SetVSync failed: ")+SDL_GetError());
    presentMode=mode;
}
}
bool Init(std::string* error) {
    if(error) error->clear();
    if (initialized) return true;
    const auto fail=[error](const std::string& message) {
        if(error) *error=message;
        else core::runtime::Log::Error(message);
    };
    pendingPresentMode.reset();
    window=Window::GetNativeHandle();
    if (!window) { fail("Graphic initialization failed: window is unavailable"); return false; }
    // Own the device error before any cleanup. The backend does not also log it.
    std::string deviceError;
    device=backend::CreateDevice(backend::DefaultDeviceOptions(),&deviceError);
    if (!device) {
        fail(deviceError);
        window=nullptr; return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device,window)) {
        fail(std::string("SDL_ClaimWindowForGPUDevice failed: ")+SDL_GetError());
        SDL_DestroyGPUDevice(device); device=nullptr; window=nullptr; return false;
    }
    try {
        uploads=std::make_unique<Upload::Queue>(device);
        renderer=std::make_unique<Render::Renderer>(device,window);
        programs=std::make_unique<Shader::ProgramStore>(device);
    } catch (const std::exception& error) {
        fail(std::string("Graphic service initialization failed: ")+error.what());
        programs.reset(); renderer.reset(); uploads.reset();
        SDL_ReleaseWindowFromGPUDevice(device,window); SDL_DestroyGPUDevice(device);
        device=nullptr; window=nullptr; return false;
    }
    presentMode=SDL_GPU_PRESENTMODE_VSYNC; // SDL's default after window claim.
    initialized=true;
    const auto* driver=SDL_GetGPUDeviceDriver(device);
    core::runtime::Log::Debug(std::string("Graphic module initialized: GPU backend = ")+(driver ? driver : "unknown"));
    return true;
}
Upload::Queue& Uploads() {
    if (!initialized) throw std::logic_error("Graphic uploads require initialization");
    return *uploads;
}
Render::Renderer& Frames() {
    if (!initialized) throw std::logic_error("Graphic frames require initialization");
    return *renderer;
}
Shader::ProgramStore& Shaders() {
    if(!initialized) throw std::logic_error("Graphic shaders require initialization");
    return *programs;
}
void SetVSync(bool enabled) {
    if(!initialized || !SDL_IsMainThread()) throw std::logic_error("Window.SetVSync requires the initialized graphics main thread");
    const auto mode=enabled?SDL_GPU_PRESENTMODE_VSYNC:SDL_GPU_PRESENTMODE_IMMEDIATE;
    if(mode==presentMode) { pendingPresentMode.reset(); return; }
    if(!SDL_WindowSupportsGPUPresentMode(device,window,mode))
        throw std::runtime_error("Window.SetVSync: requested SDL_GPU present mode is unsupported");
    // A synchronous window screenshot may acquire its present CB before Lua
    // returns. Recreating that swapchain would invalidate its pinned backbuffer.
    if(renderer->HasAcquiredSwapchain()) { pendingPresentMode=mode; return; }
    pendingPresentMode.reset();
    ApplyPresentation(mode);
}
bool IsVSyncEnabled() {
    if(!initialized || !SDL_IsMainThread()) throw std::logic_error("Window.IsVSyncEnabled requires the initialized graphics main thread");
    return presentMode==SDL_GPU_PRESENTMODE_VSYNC;
}
void ApplyPendingPresentation() {
    if(!initialized || !SDL_IsMainThread()) throw std::logic_error("Presentation changes require the initialized graphics main thread");
    if(!pendingPresentMode || renderer->HasAcquiredSwapchain()) return;
    const auto mode=*pendingPresentMode;
    // One request, one attempt. The host reports failure; keep the last applied
    // mode and never repeatedly retry a failed request during error cleanup.
    pendingPresentMode.reset();
    if(mode!=presentMode) ApplyPresentation(mode);
}
void Shutdown() {
    if (!initialized) return;
    pendingPresentMode.reset(); // Shutdown never recreates a swapchain.
    // Invalidate/retire Canvas before destroying the device. ResourceHost has
    // already joined workers; both services can now drain without new submissions.
    if (!renderer->Shutdown()) core::runtime::Log::Error("Renderer GPU drain failed; device will be destroyed");
    renderer.reset();
    programs->Shutdown(); programs.reset();
    if (!uploads->Shutdown()) core::runtime::Log::Error(uploads->FatalError()+"; device will be destroyed");
    uploads.reset();
    SDL_ReleaseWindowFromGPUDevice(device,window); SDL_DestroyGPUDevice(device);
    device=nullptr; window=nullptr; initialized=false;
    core::runtime::Log::Debug("Graphic module shutdown");
}
}
