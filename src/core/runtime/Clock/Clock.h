#pragma once
#include <array>
#include <cstdint>

namespace core::runtime {
// Host timing only: no SDL, Lua, Object or renderer dependency. Explicit timestamps
// make pacing verifiable without sleeping. All access belongs to the host thread.
class Clock final {
public:
    void Reset(std::uint64_t now);
    void SetTargetFPS(double fps);
    double TargetFPS() const { return targetFPS_; }
    // Actual frame start, after preparation and pacing. Measurements always use
    // now; only the pacing anchor can retain a bounded amount of lateness.
    void BeginFrame(std::uint64_t now, bool invokesCallback);
    // Small scheduling errors can be recovered; an overdue cadence is discarded
    // at BeginFrame instead of carrying a backlog of logical frames.
    std::uint64_t Remaining(std::uint64_t now) const;
    double CurrentFPS() const;
    double ElapsedMilliseconds(std::uint64_t now) const;
    double DeltaMilliseconds() const { return deltaMs_; }
    std::uint64_t FrameCount() const { return callbacks_; }
private:
    std::uint64_t origin_{}, start_{}, phase_{}, callbackStart_{}, callbacks_{}, budget_{16666667};
    double targetFPS_{60}, deltaMs_{};
    bool started_{};
    std::array<std::uint64_t,60> intervals_{};
    std::size_t samples_{}, cursor_{};
    long double total_{};
};
}
