#include "Clock.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace core::runtime {
void Clock::Reset(std::uint64_t now) {
    *this=Clock{};
    origin_=now;
}
void Clock::SetTargetFPS(double fps) {
    if(!std::isfinite(fps) || fps<=0 || fps>1000)
        throw std::invalid_argument("Runtime target FPS must be finite and in (0,1000]");
    const long double duration=1'000'000'000.L/fps;
    if(duration>static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Runtime target FPS has an unrepresentable frame interval");
    targetFPS_=fps;
    budget_=static_cast<std::uint64_t>(std::round(duration));
}
void Clock::BeginFrame(std::uint64_t now,bool invokesCallback) {
    if(now<origin_ || (started_ && now<start_)) throw std::logic_error("Runtime clock moved backwards");
    start_=now; started_=true;
    if(invokesCallback) {
        if(callbacks_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Runtime frame counter overflow");
        if(callbacks_) {
            const auto interval=now-callbackStart_;
            total_-=intervals_[cursor_]; total_+=interval;
            intervals_[cursor_]=interval; cursor_=(cursor_+1)%intervals_.size();
            if(samples_<intervals_.size()) ++samples_;
            deltaMs_=double(interval)/1'000'000.;
        }
        callbackStart_=now;
        ++callbacks_;
    }
}
std::uint64_t Clock::Remaining(std::uint64_t now) const {
    if(!started_) return 0;
    if(now<start_) throw std::logic_error("Runtime clock moved backwards");
    const auto elapsed=now-start_;
    return elapsed<budget_ ? budget_-elapsed:0;
}
double Clock::CurrentFPS() const { return total_>0 ? double(samples_*1'000'000'000.L/total_):0; }
double Clock::ElapsedMilliseconds(std::uint64_t now) const {
    if(now<origin_) throw std::logic_error("Runtime clock moved backwards");
    return double(now-origin_)/1'000'000.;
}
}
