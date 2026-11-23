#include "Engine.h"
#include "StartupFailure.h"
#include "core/modules/File/Profile.h"
#include "ResourceHost.h"
#include "BuildInfo.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
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
#include "core/modules/Graphic/shader/DxcRuntime.h"
#include "core/modules/Graphic/render/Renderer.h"
#include "core/modules/Graphic/render/Profile.h"
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

void Report(const char* context,std::string_view error,StartupFailure::Failure* failure=nullptr,
    StartupFailure::Kind kind=StartupFailure::Kind::Runtime) noexcept {
    if(failure) failure->Set(kind,error);
    try { std::string message(context); message+=": "; message.append(error); Log::Error(message); }
    catch(...) { Log::Error(error); }
}
void ReportLua(const char* context,const std::string& error,StartupFailure::Failure* failure,StartupFailure::Kind kind) noexcept {
    if(failure) failure->Set(kind,error);
    try { Log::LuaError(std::string(context)+": "+error); }
    catch(...) { Log::LuaError(error); } // Preserve the original diagnostic if context allocation fails.
}
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
bool Exit(StartupFailure::Failure* failure) {
    if(!bindingsReady || exitCalled) return exitSucceeded;
    exitCalled=true;
    if(objects) objects->BeginShutdown();
    try {
        std::string error;
        exitSucceeded=Lua::CallExitFunction(error);
        if(!exitSucceeded) ReportLua("Lua exit callback failed",error,failure,StartupFailure::Kind::LuaExit);
    } catch(const std::exception& error) {
        exitSucceeded=false; Report("Exit callback dispatch failed",error.what(),failure,StartupFailure::Kind::Cleanup);
    } catch(...) {
        exitSucceeded=false; Report("Exit callback dispatch failed","unknown native exception",failure,StartupFailure::Kind::Cleanup);
    }
    return exitSucceeded;
}
bool Pump() {
    Event::Pump();
    quitRequested=quitRequested || Event::Events.QuitRequested || Log::TerminationRequested();
    return !quitRequested;
}
}
bool Init(const std::string& entry, std::shared_ptr<const core::modules::File::FileSystem> fileSystem,
    const std::vector<std::string>& arguments, StartupFailure::Failure* failure) {
    VFS_PROFILE_SCOPE(EngineInit);
    if(failure) *failure={};
    if(started) { Report("Engine initialization failed","Engine must be shut down before reinitialization",failure,StartupFailure::Kind::NativeServices); return false; }
    started=true; initialized=bindingsReady=ran=quitRequested=exitCalled=false; exitSucceeded=true;
    clock.Reset(SDL_GetTicksNS());
    if(!Log::Init()) {
        if(failure) failure->Set(StartupFailure::Kind::Logging,Log::InitializationErrorText());
        return false;
    }
    Log::Info(std::string(build::Title) + " | " + build::Developer + " | MIT License (third-party notices retained)");
    Log::Info("Engine initializing");
    auto phase=StartupFailure::Kind::Dxc;
    const char* nativeOperation="native services";
    bool entryStarted{};
    try {
        Graphic::Shader::DxcRuntime::Initialize();
        phase=StartupFailure::Kind::NativeServices;
#ifdef ENGINE_SPRITE_PROFILE
        Graphic::Render::profile::Initialize();
#endif
        if(!fileSystem) throw std::invalid_argument("Engine requires a mounted FileSystem");
        files=std::move(fileSystem);
        startupArguments=arguments;
        std::string nativeError;
        const auto initialize=[&](const char* name,auto&& operation) {
            nativeOperation=name;
            nativeError.clear();
            if(!operation()) throw std::runtime_error(nativeError.empty()?"initialization returned false":nativeError);
        };
        initialize("SDL",[&] { return core::runtime::SDL::Init(&nativeError); });
        initialize("Event",[] { return Event::Init(); });
        initialize("Lua",[&] { return Lua::Init(&nativeError); });
        initialize("Debug",[] { return core::modules::Debug::Init(); });
        initialize("Window",[&] { return core::modules::Window::Init(1280,960,&nativeError); });
        initialize("Input",[&] { return core::modules::Input::Init(&nativeError); });
        initialize("Graphic",[&] { VFS_PROFILE_SCOPE(GpuInit); return Graphic::Init(&nativeError); });
        initialize("Math",[] { return core::modules::Math::Init(); });
        nativeOperation="Audio";
        audio.SetFileSystem(files);
        { VFS_PROFILE_SCOPE(AudioInit); audio.Initialize(); }
        nativeOperation="ResourceHost";
        resources=std::make_unique<ResourceHost>(Graphic::Uploads(),files,&audio,&Graphic::Shaders());
        nativeOperation="Lua bindings";
        RegisterBindings();
        bindingsReady=true; // Entry errors must still run an already registered ExitFunction.
        std::string error;
        VFS_PROFILE_MARK(BeforeEntryElapsed);
        phase=StartupFailure::Kind::Configuration;
        {
            VFS_PROFILE_SCOPE(Entry);
            if(!entry.empty() && !Lua::DoFile(entry,*files,error,&entryStarted)) {
                if(entryStarted) ReportLua("Failed to execute entry script",error,failure,StartupFailure::Kind::LuaEntry);
                else {
                    if(failure) failure->Set(phase,error);
                    Report("Failed to read entry script",error);
                }
                return false;
            }
        }
        phase=StartupFailure::Kind::NativeServices;
        nativeOperation="Window startup presentation";
        // Finish the current initialization/entry call before processing an
        // asynchronous console request. It must never run cleanup on a worker
        // thread or interrupt Lua/native code halfway through an operation.
        quitRequested=quitRequested || Log::TerminationRequested();
        if(!quitRequested) {
            core::modules::Window::ShowReady();
            VFS_PROFILE_MARK(WindowShownElapsed);
        }
        initialized=true; // Do not overwrite Quit requested by entry Lua.
        Log::Info("Engine initialized"); return true;
    } catch(const std::exception& error) {
        // The service captured its original detail at the failing operation,
        // before SDL/native cleanup could replace a thread-local last error.
        try {
            const std::string detail=phase==StartupFailure::Kind::NativeServices
                ? std::string(nativeOperation)+": "+error.what() : error.what();
            if(failure) failure->Set(phase,detail);
            Report("Engine initialization failed",detail);
        } catch(...) {
            Report(nativeOperation,error.what(),failure,phase);
        }
        return false;
    } catch(...) {
        Report("Engine initialization failed","unknown native exception",failure,phase); return false;
    }
}
bool Run(std::uint64_t frameLimit,StartupFailure::Failure* failure) {
    if(!initialized || ran) { Report("Engine frame failed","Engine.Run requires a newly initialized host",failure); return false; }
    ran=true;
    bool succeeded=true;
    std::uint64_t hostFrames{};
    auto& renderer=Graphic::Frames();
    auto& uploads=Graphic::Uploads();
    bool recording{};
    const auto abort=[&] {
        if(!recording) return;
        try { renderer.AbortFrame(); recording=false; Graphic::ApplyPendingPresentation(); }
        catch(const std::exception& cleanup) { Report("Engine frame cleanup failed",cleanup.what(),failure,StartupFailure::Kind::Cleanup); }
        catch(...) { Report("Engine frame cleanup failed","unknown native exception",failure,StartupFailure::Kind::Cleanup); }
    };
    try {
        Event::BeginFrame();
        while(!quitRequested) {
            // Like Coral2D's Begin/End: all host work belongs to this actual
            // frame. Late work or sleep never shortens a later frame's budget.
            clock.BeginFrame(SDL_GetTicksNS());
            if(!Pump()) break;
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
            if(Lua::HasFrameFunction()) clock.BeginCallback(SDL_GetTicksNS());
#ifdef ENGINE_SPRITE_PROFILE
            Graphic::Render::profile::Begin(clock.FrameCount(),renderer.GetStats());
#endif
            audio.SetRuntimeFrame(clock.FrameCount());
            std::string error;
#ifdef ENGINE_SPRITE_PROFILE
            Graphic::Render::profile::Timer recordTimer(Graphic::Render::profile::Phase::Record);
#endif
            if(!Lua::CallFrameFunction(error)) {
                ReportLua("Lua frame callback failed",error,failure,StartupFailure::Kind::LuaFrame); succeeded=false;
                renderer.AbortFrame(); recording=false;
                Graphic::ApplyPendingPresentation(); break;
            }
#ifdef ENGINE_SPRITE_PROFILE
            recordTimer.Stop();
#endif
            renderer.EndFrame(); recording=false;
            // Keep the final update and draw of a deleted object in this frame.
            if(!objects->FinishFrame(error)) {
                ReportLua("Frame-end Object.Delete callback failed",error,failure,StartupFailure::Kind::LuaFrame);
                succeeded=false;
                break;
            }
            // Submission boundary, not a GPU timestamp or physical display time.
            if(hostFrames==0) { VFS_PROFILE_MARK(FirstFrameSubmitElapsed); }
            Graphic::ApplyPendingPresentation();
            // Device startup, refill and reclamation run independently on the
            // audio service thread. Only report queued failures on this thread.
            audio.CheckErrors();
#ifdef ENGINE_SPRITE_PROFILE
            Graphic::Render::profile::Finish(renderer.GetStats());
#endif
            ++hostFrames;
            if(frameLimit && hostFrames>=frameLimit) quitRequested=true;
            if(quitRequested) break;
            // Collect next callback's input throughout this frame's end wait.
            // Do not clear it at the next loop start (notably mouse wheel).
            Event::BeginFrame();
            while(Pump()) {
                const auto remaining=clock.Remaining(SDL_GetTicksNS());
                if(!remaining) break;
                // SDL_DelayPrecise may still enter the OS sleep for sub-ms
                // requests. Keep a fixed, current-frame tail instead of paying
                // that oversleep back in a later frame. Long waits stay sliced
                // for events; only the final <=1 ms is explicitly busy-waited.
                if(remaining>6'000'000)
                    SDL_DelayNS(std::min<std::uint64_t>(remaining-2'000'000,4'000'000));
                else if(remaining>1'000'000)
                    SDL_DelayPrecise(remaining-1'000'000);
                else {
                    do { SDL_CPUPauseInstruction(); }
                    while(clock.Remaining(SDL_GetTicksNS()));
                }
            }
        }
    } catch(const std::exception& error) {
        Report("Engine frame failed",error.what(),failure); succeeded=false;
        // A terminal GPU failure may already have closed recording. Keep its
        // in-flight pins until Graphic shutdown drains the device.
        abort();
    } catch(...) {
        Report("Engine frame failed","unknown native exception",failure); succeeded=false;
        abort();
    }
    quitRequested=true;
    const bool exited=Exit(failure);
    return succeeded && exited;
}
bool Shutdown(StartupFailure::Failure* failure) {
    if(!started) return exitSucceeded;
    quitRequested=true;
    const auto cleanup=[&](const char* operation,auto&& action) {
        try { action(); }
        catch(const std::exception& error) { Report(operation,error.what(),failure,StartupFailure::Kind::Cleanup); exitSucceeded=false; }
        catch(...) { Report(operation,"unknown native exception",failure,StartupFailure::Kind::Cleanup); exitSucceeded=false; }
    };
    cleanup("Exit callback dispatch failed",[&]{ Exit(failure); });
    Log::Info("Engine shutting down");
    cleanup("Object shutdown failed",[&]{
        if(!objects) return;
        objects->BeginShutdown();
        std::string error;
        if(!objects->FinalizeShutdown(error)) {
            // Individual callback diagnostics were already logged by Object.
            if(failure) failure->Set(StartupFailure::Kind::ObjectCleanup,error);
            exitSucceeded=false;
        }
    });
    // Keep the established cleanup order, and continue safe independent cleanup
    // after a reported failure. No presentation runs here or inside a Lua call.
    cleanup("Binding cleanup failed",[&]{ graphicBindings.reset(); resourceBindings.reset(); audioBindings.reset(); });
    cleanup("Audio drain failed",[&]{ audio.StopAllPlayersAndDrain(); });
    cleanup("Resource cleanup failed",[&]{ objects.reset(); resources.reset(); });
    cleanup("Lua shutdown failed",[]{ Lua::Shutdown(); });
    startupArguments.clear();
    cleanup("Math shutdown failed",[]{ core::modules::Math::Shutdown(); });
    cleanup("Audio shutdown failed",[]{ audio.Shutdown(); });
    if(audio.DrainErrors([](std::string_view message,void* context) noexcept {
        if(auto* result=static_cast<StartupFailure::Failure*>(context)) result->Set(StartupFailure::Kind::Cleanup,message);
        Log::Error(message);
    },failure)) exitSucceeded=false;
    cleanup("Graphic shutdown failed",[]{ Graphic::Shutdown(); });
    files.reset();
    cleanup("Input shutdown failed",[]{ core::modules::Input::Shutdown(); });
    cleanup("Window shutdown failed",[]{ core::modules::Window::Shutdown(); });
    cleanup("Debug shutdown failed",[]{ core::modules::Debug::Shutdown(); });
    cleanup("Event shutdown failed",[]{ Event::Shutdown(); });
    cleanup("SDL shutdown failed",[]{ core::runtime::SDL::Shutdown(); });
    initialized=bindingsReady=false; started=false;
#ifdef ENGINE_SPRITE_PROFILE
    cleanup("Sprite profile output failed",[]{ Graphic::Render::profile::Flush(); });
#endif
    Log::Info("Engine shutdown complete"); Log::Shutdown();
    return exitSucceeded;
}
}
