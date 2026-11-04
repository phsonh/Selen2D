#include "Engine.h"
#include "ResourceHost.h"
#include "BuildInfo.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <SDL3/SDL.h>
#include "core/runtime/Log/Log.h"
#include "core/runtime/Event/Event.h"
#include "core/runtime/Clock/Clock.h"
#include "core/runtime/Lua/Lua.h"
#include "core/runtime/Lua/VfsScripts.h"
#include "core/modules/File/Vfs.h"
#include "core/runtime/SDL/SDL.h"
#include "core/modules/Window/Window.h"
#include "core/modules/Debug/Debug.h"
#include "core/modules/Math/Math.h"
#include "core/modules/Graphic/Graphic.h"
#include "core/modules/Graphic/render/Renderer.h"
#include "core/modules/Graphic/upload/Upload.h"
#include "core/modules/Input/Input.h"
#include "core/modules/Audio/Audio.h"
#include "lua_bind/runtime/Runtime.h"
#include "lua_bind/modules/Time/Time.h"
#include "lua_bind/modules/Text/Text.h"
#include "lua_bind/modules/Window/Window.h"
#include "lua_bind/modules/Math/Math.h"
#include "lua_bind/modules/Debug/Debug.h"
#include "lua_bind/modules/Input/Input.h"
#include "lua_bind/modules/File/File.h"
#include "lua_bind/modules/Audio/Audio.h"
#include "lua_bind/modules/Object/Object.h"
#include "lua_bind/modules/Resource/Resource.h"
#include "lua_bind/modules/Graphic/Graphic.h"

