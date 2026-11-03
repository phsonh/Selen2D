#include "Mesh.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace core::modules::Graphic::Geometry {
static_assert(std::is_standard_layout_v<MeshVertex> && sizeof(MeshVertex) == 36);
static_assert(offsetof(MeshVertex, position) == 0 && offsetof(MeshVertex, uv) == 12 && offsetof(MeshVertex, color) == 20);
namespace {
float FiniteFloat(double value) {
    constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -limit || value > limit)
        throw std::invalid_argument("Mesh coordinates and UV must fit finite float32 values");
    return static_cast<float>(value);
}
}
MeshData::MeshData(std::vector<MeshVertex> vertices, std::vector<std::uint32_t> indices)
    : vertices_(std::move(vertices)), indices_(std::move(indices)) {}
std::shared_ptr<const MeshData> MeshData::Create(std::span<const MeshInputVertex> vertices,
    std::span<const std::uint32_t> indices, MeshLimits limits) {
    if (limits.maxVertices < 3 || limits.maxIndices < 3 ||
        limits.maxVertices > std::numeric_limits<std::uint32_t>::max() ||
        limits.maxIndices > std::numeric_limits<std::uint32_t>::max() ||
        limits.maxVertices > std::numeric_limits<std::size_t>::max() / sizeof(MeshVertex) ||
        limits.maxIndices > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        throw std::invalid_argument("invalid Mesh capacity limits");
    if (vertices.size() < 3 || indices.size() < 3 || indices.size() % 3)
        throw std::invalid_argument("Mesh requires at least three vertices and a nonempty triangle index list");
    if (vertices.size() > limits.maxVertices || indices.size() > limits.maxIndices ||
        indices.size() > (std::numeric_limits<std::size_t>::max() - vertices.size() * sizeof(MeshVertex)) / sizeof(std::uint32_t))
        throw std::length_error("Mesh vertex/index capacity exceeded");
    for (const auto index : indices)
        if (index >= vertices.size()) throw std::invalid_argument("Mesh index is outside the vertex array");
    std::vector<MeshVertex> copied;
    copied.reserve(vertices.size());
    for (const auto& input : vertices) {
        const auto color = input.color;
        copied.push_back({{FiniteFloat(input.x), FiniteFloat(input.y), FiniteFloat(input.z)},
            {FiniteFloat(input.u), FiniteFloat(input.v)},
            {float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255,
                float(color & 255) / 255, float(color >> 24) / 255}});
    }
    // No reference to either caller-owned array survives this construction.
    return std::shared_ptr<const MeshData>(new MeshData(std::move(copied),
        std::vector<std::uint32_t>(indices.begin(), indices.end())));
}
std::size_t MeshData::ByteSize() const noexcept {
    return vertices_.size() * sizeof(MeshVertex) + indices_.size() * sizeof(std::uint32_t);
}
}
