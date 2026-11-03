#pragma once
#include <SDL3/SDL_events.h>

namespace core::modules::Window {
struct Size { int width{}, height{}; };
// SDL window-coordinate units. Framebuffer pixels remain a renderer concern.
struct FrameMetrics {
    Size logicalSize;
    bool focused{}, minimized{}, hidden{};
};
// Pure state permits CPU tests without creating a window or acquiring a GPU.
class State final {
public:
    void HandleEvent(SDL_EventType type) {
        switch(type) {
        case SDL_EVENT_WINDOW_SHOWN: hidden_=false; minimized_=false; break;
        case SDL_EVENT_WINDOW_HIDDEN: hidden_=true; break;
        case SDL_EVENT_WINDOW_MINIMIZED: minimized_=true; break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED: minimized_=false; break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: fullscreen_=true; break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: fullscreen_=false; break;
        default: break;
        }
    }
    bool Minimized() const { return minimized_; }
    bool Fullscreen() const { return fullscreen_; }
    bool Hidden() const { return hidden_; }
    bool Published() const { return published_; }
    void Publish(Size size,bool focused) {
        frame_={size,focused,minimized_,hidden_}; published_=true;
    }
    FrameMetrics Frame() const { return frame_; }
private:
    bool minimized_{},fullscreen_{},hidden_{true},published_{};
    FrameMetrics frame_{};
};
}
