#pragma once
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <array>
#include <cstdint>
#include <span>

namespace core::modules::Input {
constexpr int leftTriggerButton=SDL_GAMEPAD_BUTTON_COUNT;
constexpr int rightTriggerButton=SDL_GAMEPAD_BUTTON_COUNT+1;
constexpr int leftStickUpButton=SDL_GAMEPAD_BUTTON_COUNT+2;
constexpr int leftStickDownButton=SDL_GAMEPAD_BUTTON_COUNT+3;
constexpr int leftStickLeftButton=SDL_GAMEPAD_BUTTON_COUNT+4;
constexpr int leftStickRightButton=SDL_GAMEPAD_BUTTON_COUNT+5;
constexpr int rightStickUpButton=SDL_GAMEPAD_BUTTON_COUNT+6;
constexpr int rightStickDownButton=SDL_GAMEPAD_BUTTON_COUNT+7;
constexpr int rightStickLeftButton=SDL_GAMEPAD_BUTTON_COUNT+8;
constexpr int rightStickRightButton=SDL_GAMEPAD_BUTTON_COUNT+9;
constexpr int gamepadButtonCount=SDL_GAMEPAD_BUTTON_COUNT+10;
struct Pair { double left, right; };
struct Threshold { double press, release; };
struct GamepadSample {
    std::uint32_t id{}; // Internal identity, never a public Lua slot.
    bool neutral{}; // A remap can invalidate input without disconnecting the device.
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
// Collection is separate from publication; invalidation may only neutralize it.
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
    void SetGamepadEnabled(bool enabled);
    void SetGamepadDeadzone(double left, double right);
    void SetGamepadTriggerDeadzone(double left, double right);
    void SetGamepadStickButtonThreshold(double press, double release);
    void SetGamepadTriggerThreshold(double press, double release);
    bool MouseEnabled() const { return mouseEnabled_; }
    bool GamepadEnabled() const { return gamepadEnabled_; }
    bool GamepadConnected() const { return padId_!=0; }
    Pair GamepadDeadzone() const { return stickDeadzone_; }
    Pair GamepadTriggerDeadzone() const { return triggerDeadzone_; }
    Threshold GamepadStickButtonThreshold() const { return stickThreshold_; }
    Threshold GamepadTriggerThreshold() const { return triggerThreshold_; }
    void InvalidateFocus();
    void InvalidateMouse() { ClearMouse(); }
    void InvalidateGamepad(std::uint32_t id, bool disconnected);
    bool Down(int scancode) const;
    bool MouseValid() const { return mouseValid_; }
    bool MouseDown(int button) const;
    double MouseX() const { return mouseX_; }
    double MouseY() const { return mouseY_; }
    double WheelX() const { return wheelX_; }
    double WheelY() const { return wheelY_; }
    bool GamepadDown(int button) const;
    double GamepadAxis(int axis) const;
private:
    void ClearMouse();
    void ClearGamepad();
    std::array<bool,SDL_SCANCODE_COUNT> keys_{};
    std::array<bool,gamepadButtonCount> padButtons_{};
    std::array<bool,10> padLatches_{}; // Triggers, then four directions per stick.
    std::array<double,SDL_GAMEPAD_AXIS_COUNT> padAxes_{};
    std::uint32_t padId_{},mouseButtons_{};
    unsigned resetLatches_{};
    Pair stickDeadzone_{.2,.2},triggerDeadzone_{.05,.05};
    Threshold stickThreshold_{.5,.4},triggerThreshold_{.5,.4};
    bool mouseEnabled_{true},gamepadEnabled_{true},mouseValid_{};
    double mouseX_{},mouseY_{};
    double wheelX_{},wheelY_{};
};
}
