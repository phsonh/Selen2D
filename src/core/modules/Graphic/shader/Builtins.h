#pragma once
#include "Compiler.h"
#include "core/modules/Graphic/canvas/Canvas.h"
#include <cstdint>
#include <vector>

namespace core::modules::Graphic::Shader::Builtins {
struct Uniform {
    std::uint32_t slot{};
    std::vector<std::uint8_t> bytes;
};
// CPU-only snapshots for reflected EngineBuiltins buffers; does not include
// Params, own GPU resources, or alter Canvas/Camera/depth/history. Numeric and
// matrix inputs are validated even if this particular stage optimized them out.
// Canvas3D always validates through Geometry::MakeView, including fullscreen.
// All matrices use column-vector mathematics, stored as column-major float32.
std::vector<Uniform> Pack(const Canvas::Info&, const Geometry::SpatialState&, bool fullscreen, const Stage&);
}
