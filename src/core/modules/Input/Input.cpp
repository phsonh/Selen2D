#include "Input.h"
#include "Gamepad.h"
#include "core/modules/Window/Window.h"
#include "core/runtime/Log/Log.h"
#include <SDL3/SDL.h>
#include <stdexcept>

namespace core::modules::Input {
namespace {
State snapshot;
Wheel wheel;
Gamepad gamepad;
SDL_Window* window{};
SDL_WindowID windowId{};
bool collecting{};
Uint64 wheelNotBefore{};
bool Focused() { return window && Window::IsFocused() && !Window::IsMinimized() && !(SDL_GetWindowFlags(window)&SDL_WINDOW_HIDDEN); }
void Check() { if(!window || !SDL_IsMainThread()) throw std::logic_error("Input requires an initialized main-thread service"); }
}
bool Init() {
    if(window) return true;
    auto* target=Window::GetNativeHandle();
    if(!target || !gamepad.Init()) return false;
    window=target; windowId=SDL_GetWindowID(window);
    snapshot.Reset(); wheel.Begin(false); collecting=false; wheelNotBefore=0;
    core::runtime::Log::Debug("SDL input initialized: physical keyboard, client mouse, one standard gamepad");
    return true;
}
void Shutdown() {
    gamepad.Shutdown(); window=nullptr; windowId=0; collecting=false;
    snapshot.Reset(); wheel.Begin(false); wheelNotBefore=0;
}
void BeginFrame() { if(window) { wheel.Begin(Focused()); collecting=true; } }
void HandleEvent(const SDL_Event& event) {
    if(!window) return;
    gamepad.Event(event);
    if(event.type==SDL_EVENT_GAMEPAD_REMOVED || event.type==SDL_EVENT_GAMEPAD_REMAPPED)
        snapshot.InvalidateGamepad(event.gdevice.which,event.type==SDL_EVENT_GAMEPAD_REMOVED);
    if((event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED || event.type==SDL_EVENT_WINDOW_HIDDEN) &&
        event.window.windowID==windowId) snapshot.InvalidateFocus();
    if(event.type==SDL_EVENT_WINDOW_MOUSE_LEAVE && event.window.windowID==windowId) snapshot.InvalidateMouse();
    if(!collecting) return;
    if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED || event.type==SDL_EVENT_WINDOW_HIDDEN) {
        if(event.window.windowID==windowId) wheel.Focus(false);
    } else if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) {
        if(event.window.windowID==windowId) wheel.Focus(true);
    } else if(event.type==SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        if(event.window.windowID==windowId) wheel.Clear();
    } else if(event.type==SDL_EVENT_MOUSE_WHEEL && event.wheel.windowID==windowId && snapshot.MouseEnabled() &&
              event.wheel.timestamp>=wheelNotBefore) {
        const double sign=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-1.:1.;
        wheel.Add(event.wheel.x*sign,event.wheel.y*sign);
    }
}
void EndFrame() {
    if(!window || !collecting) return;
    const auto metrics=Window::GetFrameMetrics();
    Sample sample; sample.focused=metrics.focused && !metrics.minimized && !metrics.hidden;
    sample.mouseFocused=SDL_GetMouseFocus()==window;
    int count{}; const bool* keys=SDL_GetKeyboardState(&count);
    sample.keys={keys,static_cast<std::size_t>(count)};
    sample.mouseButtons=SDL_GetMouseState(&sample.mouseX,&sample.mouseY);
    sample.width=metrics.logicalSize.width; sample.height=metrics.logicalSize.height;
    sample.wheelX=wheel.X(); sample.wheelY=wheel.Y();
    sample.gamepad=gamepad.Poll();
    snapshot.Publish(sample); collecting=false;
}
const State& GetState() { Check(); return snapshot; }
void SetMouseEnabled(bool enabled) {
    Check();
    // Events can already be queued while Lua toggles input. Do not replay wheel
    // events from the disabled interval when the next event pump runs.
    if(enabled!=snapshot.MouseEnabled()) wheelNotBefore=SDL_GetTicksNS();
    snapshot.SetMouseEnabled(enabled);
    if(!enabled) wheel.Clear();
}
void SetGamepadEnabled(bool enabled) { Check(); snapshot.SetGamepadEnabled(enabled); }
void SetGamepadDeadzone(double left,double right) { Check(); snapshot.SetGamepadDeadzone(left,right); }
void SetGamepadTriggerDeadzone(double left,double right) { Check(); snapshot.SetGamepadTriggerDeadzone(left,right); }
void SetGamepadStickButtonThreshold(double press,double release) { Check(); snapshot.SetGamepadStickButtonThreshold(press,release); }
void SetGamepadTriggerThreshold(double press,double release) { Check(); snapshot.SetGamepadTriggerThreshold(press,release); }
}
