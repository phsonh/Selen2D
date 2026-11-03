#pragma once
#include "Quad.h"
#include <span>
#include <vector>

namespace core::modules::Graphic::Geometry {
struct RibbonVertex { Point position; double u{},v{}; };
// Reusable owner-thread scratch. No Lua, Image, Canvas or GPU dependencies.
// Output is a triangle list: u follows arc length, v=0 left / v=1 right.
class RibbonBuilder final {
public:
    static constexpr std::size_t maximumPoints=16384;
    static constexpr double miterLimit=4;
    std::span<const RibbonVertex> Build(std::span<const Point>,double width,bool closed);
private:
    struct Section { Point left,right; };
    struct Join { Section incoming,outgoing; bool bevel{}; bool leftTurn{}; };
    std::vector<Point> points_,tangents_;
    std::vector<double> distance_;
    std::vector<Join> joins_;
    std::vector<RibbonVertex> vertices_;
};
}
