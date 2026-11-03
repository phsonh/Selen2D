#include "Time.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace core::modules::Time {
namespace {
constexpr double maximumInteger=9007199254740991.;
void Finite(double value) {
    if(!std::isfinite(value)) throw std::invalid_argument("Time values must be finite");
}
void Frequency(double fps) {
    if(!std::isfinite(fps) || fps<=0) throw std::invalid_argument("Time conversion FPS must be finite and positive");
}
}
Date GetDate() {
    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    if(localtime_s(&local,&now)!=0) throw std::runtime_error("Cannot read local calendar time");
#else
    if(!localtime_r(&now,&local)) throw std::runtime_error("Cannot read local calendar time");
#endif
    return {local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec};
}
double FramesToSeconds(double frames,double fps) {
    Finite(frames); Frequency(fps);
    const double result=frames/fps;
    if(!std::isfinite(result)) throw std::overflow_error("Time conversion exceeds numeric range");
    return result;
}
double SecondsToFrames(double seconds,double fps) {
    Finite(seconds); Frequency(fps);
    const double result=std::round(seconds*fps); // Halfway values round away from zero on either side.
    if(!std::isfinite(result) || std::abs(result)>maximumInteger) throw std::overflow_error("Frame count exceeds Lua exact integer range");
    return result;
}
}
