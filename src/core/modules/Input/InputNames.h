#pragma once
#include <span>
#include <string_view>
namespace core::modules::Input {
struct NamedInput { std::string_view name; int value; };
std::span<const NamedInput> KeyNames();
std::span<const NamedInput> GamepadButtonNames();
int KeyCode(std::string_view);
int MouseButton(std::string_view);
int GamepadButton(std::string_view);
int GamepadAxis(std::string_view);
}
