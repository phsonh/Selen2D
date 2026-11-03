#pragma once
#include "InputState.h"
#include <SDL3/SDL_events.h>

namespace core::modules::Input {
    bool Init();
    void Shutdown();
    // Only the event pump calls these. Published input stays stable except for
    // explicit disabling or notifications that invalidate a device/window.
    void BeginFrame();
    void HandleEvent(const SDL_Event& event);
    void EndFrame();
    const State& GetState();
    void SetMouseEnabled(bool enabled);
    void SetGamepadEnabled(bool enabled);
    void SetGamepadDeadzone(double left, double right);
    void SetGamepadTriggerDeadzone(double left, double right);
    void SetGamepadStickButtonThreshold(double press, double release);
    void SetGamepadTriggerThreshold(double press, double release);
}
