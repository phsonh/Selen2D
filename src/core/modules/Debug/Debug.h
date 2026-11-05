#pragma once

#include <SDL3/SDL_messagebox.h>
#include <string>
#include <vector>

namespace core::modules::Debug {
enum class DialogType { Info, Warning, Error };

// Own the UTF-8 strings for the entire synchronous SDL call. No Lua pointers
// or native window handles survive the call.
struct Dialog {
    std::string message;
    std::string title;
    std::vector<std::string> buttons;
    DialogType type = DialogType::Info;
};

bool Init();
void Shutdown();


// The optional native call is a narrow test seam, not a Lua API or a backend
// selector. Production always uses SDL_ShowMessageBox. Return -1 for window
// close, otherwise the zero-based button index; throw on SDL failure.
using MessageBoxFunction = decltype(&SDL_ShowMessageBox);
int ShowDialog(const Dialog& dialog, MessageBoxFunction show = SDL_ShowMessageBox);
}