namespace engine {
namespace {
namespace Lua=core::runtime::Lua;
namespace Event=core::runtime::Event;
namespace Log=core::runtime::Log;
namespace Graphic=core::modules::Graphic;
// Host lifetime, not game state. Borrowed by Lua until the VM is closed.
core::runtime::Clock clock;
bool started{}, initialized{}, bindingsReady{}, ran{}, quitRequested{}, exitCalled{}, exitSucceeded{true};
std::vector<std::string> startupArguments;
core::modules::Audio::AudioSystem audio;
std::unique_ptr<lua_bind::modules::Object::Context> objects;
std::unique_ptr<ResourceHost> resources;
std::unique_ptr<lua_bind::modules::Resource::Context> resourceBindings;
std::unique_ptr<lua_bind::modules::Audio::Context> audioBindings;
std::unique_ptr<lua_bind::modules::Graphic::Context> graphicBindings;
std::shared_ptr<const core::modules::File::FileSystem> files;

void Report(const char* context,const std::string& error) { Log::Error(std::string(context)+": "+error); }
void RegisterBindings() {
    auto* L=Lua::GetState();
    lua_bind::runtime::Register(L,clock,quitRequested,startupArguments);
    lua_bind::modules::Time::Register(L,clock);
    lua_bind::modules::Text::Register(L);
    lua_bind::modules::Window::Register(L);
    lua_bind::modules::Input::Register(L);
    lua_bind::modules::Math::Register(L);
    lua_bind::modules::File::Register(L,files);
    Lua::InstallSearchers(L,files);
    resourceBindings=std::make_unique<lua_bind::modules::Resource::Context>(L,resources->Resources(),
        [](auto& job){ resources->Wait(job); },files);
    lua_bind::modules::Resource::Register(L,*resourceBindings);
    audioBindings=std::make_unique<lua_bind::modules::Audio::Context>(L,audio,resources->Resources());
    lua_bind::modules::Audio::Register(L,*audioBindings);
    graphicBindings=std::make_unique<lua_bind::modules::Graphic::Context>(L,Graphic::Frames(),resources->Resources());
    lua_bind::modules::Graphic::Register(L,*graphicBindings);
    objects=std::make_unique<lua_bind::modules::Object::Context>(L,65536);
    lua_bind::modules::Object::Register(L,*objects);
    lua_bind::modules::Debug::Register(L);
}
bool Exit() {
    if(!bindingsReady || exitCalled) return exitSucceeded;
    exitCalled=true;
    if(objects) objects->BeginShutdown();
    std::string error;
    exitSucceeded=Lua::CallExitFunction(error);
    if(!exitSucceeded) Report("Lua exit callback failed",error);
    return exitSucceeded;
}
bool Pump() {
    Event::Pump();
    quitRequested=quitRequested || Event::Events.QuitRequested;
    return !quitRequested;
}
}
bool Init(const std::string& entry, std::shared_ptr<const core::modules::File::FileSystem> fileSystem,
    const std::vector<std::string>& arguments) {
    if(started) { Log::Error("Engine must be shut down before reinitialization"); return false; }
    started=true; initialized=bindingsReady=ran=quitRequested=exitCalled=false; exitSucceeded=true;
    clock.Reset(SDL_GetTicksNS());
    if(!Log::Init()) return false;
    Log::Info(std::string(build::Title) + " | " + build::Developer + " | MIT License (third-party notices retained)");
    Log::Info("Engine initializing");
    try {
        if(!fileSystem) throw std::invalid_argument("Engine requires a mounted FileSystem");
        files=std::move(fileSystem);
        startupArguments=arguments;
        if(!core::runtime::SDL::Init() || !Event::Init() || !Lua::Init() ||
           !core::modules::Debug::Init() || !core::modules::Window::Init(1280,960) ||
           !core::modules::Input::Init() || !Graphic::Init() || !core::modules::Math::Init())
            throw std::runtime_error("native service initialization failed");
        audio.SetFileSystem(files);
        audio.Initialize();
        resources=std::make_unique<ResourceHost>(Graphic::Uploads(),files,&audio,&Graphic::Shaders());
        RegisterBindings();
        bindingsReady=true; // Entry errors must still run an already registered ExitFunction.
        std::string error;
        if(!entry.empty() && !Lua::DoFile(entry,*files,error)) {
            Report("Failed to execute entry script",error); return false;
        }
        if(!quitRequested) core::modules::Window::ShowReady();
        initialized=true; // Do not overwrite Quit requested by entry Lua.
        Log::Info("Engine initialized"); return true;
    } catch(const std::exception& error) { Report("Engine initialization failed",error.what()); return false; }
}
bool Run(std::uint64_t frameLimit) {
    if(!initialized || ran) { Log::Error("Engine.Run requires a newly initialized host"); return false; }
    ran=true;
    bool succeeded=true;
    std::uint64_t hostFrames{};
    auto& renderer=Graphic::Frames();
    auto& uploads=Graphic::Uploads();
    bool recording{};
    try {
        while(!quitRequested) {
            Event::BeginFrame();
            // Bound each sleep so SDL events remain responsive even at low FPS.
            // Only the previous ACTUAL start is used, never accumulated deadlines.
            while(Pump()) {
                const auto remaining=clock.Remaining(SDL_GetTicksNS());
                if(!remaining) break;
                SDL_DelayNS(std::min<std::uint64_t>(remaining,4'000'000));
            }
            if(quitRequested) break;
            resources->Tick();
            uploads.Tick();
            if(!uploads.FatalError().empty()) throw std::runtime_error(uploads.FatalError());
            // A whole Lua frame includes rendering. Wait for a recording slot
            // BEFORE calling it; never skip the callback or call it twice.
            while(Pump()) {
                if(renderer.TryBeginFrame()) { recording=true; break; }
                SDL_Delay(1);
            }
            if(!recording) break;
            if(!Pump()) { renderer.AbortFrame(); recording=false; Graphic::ApplyPendingPresentation(); break; }
            Event::EndFrame();
            const auto windowMetrics=core::modules::Window::GetFrameMetrics();
            renderer.Canvases().PublishWindowMetrics(windowMetrics.logicalSize.width,windowMetrics.logicalSize.height);
            if(quitRequested) { renderer.AbortFrame(); recording=false; Graphic::ApplyPendingPresentation(); break; }
            clock.BeginFrame(SDL_GetTicksNS(),Lua::HasFrameFunction());
            audio.SetRuntimeFrame(clock.FrameCount());
            std::string error;
            if(!Lua::CallFrameFunction(error)) {
                Report("Lua frame callback failed",error); succeeded=false;
                renderer.AbortFrame(); recording=false;
                Graphic::ApplyPendingPresentation(); break;
            }
            renderer.EndFrame(); recording=false;
            Graphic::ApplyPendingPresentation();
            // Device startup, refill and reclamation run independently on the
            // audio service thread. Only report queued failures on this thread.
            audio.CheckErrors();
            ++hostFrames;
            if(frameLimit && hostFrames>=frameLimit) quitRequested=true;
        }
    } catch(const std::exception& error) {
        Report("Engine frame failed",error.what()); succeeded=false;
        // A terminal GPU failure may already have closed recording. Keep its
        // in-flight pins until Graphic shutdown drains the device.
        if(recording) {
            try { renderer.AbortFrame(); recording=false; Graphic::ApplyPendingPresentation(); }
            catch(const std::exception& cleanup) { Report("Engine frame cleanup failed",cleanup.what()); }
            catch(...) { Log::Error("Engine frame cleanup failed: unknown native exception"); }
        }
    }
    quitRequested=true;
    const bool exited=Exit();
    return succeeded && exited;
}
bool Shutdown() {
    if(!started) return exitSucceeded;
    quitRequested=true;
    Exit(); // Also covers Init followed by Shutdown without Run.
    Log::Info("Engine shutting down");
    if(objects) {
        objects->BeginShutdown();
        std::string error;
        if(!objects->FinalizeShutdown(error)) {
            Report("Object shutdown callbacks failed",error);
            exitSucceeded=false;
        }
    }
    // Exit can still use File, Audio and initialized resources. Ordinary drawing
    // is illegal here because no frame is recording. Revoke bindings afterwards.
    graphicBindings.reset(); resourceBindings.reset(); audioBindings.reset();
    audio.StopAllPlayersAndDrain();
    objects.reset(); resources.reset();
    Lua::Shutdown();
    startupArguments.clear();
    core::modules::Math::Shutdown(); audio.Shutdown(); Graphic::Shutdown();
    files.reset();
    core::modules::Input::Shutdown(); core::modules::Window::Shutdown();
    core::modules::Debug::Shutdown(); Event::Shutdown(); core::runtime::SDL::Shutdown();
    initialized=bindingsReady=false; started=false;
    Log::Info("Engine shutdown complete"); Log::Shutdown();
    return exitSucceeded;
}
}
