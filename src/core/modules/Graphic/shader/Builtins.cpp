#include "Builtins.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace core::modules::Graphic::Shader::Builtins {
namespace {
using Matrix = std::array<double, 16>; // Row-major mathematical working storage.
using PackedMatrix = std::array<float, 16>;
[[noreturn]] void Error(std::string_view name, std::string_view message) {
    throw std::invalid_argument("Shader builtin " + std::string(name) + ": " + std::string(message));
}
double Finite(double value, std::string_view name) {
    if (!std::isfinite(value)) Error(name, "matrix arithmetic exceeds finite double range");
    return value;
}
float Float(double value, std::string_view name) {
    if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max())
        Error(name, "value exceeds finite GPU float32 range");
    return static_cast<float>(value);
}
Matrix Identity() {
    return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}
Matrix CanvasMatrix(const Canvas::Bounds& b) {
    // Canvas::Validate resides with the GPU Store implementation. Keep this
    // value component independent: the same signed, finite range rules apply.
    if (!std::isfinite(b.left) || !std::isfinite(b.right) || !std::isfinite(b.bottom) || !std::isfinite(b.top) ||
        !std::isfinite(b.right-b.left) || !std::isfinite(b.top-b.bottom) || b.left == b.right || b.bottom == b.top)
        Error("EngineCanvasToClip", "Canvas bounds must be finite nonzero ranges");
    auto result = Identity();
    const auto width = b.right-b.left, height = b.top-b.bottom;
    result[0] = Finite(2/width, "EngineCanvasToClip");
    result[5] = Finite(2/height, "EngineCanvasToClip");
    // Avoid left+right or 2*left overflowing before division when the final
    // affine offset is representable. Reversed ranges deliberately retain sign.
    result[3] = Finite(-1-2*(b.left/width), "EngineCanvasToClip");
    result[7] = Finite(-1-2*(b.bottom/height), "EngineCanvasToClip");
    return result;
}
double Dot(const Geometry::Point3& a, const Geometry::Point3& b) {
    return Finite(std::fma(a[0], b[0], std::fma(a[1], b[1], a[2]*b[2])), "EngineView");
}
Matrix ViewMatrix(const Geometry::View& view) {
    // Reuse MakeView's orthonormal basis. No second camera/up normalization.
    return {view.right[0],view.right[1],view.right[2],-Dot(view.right,view.origin),
        view.up[0],view.up[1],view.up[2],-Dot(view.up,view.origin),
        -view.forward[0],-view.forward[1],-view.forward[2],Dot(view.forward,view.origin),
        0,0,0,1};
}
Matrix ProjectionMatrix(const Geometry::View& view) {
    // Right-handed view points forward along -Z. Project's positive distance
    // z=dot(point-origin,forward) is therefore -viewPosition.z.
    return {view.sx,0,0,0, 0,view.sy,0,0,
        0,0,-view.depthScale,Finite(-view.nearPlane*view.depthScale, "EngineProjection"),
        0,0,-1,0};
}
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
        double value{};
        for (std::size_t k = 0; k < 4; ++k)
            value = Finite(std::fma(a[row*4+k], b[k*4+column], value), "EngineViewProjection");
        result[row*4+column] = value;
    }
    return result;
}
PackedMatrix Columns(const Matrix& matrix, std::string_view name) {
    PackedMatrix result{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column)
        result[column*4+row] = Float(matrix[row*4+column], name);
    return result;
}
struct Values {
    PackedMatrix view, projection, combined, canvas;
    std::array<float,2> size, inverse;
    std::uint32_t fullscreen;
};
Values Build(const Canvas::Info& info, const Geometry::SpatialState& spatial, bool fullscreen) {
    if (!info.width || !info.height) Error("EngineTargetSize", "target dimensions must be positive");
    auto view = Identity(), projection = Identity();
    if (info.threeD) {
        // Fullscreen only changes the generated vertex source. It neither skips
        // Camera validation nor modifies its world-space matrices or history.
        const auto source = Geometry::MakeView(spatial.camera, spatial.perspective, info.width, info.height);
        view = ViewMatrix(source);
        projection = ProjectionMatrix(source);
    }
    const auto combined = Multiply(projection, view);
    return {Columns(view,"EngineView"), Columns(projection,"EngineProjection"),
        Columns(combined,"EngineViewProjection"), Columns(CanvasMatrix(info.bounds),"EngineCanvasToClip"),
        {Float(info.width,"EngineTargetSize"),Float(info.height,"EngineTargetSize")},
        {Float(1./info.width,"EngineTargetInvSize"),Float(1./info.height,"EngineTargetInvSize")},
        fullscreen?1u:0u};
}
struct Source {
    ValueType type;
    std::span<const std::uint8_t> bytes;
};
template<class T> Source Data(ValueType type, const T& value) {
    return {type, {reinterpret_cast<const std::uint8_t*>(&value),sizeof(value)}};
}
Source Lookup(const Values& values, std::string_view name) {
    if (name == "EngineView") return Data(ValueType::Matrix4, values.view);
    if (name == "EngineProjection") return Data(ValueType::Matrix4, values.projection);
    if (name == "EngineViewProjection") return Data(ValueType::Matrix4, values.combined);
    if (name == "EngineCanvasToClip") return Data(ValueType::Matrix4, values.canvas);
    if (name == "EngineTargetSize") return Data(ValueType::Float2, values.size);
    if (name == "EngineTargetInvSize") return Data(ValueType::Float2, values.inverse);
    if (name == "EngineIsFullscreen") return Data(ValueType::UInt, values.fullscreen);
    Error(name, "unknown reflected builtin");
}
}
std::vector<Uniform> Pack(const Canvas::Info& info, const Geometry::SpatialState& spatial, bool fullscreen, const Stage& stage) {
    const auto values = Build(info, spatial, fullscreen);
    std::vector<Uniform> result;
    std::unordered_set<std::uint32_t> slots;
    for (const auto& buffer : stage.buffers) {
        if (!slots.insert(buffer.slot).second || buffer.slot >= 4) Error("buffer", "invalid reflected uniform slots");
        if (buffer.name != "EngineBuiltins") continue;
        if (!result.empty()) Error("buffer", "duplicate EngineBuiltins buffer");
        if (!buffer.size || buffer.size > 64*1024) Error("buffer", "invalid reflected builtin buffer size");
        Uniform output{buffer.slot,std::vector<std::uint8_t>(buffer.size)};
        std::unordered_set<std::string> names;
        for (const auto& field : buffer.fields) {
            if (!names.insert(field.name).second) Error(field.name, "duplicate reflected field");
            const auto source = Lookup(values, field.name);
            if (field.type != source.type || field.size != source.bytes.size() ||
                field.offset > buffer.size || field.size > buffer.size-field.offset)
                Error(field.name, "invalid reflected type or extent");
            if (field.used) std::memcpy(output.bytes.data()+field.offset, source.bytes.data(), source.bytes.size());
        }
        result.push_back(std::move(output));
    }
    return result;
}
}
