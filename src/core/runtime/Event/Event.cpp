#include "Event.h"
#include <SDL3/SDL.h>
#include "core/runtime/Log/Log.h"
#include "core/modules/Input/Input.h"
#include "core/modules/Window/Window.h"

namespace core::runtime::Event {
    namespace Input = core::modules::Input;
    namespace Window = core::modules::Window;
    namespace {
        bool Initialized = false;
    }
    Engine_Events Events;
    bool Init()
    {
        if (Initialized)
            return true;

        Initialized = true;
        Events = {};

        core::runtime::Log::Debug(
            "Event runtime initialized"
        );

        return true;
    }
    void BeginFrame() {
        Events = {};
        Input::BeginFrame();
    }
    void Pump() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            Input::HandleEvent(event);
            switch (event.type)
            {
                case SDL_EVENT_QUIT:
                    Events.QuitRequested = true;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (event.window.windowID == SDL_GetWindowID(Window::GetNativeHandle())) Events.QuitRequested = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    if (event.window.windowID == SDL_GetWindowID(Window::GetNativeHandle())) Events.FocusGained = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    if (event.window.windowID == SDL_GetWindowID(Window::GetNativeHandle())) Events.FocusLost = true;
                    break;
            }
        }
    }
    void EndFrame() { Input::EndFrame(); }
    void Shutdown()
    {
        if (!Initialized)
            return;

        Initialized = false;

        core::runtime::Log::Debug(
            "Event runtime shutdown"
        );
    }
}
