#pragma once

#include <string_view>

namespace core::runtime::Console {
// The application host owns this service. It never attaches to the caller's
// terminal or redirects CRT/Win32 standard streams to the diagnostic console.
void SetTitle(std::string_view title);
void SetVisible(bool visible);
bool IsVisible() noexcept;
void Write(std::string_view text) noexcept;
void Shutdown() noexcept;
}
