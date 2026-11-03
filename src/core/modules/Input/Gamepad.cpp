#include "Gamepad.h"
#include "core/runtime/Log/Log.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <string>

namespace core::modules::Input {
bool Gamepad::Init() {
    if(initialized_) return true;
    if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        core::runtime::Log::Error(std::string("SDL gamepad initialization failed: ")+SDL_GetError()); return false;
    }
    initialized_=dirty_=true; neutral_=false;
    return true;
}
void Gamepad::Event(const SDL_Event& event) {
    if(!initialized_) return;
    if(event.type==SDL_EVENT_GAMEPAD_ADDED || event.type==SDL_EVENT_GAMEPAD_REMOVED || event.type==SDL_EVENT_GAMEPAD_REMAPPED)
        dirty_=true;
    if(device_ && event.type==SDL_EVENT_GAMEPAD_REMAPPED && event.gdevice.which==SDL_GetGamepadID(device_)) neutral_=true;
}
GamepadSample Gamepad::Poll() {
    GamepadSample result;
    if(!initialized_) return result;
    // SDL device callbacks may run on another thread. Freeze the complete set
    // of axis/button reads; SDL's documented joystick lock is recursive.
    struct Lock { Lock() { SDL_LockJoysticks(); } ~Lock() { SDL_UnlockJoysticks(); } } lock;
    if(device_ && !SDL_GamepadConnected(device_)) {
        SDL_CloseGamepad(device_); device_=nullptr; dirty_=true; neutral_=false;
        return result; // One complete release snapshot before any replacement.
    }
    if(neutral_) { neutral_=false; return result; }
    if(!device_ && dirty_) {
        dirty_=false;
        int count{};
        auto* ids=SDL_GetGamepads(&count);
        if(!ids) { core::runtime::Log::Warn(std::string("Gamepad discovery failed: ")+SDL_GetError()); return result; }
        std::sort(ids,ids+count);
        for(int i=0;i<count;++i) {
            device_=SDL_OpenGamepad(ids[i]);
            if(device_) break;
            core::runtime::Log::Warn(std::string("Cannot open gamepad: ")+SDL_GetError());
        }
        SDL_free(ids);
    }
    if(!device_) return result;
    result.id=SDL_GetGamepadID(device_);
    for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i) result.buttons[i]=SDL_GetGamepadButton(device_,SDL_GamepadButton(i));
    for(int i=0;i<SDL_GAMEPAD_AXIS_COUNT;++i) result.axes[i]=SDL_GetGamepadAxis(device_,SDL_GamepadAxis(i));
    return result;
}
void Gamepad::Shutdown() {
    if(device_) SDL_CloseGamepad(device_);
    device_=nullptr; dirty_=neutral_=false;
    if(initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    initialized_=false;
}
}
