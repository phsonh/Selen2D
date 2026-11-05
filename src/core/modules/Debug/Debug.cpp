#include "Debug.h"
#include "core/runtime/Log/Log.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <limits>
#include <stdexcept>

namespace core::modules::Debug {
namespace {
bool initialized = false;
}

bool Init() {
    if (!initialized) {
        initialized = true;
        core::runtime::Log::Debug("Debug module initialized");
    }
    return true;
}

void Shutdown() {
    if (initialized) {
        initialized = false;
        core::runtime::Log::Debug("Debug module shutdown");
    }
}

int ShowDialog(const Dialog& dialog, MessageBoxFunction show) {
    if (!SDL_IsMainThread())
        throw std::runtime_error("Debug dialogs must run on the SDL main thread");
    if (!show || dialog.buttons.empty() || dialog.buttons.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("invalid Debug dialog button count or platform call");

    SDL_MessageBoxFlags flags = SDL_MESSAGEBOX_INFORMATION;
    switch (dialog.type) {
    case DialogType::Info: break;
    case DialogType::Warning: flags = SDL_MESSAGEBOX_WARNING; break;
    case DialogType::Error: flags = SDL_MESSAGEBOX_ERROR; break;
    default: throw std::invalid_argument("invalid Debug dialog type");
    }
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.reserve(dialog.buttons.size());
    for (std::size_t index = 0; index < dialog.buttons.size(); ++index) {
        // Never bind Escape to a user button: closing the window must remain
        // distinguishable from clicking any label (including "取消").
        buttons.push_back({index == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0,
            static_cast<int>(index), dialog.buttons[index].c_str()});
    }
    SDL_MessageBoxData data{};
    data.flags = flags | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    data.window = nullptr;
    data.title = dialog.title.c_str();
    data.message = dialog.message.c_str();
    data.numbuttons = static_cast<int>(buttons.size());
    data.buttons = buttons.data();

    int selected = -1;
    if (!show(&data, &selected)) {
        const std::string error = std::string("SDL_ShowMessageBox failed: ") + SDL_GetError();
        core::runtime::Log::Error(error);
        throw std::runtime_error(error);
    }
    if (selected < -1 || selected >= data.numbuttons) {
        constexpr auto error = "SDL_ShowMessageBox returned an unknown button ID";
        core::runtime::Log::Error(error);
        throw std::runtime_error(error);
    }
    return selected;
}
}
