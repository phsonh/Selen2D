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
    // Called at the actual start of an engine frame, after waiting for GPU room.
    void BeginFrame(std::uint64_t now, bool invokesCallback);
    // Only this actual frame's budget matters; never carry a theoretical deadline.
    std::uint64_t Remaining(std::uint64_t now) const;
    double CurrentFPS() const;
    double ElapsedMilliseconds(std::uint64_t now) const;
    double DeltaMilliseconds() const { return deltaMs_; }
    std::uint64_t FrameCount() const { return callbacks_; }
private:
    std::uint64_t origin_{}, start_{}, callbacks_{}, budget_{16666667};
    double targetFPS_{60}, deltaMs_{};
    bool started_{};
    std::array<std::uint64_t,60> intervals_{};
    std::size_t samples_{}, cursor_{};
    long double total_{};
};
}
