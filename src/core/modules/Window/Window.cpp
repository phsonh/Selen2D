#include "Window.h"
#include "BuildInfo.h"
#include "core/runtime/Log/Log.h"
#include <stdexcept>
#include <string>

namespace core::modules::Window {
namespace {
SDL_Window* window{};
bool initialFullscreen{}; // Only a pre-show request, never the IsFullscreen result.
void Check() {
    if(!SDL_IsMainThread()) throw std::logic_error("Window requires the SDL main thread");
    if(!window) throw std::logic_error("Window is not initialized");
}
void Require(bool success,const char* operation) {
    if(!success) throw std::runtime_error(std::string(operation)+": "+SDL_GetError());
}
}
bool Init(int width,int height) {
    if(window) return true;
    initialFullscreen=false;
    window=SDL_CreateWindow(engine::build::Title,width,height,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!window) { core::runtime::Log::Error(std::string("Window initialization failed: ")+SDL_GetError()); return false; }
    core::runtime::Log::Debug("Window initialized hidden");
    return true;
}
SDL_Window* GetNativeHandle() { return window; }
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
Size GetSize() { Check(); Size size{}; Require(SDL_GetWindowSize(window,&size.width,&size.height),"Window.GetSize failed"); return size; }
void SetFullscreen(bool enabled) {
    Check();
    // SDL owns the windowed rect and pending hidden-window state. A null mode
    // explicitly selects the desktop; never choose an exclusive display mode.
    if(enabled) Require(SDL_SetWindowFullscreenMode(window,nullptr),"Window desktop fullscreen mode failed");
    Require(SDL_SetWindowFullscreen(window,enabled),"Window.SetFullscreen failed");
    if(SDL_GetWindowFlags(window)&SDL_WINDOW_HIDDEN) initialFullscreen=enabled;
}
bool IsFullscreen() { Check(); return (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=0; }
void SetResizable(bool enabled) { Check(); Require(SDL_SetWindowResizable(window,enabled),"Window.SetResizable failed"); }
bool IsResizable() { Check(); return (SDL_GetWindowFlags(window)&SDL_WINDOW_RESIZABLE)!=0; }
bool IsFocused() { Check(); return SDL_GetKeyboardFocus()==window; }
void SetCursorVisible(bool visible) { Check(); Require(visible?SDL_ShowCursor():SDL_HideCursor(),"Window.SetCursorVisible failed"); }
bool IsCursorVisible() { Check(); return SDL_CursorVisible(); }
void Shutdown() {
    if(!window) return;
    SDL_DestroyWindow(window); window=nullptr; initialFullscreen=false;
    core::runtime::Log::Debug("Window shutdown");
}
}
