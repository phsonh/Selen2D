#include "Window.h"
#include "BuildInfo.h"
#include "core/runtime/Log/Log.h"
#include <stdexcept>
#include <string>

namespace core::modules::Window {
namespace {
SDL_Window* window{};
SDL_WindowID windowId{};
State state;
bool initialFullscreen{}; // Only a pre-show request, never the IsFullscreen result.
void Check() {
    if(!SDL_IsMainThread()) throw std::logic_error("Window requires the SDL main thread");
    if(!window) throw std::logic_error("Window is not initialized");
}
void Require(bool success,const char* operation) {
    if(!success) throw std::runtime_error(std::string(operation)+": "+SDL_GetError());
}
FrameMetrics ReadMetrics() {
    FrameMetrics metrics;
    Require(SDL_GetWindowSize(window,&metrics.logicalSize.width,&metrics.logicalSize.height),"Window.GetSize failed");
    metrics.focused=SDL_GetKeyboardFocus()==window;
    metrics.minimized=state.Minimized(); metrics.hidden=state.Hidden();
    return metrics;
}
}
bool Init(int width,int height,std::string* error) {
    if(error) error->clear();
    if(window) return true;
    initialFullscreen=false;
    state=State{};
    window=SDL_CreateWindow(engine::build::Title,width,height,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!window) {
        const std::string message=std::string("SDL_CreateWindow failed: ")+SDL_GetError();
        if(error) *error=message;
        else core::runtime::Log::Error(message);
        return false;
    }
    windowId=SDL_GetWindowID(window);
    core::runtime::Log::Debug("Window initialized hidden");
    return true;
}
SDL_Window* GetNativeHandle() { return window; }
void HandleEvent(const SDL_Event& event) {
    if(!window || event.type<SDL_EVENT_WINDOW_FIRST || event.type>SDL_EVENT_WINDOW_LAST || event.window.windowID!=windowId) return;
    // SDL_GetWindowFlags includes pending hidden-window requests. Consume the
    // confirmed transition events instead of reporting a request as applied.
    state.HandleEvent(static_cast<SDL_EventType>(event.type));
}
void PublishFrameState() {
    Check();
    const auto metrics=ReadMetrics();
    state.Publish(metrics.logicalSize,metrics.focused);
}
FrameMetrics GetFrameMetrics() {
    Check();
    // Startup Lua can configure the window before the first frame exists.
    // Thereafter a resize request never changes an already-published input frame.
    return state.Published()?state.Frame():ReadMetrics();
}
void ShowReady() {
    Check();
    // SDL applies a hidden window's pending fullscreen AFTER its SHOWN event.
    // Hide that native transition, without touching SDL internals or native OS
    // handles. Opacity is internal startup policy, not another public Window API.
    const bool covered=initialFullscreen && SDL_SetWindowOpacity(window,0);
    if(initialFullscreen && !covered)
        core::runtime::Log::Warn(std::string("Window startup fullscreen transition cannot be concealed on this platform: ")+SDL_GetError());
    try {
        Require(SDL_ShowWindow(window),"Window initial show failed");
        if(initialFullscreen) Require(SDL_SyncWindow(window),"Window startup fullscreen synchronization failed");
        if(covered) Require(SDL_SetWindowOpacity(window,1),"Window startup visibility restore failed");
        initialFullscreen=false;
    } catch(...) {
        SDL_HideWindow(window);
        throw;
    }
}
void SetTitle(const char* title) { Check(); Require(SDL_SetWindowTitle(window,title),"Window.SetTitle failed"); }
const char* GetTitle() { Check(); return SDL_GetWindowTitle(window); }
void SetSize(int width,int height) { Check(); Require(SDL_SetWindowSize(window,width,height),"Window.SetSize failed"); }
Size GetSize() { return GetFrameMetrics().logicalSize; }
void SetFullscreen(bool enabled) {
    Check();
    // SDL owns the windowed rect and pending hidden-window state. A null mode
    // explicitly selects the desktop; never choose an exclusive display mode.
    if(enabled) Require(SDL_SetWindowFullscreenMode(window,nullptr),"Window desktop fullscreen mode failed");
    Require(SDL_SetWindowFullscreen(window,enabled),"Window.SetFullscreen failed");
    if(SDL_GetWindowFlags(window)&SDL_WINDOW_HIDDEN) initialFullscreen=enabled;
}
bool IsFullscreen() { Check(); return state.Fullscreen(); }
void SetResizable(bool enabled) { Check(); Require(SDL_SetWindowResizable(window,enabled),"Window.SetResizable failed"); }
bool IsResizable() { Check(); return (SDL_GetWindowFlags(window)&SDL_WINDOW_RESIZABLE)!=0; }
bool IsFocused() { Check(); return SDL_GetKeyboardFocus()==window; }
bool IsMinimized() { Check(); return state.Minimized(); }
void SetCursorVisible(bool visible) { Check(); Require(visible?SDL_ShowCursor():SDL_HideCursor(),"Window.SetCursorVisible failed"); }
bool IsCursorVisible() { Check(); return SDL_CursorVisible(); }
void Shutdown() {
    if(!window) return;
    SDL_DestroyWindow(window); window=nullptr; windowId=0; initialFullscreen=false; state=State{};
    core::runtime::Log::Debug("Window shutdown");
}
}
