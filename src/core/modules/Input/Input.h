#pragma once
#include "InputState.h"
#include <SDL3/SDL_events.h>

namespace core::modules::Input {
    bool Init();
    void Shutdown();
    // Only the event pump calls these. Consumers see an immutable frame snapshot.
    void BeginFrame();
    void HandleEvent(const SDL_Event& event);
    void EndFrame();
    const State& GetState();
    void SetMouseEnabled(bool enabled);
}
