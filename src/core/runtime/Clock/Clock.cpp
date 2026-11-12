#include "Clock.h"
#include <algorithm>
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
    // Changing cadence rebases the pending wait on the last actual start.
    // Repeating the same setting must not reintroduce cumulative wakeup drift.
    if(fps!=targetFPS_) phase_=start_;
    targetFPS_=fps;
    budget_=static_cast<std::uint64_t>(std::round(duration));
}
void Clock::BeginFrame(std::uint64_t now,bool invokesCallback) {
    if(now<origin_ || (started_ && now<start_)) throw std::logic_error("Runtime clock moved backwards");
    // At most 1 ms, and never more than one eighth of a period, may be recovered
    // by the following wait. Thus its interval cannot be shortened below 7/8
    // of a period by this clock. Stalls/overload discard the old phase entirely.
    // Differences avoid overflowing an absolute deadline near UINT64_MAX.
    const auto correction=std::min<std::uint64_t>(1'000'000,budget_/8);
    const auto elapsed=started_ ? now-phase_ : 0;
    if(started_ && elapsed>=budget_ && elapsed-budget_<=correction) phase_+=budget_;
    else phase_=now;
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
    const auto elapsed=now-phase_;
    return elapsed<budget_ ? budget_-elapsed:0;
}
double Clock::CurrentFPS() const { return total_>0 ? double(samples_*1'000'000'000.L/total_):0; }
double Clock::ElapsedMilliseconds(std::uint64_t now) const {
    if(now<origin_) throw std::logic_error("Runtime clock moved backwards");
    return double(now-origin_)/1'000'000.;
}
}
