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
    // Each host frame owns only its actual start and current budget. No previous
    // frame's deadline, oversleep or work overrun is carried into this frame.
    void BeginFrame(std::uint64_t now);
    // Call immediately before an actual Lua callback, after host preparation.
    // FPS/Delta/count describe callbacks, not the earlier host-frame timestamp.
    void BeginCallback(std::uint64_t now);
    // Zero means this frame has exhausted its budget; never wait another period.
    std::uint64_t Remaining(std::uint64_t now) const;
    double CurrentFPS() const;
    double ElapsedMilliseconds(std::uint64_t now) const;
    double DeltaMilliseconds() const { return deltaMs_; }
    std::uint64_t FrameCount() const { return callbacks_; }
private:
    std::uint64_t origin_{}, start_{}, callbackStart_{}, callbacks_{}, budget_{16666667};
    double targetFPS_{60}, deltaMs_{};
    bool started_{};
    std::array<std::uint64_t,60> intervals_{};
    std::size_t samples_{}, cursor_{};
    long double total_{};
};
}
