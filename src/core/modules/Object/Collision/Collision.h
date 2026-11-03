#pragma once
namespace core::modules::Object::Collision {
// Value-only geometry. Circle radius=(a+b)/2; rectangle half-width/height=a/b.
struct Collider { double x{},y{},a{},b{},rotationDegrees{}; bool rectangle{}; };
// Tangency counts, degenerate boxes allowed. No ellipse, composite or laser.
// Invalid inputs/unrepresentable differences report errors, never NaN=false.
[[nodiscard]] bool Intersects(const Collider& first,const Collider& second);
}
