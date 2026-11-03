#pragma once
#include <array>

namespace core::modules::Graphic::Geometry {
using Point=std::array<double,2>;
using Quad=std::array<Point,4>;
// Strict convex contour, either winding. Reject repeated/collinear/crossed or
// numerically unresolved edges. Geometry is normalized before relative checks.
void ValidateQuad(const Quad& positions);
// Solve destination -> UV homography. Returns positive, normalized denominator
// weights for interpolating (u*q,v*q,q). This is independent of GPU clip.w.
// Affine/constant/affine-1D UV is valid. Nonunique maps, poles within the convex
// domain or a denominator ratio below 1e-6 are rejected instead of triangulated.
std::array<double,4> ProjectiveWeights(const Quad& positions,const Quad& uv);
}
