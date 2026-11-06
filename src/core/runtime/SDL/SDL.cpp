#include "SDL.h"
#include <SDL3/SDL.h>
#include <string>
#include "core/runtime/Log/Log.h"

namespace core::runtime::SDL {
    namespace {
        bool Initialized = false;
    }
    bool Init(std::string* error)
    {
        if (error) error->clear();
        if (Initialized)
            return true;

        SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
        bool success =
            SDL_Init(SDL_INIT_VIDEO);

        if (!success)
        {
            const std::string message = std::string("SDL_Init failed: ") + SDL_GetError();
            if (error) *error = message;
            else core::runtime::Log::Error(message);

            return false;
        }

        Initialized = true;

        core::runtime::Log::Debug(
            "SDL runtime initialized"
        );

        return true;
    }


    void Shutdown()
    {
        if (!Initialized)
            return;

        SDL_Quit();

        Initialized = false;

        core::runtime::Log::Debug(
            "SDL runtime shutdown"
        );
    }
}
