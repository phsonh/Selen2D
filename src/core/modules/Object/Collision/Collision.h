#pragma once
namespace core::modules::Object::Collision {
// Value-only geometry. Circle radius=(a+b)/2; rectangle half-width/height=a/b.
struct Collider { double x{},y{},a{},b{},rotationDegrees{}; bool rectangle{}; };
// Tangency counts, degenerate boxes allowed. No ellipse, composite or laser.
// Invalid inputs/unrepresentable differences report errors, never NaN=false.
[[nodiscard]] bool Intersects(const Collider& first,const Collider& second);
// Optional per-dispatch broad phase. Negative means that Intersects must decide,
// including validation errors; preparing a bound never validates by throwing.
[[nodiscard]] double ConservativeRadius(const Collider&) noexcept;
// Only use with nonnegative radii prepared before any user callback. The margin
// covers Intersects' normalized contact tolerance, not a new geometry tolerance.
[[nodiscard]] bool Separated(double dx,double dy,double firstRadius,double secondRadius) noexcept;
}
