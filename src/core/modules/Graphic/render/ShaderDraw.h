#pragma once
#include "Image.h"
#include "core/modules/Graphic/canvas/Canvas.h"
#include "core/modules/Graphic/geometry/Mesh.h"
#include "core/modules/Graphic/shader/Program.h"
#include <variant>
#include <vector>

namespace core::modules::Graphic::Render {
// Value snapshots at the native submission boundary. Lua/Resource resolution is
// outside Renderer; it knows only immutable Images, Canvas handles and Program.
enum class TexturePart { Color, Depth, Distance };
using ShaderSource=std::variant<std::monostate,Image,Canvas::Handle>;
struct ShaderUniform { std::uint32_t slot{}; std::vector<std::uint8_t> bytes; };
struct ShaderTexture { std::uint32_t slot{}; std::size_t source{}; TexturePart part{TexturePart::Color}; };
struct ShaderStageData { std::vector<ShaderUniform> params; std::vector<ShaderTexture> textures; };
struct ShaderDraw {
    std::shared_ptr<const Shader::Program> program;
    std::shared_ptr<const Geometry::MeshData> mesh; // Null = one oversized clip-space triangle.
    std::vector<ShaderSource> sources;
    ShaderStageData vertex,fragment;
    double layer{}; // 2D only; Canvas3D preserves submission order.
};
}
