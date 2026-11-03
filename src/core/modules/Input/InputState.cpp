#include "InputState.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace core::modules::Input {
namespace {
constexpr unsigned triggerMask=0x3, leftStickMask=0x3c, rightStickMask=0x3c0;
void CheckDeadzone(double left,double right) {
    if(!std::isfinite(left) || !std::isfinite(right) || left<0 || right<0 || left>=1 || right>=1)
        throw std::invalid_argument("Input deadzones must be finite numbers in [0,1)");
}
void CheckThreshold(double press,double release) {
    if(!std::isfinite(press) || !std::isfinite(release) || release<0 || release>=press || press>1)
        throw std::invalid_argument("Input thresholds require finite 0 <= release < press <= 1");
}
double SignedAxis(std::int16_t value) { return value<0 ? double(value)/32768 : double(value)/32767; }
void Stick(double& x,double& y,std::int16_t rawX,std::int16_t rawY,double deadzone) {
    x=SignedAxis(rawX); y=-SignedAxis(rawY);
    const double radius=std::hypot(x,y);
    if(radius<=deadzone) { x=y=0; return; }
    const double gain=(std::min(radius,1.)-deadzone)/((1-deadzone)*radius);
    x*=gain; y*=gain;
}
void Latch(bool& held,double value,Threshold threshold) {
    if(held) { if(value<=threshold.release) held=false; }
    else if(value>=threshold.press) held=true;
}
}
void Wheel::Add(double x,double y) {
    if(focused_ && std::isfinite(x) && std::isfinite(y) && std::isfinite(x_+x) && std::isfinite(y_+y)) { x_+=x; y_+=y; }
}
void State::ClearMouse() { mouseValid_=false; mouseButtons_=0; mouseX_=mouseY_=0; wheelX_=wheelY_=0; }
void State::SetMouseEnabled(bool enabled) {
    mouseEnabled_=enabled;
    if(!enabled) ClearMouse(); // Re-enabling cannot resurrect this frame's old sample.
}
void State::ClearGamepad() {
    padButtons_.fill(false); padAxes_.fill(0); padLatches_.fill(false); resetLatches_=0;
}
void State::SetGamepadEnabled(bool enabled) {
    gamepadEnabled_=enabled;
    if(!enabled) ClearGamepad(); // Keep the physical connection, discard the published input.
}
void State::SetGamepadDeadzone(double left,double right) {
    CheckDeadzone(left,right);
    if(left!=stickDeadzone_.left) resetLatches_|=leftStickMask;
    if(right!=stickDeadzone_.right) resetLatches_|=rightStickMask;
    stickDeadzone_={left,right};
}
void State::SetGamepadTriggerDeadzone(double left,double right) {
    CheckDeadzone(left,right);
    if(left!=triggerDeadzone_.left) resetLatches_|=1;
    if(right!=triggerDeadzone_.right) resetLatches_|=2;
    triggerDeadzone_={left,right};
}
void State::SetGamepadStickButtonThreshold(double press,double release) {
    CheckThreshold(press,release);
    if(press!=stickThreshold_.press || release!=stickThreshold_.release) resetLatches_|=leftStickMask|rightStickMask;
    stickThreshold_={press,release};
}
void State::SetGamepadTriggerThreshold(double press,double release) {
    CheckThreshold(press,release);
    if(press!=triggerThreshold_.press || release!=triggerThreshold_.release) resetLatches_|=triggerMask;
    triggerThreshold_={press,release};
}
void State::InvalidateFocus() { keys_.fill(false); ClearMouse(); ClearGamepad(); }
void State::InvalidateGamepad(std::uint32_t id,bool disconnected) {
    if(id!=padId_) return;
    ClearGamepad();
    if(disconnected) padId_=0;
}
void State::Publish(const Sample& sample) {
    keys_.fill(false); ClearMouse();
    if(sample.focused) {
        std::copy_n(sample.keys.begin(),std::min(sample.keys.size(),keys_.size()),keys_.begin());
        keys_[SDL_SCANCODE_UNKNOWN]=false;
        if(mouseEnabled_ && sample.mouseFocused && std::isfinite(sample.mouseX) && std::isfinite(sample.mouseY) &&
            sample.mouseX>=0 && sample.mouseY>=0 && double(sample.mouseX)<sample.width && double(sample.mouseY)<sample.height) {
            mouseValid_=true;
            mouseX_=double(sample.mouseX)-double(sample.width)/2;
            mouseY_=double(sample.height)/2-double(sample.mouseY);
            mouseButtons_=sample.mouseButtons&0x1f;
            if(std::isfinite(sample.wheelX) && std::isfinite(sample.wheelY)) { wheelX_=sample.wheelX; wheelY_=sample.wheelY; }
        }
    }
    // Connection is independent of focus and configuration. Only input is gated.
    if(!sample.focused || !gamepadEnabled_ || !sample.gamepad.id || sample.gamepad.neutral || padId_!=sample.gamepad.id)
        ClearGamepad();
    padId_=sample.gamepad.id;
    if(!sample.focused || !gamepadEnabled_ || !padId_ || sample.gamepad.neutral) return;
    for(unsigned i=0;i<padLatches_.size();++i) if(resetLatches_&(1u<<i)) padLatches_[i]=false;
    resetLatches_=0;
    std::copy(sample.gamepad.buttons.begin(),sample.gamepad.buttons.end(),padButtons_.begin());
    const auto& raw=sample.gamepad.axes;
    Stick(padAxes_[0],padAxes_[1],raw[0],raw[1],stickDeadzone_.left);
    Stick(padAxes_[2],padAxes_[3],raw[2],raw[3],stickDeadzone_.right);
    for(int i=0;i<2;++i) {
        const double normalized=std::clamp(double(raw[SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i])/32767,0.,1.);
        const double deadzone=i==0?triggerDeadzone_.left:triggerDeadzone_.right;
        const double value=normalized<=deadzone?0.:(normalized-deadzone)/(1.-deadzone);
        padAxes_[SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i]=value;
        Latch(padLatches_[i],value,triggerThreshold_);
        const double x=padAxes_[i*2],y=padAxes_[i*2+1];
        const double components[]{y,-y,-x,x};
        for(int direction=0;direction<4;++direction)
            Latch(padLatches_[2+i*4+direction],components[direction],stickThreshold_);
    }
    std::copy(padLatches_.begin(),padLatches_.end(),padButtons_.begin()+SDL_GAMEPAD_BUTTON_COUNT);
}
bool State::Down(int code) const { return code>0 && code<SDL_SCANCODE_COUNT && keys_[code]; }
bool State::MouseDown(int button) const { return button>=1 && button<=5 && (mouseButtons_&(1u<<(button-1)))!=0; }
bool State::GamepadDown(int button) const { return button>=0 && button<int(padButtons_.size()) && padButtons_[button]; }
double State::GamepadAxis(int axis) const { return axis>=0 && axis<int(padAxes_.size()) ? padAxes_[axis]:0; }
}
