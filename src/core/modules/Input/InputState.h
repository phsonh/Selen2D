#pragma once
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <array>
#include <cstdint>
#include <span>

namespace core::modules::Input {
constexpr int leftTriggerButton=SDL_GAMEPAD_BUTTON_COUNT;
constexpr int rightTriggerButton=SDL_GAMEPAD_BUTTON_COUNT+1;
struct GamepadSample {
    std::uint32_t id{}; // Internal identity, never a public Lua slot.
    std::array<bool,SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<std::int16_t,SDL_GAMEPAD_AXIS_COUNT> axes{};
};
struct Sample {
    std::span<const bool> keys;
    bool focused{}, mouseFocused{};
    int width{},height{};
    float mouseX{},mouseY{};
    std::uint32_t mouseButtons{};
    double wheelX{},wheelY{};
    GamepadSample gamepad;
};
// Collection is separate from publication. Begin/Pump never edit Lua's snapshot.
class Wheel final {
public:
    void Begin(bool focused) { x_=y_=0; focused_=focused; }
    void Focus(bool focused) { focused_=focused; if(!focused) Clear(); }
    void Clear() { x_=y_=0; }
    void Add(double x,double y);
    double X() const { return x_; }
    double Y() const { return y_; }
private:
    double x_{},y_{};
    bool focused_{};
};
class State final {
public:
    void Reset() { *this=State{}; }
    void Publish(const Sample&);
    void SetMouseEnabled(bool enabled);
    bool MouseEnabled() const { return mouseEnabled_; }
    bool Down(int scancode) const;
    bool MouseValid() const { return mouseValid_; }
    bool MouseDown(int button) const;
    float MouseX() const { return mouseX_; }
    float MouseY() const { return mouseY_; }
    double WheelX() const { return wheelX_; }
    double WheelY() const { return wheelY_; }
    bool GamepadDown(int button) const;
    double GamepadAxis(int axis) const;
private:
    void ClearMouse();
    std::array<bool,SDL_SCANCODE_COUNT> keys_{};
    std::array<bool,SDL_GAMEPAD_BUTTON_COUNT+2> padButtons_{};
    std::array<double,SDL_GAMEPAD_AXIS_COUNT> padAxes_{};
    std::uint32_t padId_{},mouseButtons_{};
    bool mouseEnabled_{true},mouseValid_{};
    float mouseX_{},mouseY_{};
    double wheelX_{},wheelY_{};
};
}
