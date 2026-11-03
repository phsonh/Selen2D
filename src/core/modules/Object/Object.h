#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace core::modules::Object {
// Logical entity only. Visual resources, sorting and tasks belong to Lua/Graphic.
struct Object {
    static constexpr std::int32_t maximum_timer=std::numeric_limits<std::int32_t>::max();
    struct Transform { double x{},y{},rot{}; } transform;
    struct Motion { double vx{},vy{},ax{},ay{},rate{1}; } motion;
    struct Scheduling { bool update{true},render{true}; } scheduling;
    struct Collision {
        double a{},b{};
        std::optional<std::int32_t> group;
        bool enabled{true},rectangle{};
    } collision;
    enum class Status : std::uint8_t { pendingActivation,active,pendingDelete };
    struct Lifecycle {
        std::int32_t timer{};
        Status status{Status::pendingActivation};
        bool auto_bound{true};
    } lifecycle;
    void reset() noexcept;
};
}
