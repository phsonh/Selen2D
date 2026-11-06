#pragma once
#include "InputState.h"
#include <SDL3/SDL_events.h>
#include <string>

namespace core::modules::Input {
// SDL main-thread service. Independent of Lua, Window, Steam and game actions.
class Gamepad final {
public:
    ~Gamepad() { Shutdown(); }
    Gamepad()=default;
    Gamepad(const Gamepad&)=delete;
    Gamepad& operator=(const Gamepad&)=delete;
    bool Init(std::string* error = nullptr);
    void Event(const SDL_Event&);
    GamepadSample Poll();
    void Shutdown();
private:
    SDL_Gamepad* device_{};
    bool initialized_{},dirty_{},neutral_{};
};
}
