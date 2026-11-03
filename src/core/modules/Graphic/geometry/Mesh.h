#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace core::modules::Graphic::Geometry {
// Fixed public Shader vertex ABI. ARGB is decoded to normalized RGBA at
// creation; these bytes can later be copied directly into a GPU vertex buffer.
struct MeshVertex {
    float position[3];
    float uv[2];
    float color[4];
};
struct MeshInputVertex {
    double x{}, y{}, z{}, u{}, v{};
    std::uint32_t color{0xFFFFFFFF};
};
struct MeshLimits {
    std::size_t maxVertices{1048576}, maxIndices{3145728};
};

// Immutable CPU data only. No Object, Lua, SDL, resource registry or GPU
// ownership. Renderer owns upload/retirement and pins this shared payload.
class MeshData final {
public:
    MeshData(const MeshData&) = delete;
    MeshData& operator=(const MeshData&) = delete;
    static std::shared_ptr<const MeshData> Create(std::span<const MeshInputVertex>,
        std::span<const std::uint32_t> zeroBasedIndices, MeshLimits = {});
    std::span<const MeshVertex> Vertices() const noexcept { return vertices_; }
    std::span<const std::uint32_t> Indices() const noexcept { return indices_; }
    std::size_t ByteSize() const noexcept;
private:
    MeshData(std::vector<MeshVertex>, std::vector<std::uint32_t>);
    std::vector<MeshVertex> vertices_;
    std::vector<std::uint32_t> indices_;
};
}
