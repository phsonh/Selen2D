#pragma once
#include "Quad.h"
#include <array>
#include <cstdint>
#include <optional>

namespace core::modules::Graphic::Geometry {
using Point3=std::array<double,3>;
using Quad3=std::array<Point3,4>;
struct Camera {
    Point3 position{},look{1,0,0},up{0,1,0};
    bool operator==(const Camera&) const=default;
};
struct Perspective {
    double fov{45},nearPlane{1},farPlane{1000};
    bool operator==(const Perspective&) const=default;
};
enum class FogRGB { Lerp, Add, Sub, Mul, Screen };
enum class FogAlpha { Keep, Mul, Fade };
struct Fog {
    double start{},end{1};
    int mode{};
    std::uint32_t color{0xFFFFFFFF};
    FogRGB rgb{FogRGB::Lerp};
    FogAlpha alpha{FogAlpha::Keep};
    bool operator==(const Fog&) const=default;
};
struct SpatialState {
    Camera camera;
    Perspective perspective;
    std::optional<Fog> fog;
    bool depthEnabled{};
    bool operator==(const SpatialState&) const=default;
};
struct View {
    Point3 origin,right,up,forward;
    double sx{},sy{},depthScale{},nearPlane{};
};
struct Projected3 {
    std::array<double,4> clip;
    Point3 relative; // World-space vector from camera; interpolate before taking fog distance.
};
void ValidatePoint3(const Point3&);
void ValidateDirection(const Point3&); // Finite and nonzero only; pair validation occurs at drawing.
void ValidatePerspective(const Perspective&);
void ValidateFog(const Fog&);
View MakeView(const Camera&,const Perspective&,std::uint32_t width,std::uint32_t height);
Projected3 Project(const View&,const Point3&); // No divide by w; GPU clips near/far and behind-camera vertices.
Quad FlattenQuad(const Quad3&); // Strict convex, coplanar, either winding; local plane independent of camera.
double FogAmount(const Fog&,double distance); // Distance curve only; independent of color/alpha operations.
std::array<double,4> ApplyFog(const Fog&,double distance,std::array<double,4> premultiplied);
}
