#include "InputState.h"
#include <algorithm>
#include <cmath>

namespace core::modules::Input {
namespace {
constexpr double deadzone=.2,triggerPress=.5,triggerRelease=.4;
double SignedAxis(std::int16_t value) { return value<0 ? double(value)/32768 : double(value)/32767; }
void Stick(double& x,double& y,std::int16_t rawX,std::int16_t rawY) {
    x=SignedAxis(rawX); y=-SignedAxis(rawY);
    const double radius=std::hypot(x,y);
    if(radius<=deadzone) { x=y=0; return; }
    const double gain=(std::min(radius,1.)-deadzone)/((1-deadzone)*radius);
    x*=gain; y*=gain;
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
void State::Publish(const Sample& sample) {
    keys_.fill(false); ClearMouse();
    if(sample.focused) {
        std::copy_n(sample.keys.begin(),std::min(sample.keys.size(),keys_.size()),keys_.begin());
        keys_[SDL_SCANCODE_UNKNOWN]=false;
        if(mouseEnabled_ && sample.mouseFocused && std::isfinite(sample.mouseX) && std::isfinite(sample.mouseY) &&
            sample.mouseX>=0 && sample.mouseY>=0 && double(sample.mouseX)<sample.width && double(sample.mouseY)<sample.height) {
            mouseValid_=true; mouseX_=sample.mouseX; mouseY_=sample.mouseY; mouseButtons_=sample.mouseButtons&0x1f;
            if(std::isfinite(sample.wheelX) && std::isfinite(sample.wheelY)) { wheelX_=sample.wheelX; wheelY_=sample.wheelY; }
        }
    }
    // Preserve trigger hysteresis only for the same connected, foreground device.
    if(!sample.focused || !sample.gamepad.id || padId_!=sample.gamepad.id) padButtons_.fill(false);
    padAxes_.fill(0); padId_=sample.gamepad.id;
    if(!sample.focused || !padId_) return;
    std::copy(sample.gamepad.buttons.begin(),sample.gamepad.buttons.end(),padButtons_.begin());
    const auto& raw=sample.gamepad.axes;
    Stick(padAxes_[0],padAxes_[1],raw[0],raw[1]);
    Stick(padAxes_[2],padAxes_[3],raw[2],raw[3]);
    for(int i=0;i<2;++i) {
        const double value=std::max(0.,double(raw[SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i])/32767);
        padAxes_[SDL_GAMEPAD_AXIS_LEFT_TRIGGER+i]=value;
        auto& held=padButtons_[leftTriggerButton+i];
        if(value>=triggerPress) held=true;
        else if(value<=triggerRelease) held=false;
    }
}
bool State::Down(int code) const { return code>0 && code<SDL_SCANCODE_COUNT && keys_[code]; }
bool State::MouseDown(int button) const { return button>=1 && button<=5 && (mouseButtons_&(1u<<(button-1)))!=0; }
bool State::GamepadDown(int button) const { return button>=0 && button<int(padButtons_.size()) && padButtons_[button]; }
double State::GamepadAxis(int axis) const { return axis>=0 && axis<int(padAxes_.size()) ? padAxes_[axis]:0; }
}
