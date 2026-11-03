#pragma once
#include "core/modules/Graphic/geometry/Mesh.h"
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace core::modules::Graphic::Shader::MeshBuffers {
namespace detail { struct State; }
struct Config {
    std::size_t maxMeshes{256};
    std::uint64_t maxBytes{256ull * 1024 * 1024}; // Vertex/index buffers + live upload transfer payloads.
};
struct Stats {
    std::size_t meshes{}, transfers{}, encoded{};
    std::uint64_t bytes{}, hits{}, copies{}, evictions{};
};
// Frame/fence pin. Its destructor only releases CPU ownership, never SDL.
// The immutable CPU source survives at least until this lease is released.
class View final {
public:
    ~View();
    std::uint32_t VertexCount() const noexcept;
    std::uint32_t IndexCount() const noexcept;
    // Prepared is not drawable. Encoded is usable only later in the same CB;
    // after successful Commit it is queue-ordered, without a CPU GPU wait.
    SDL_GPUBuffer* Vertex(SDL_GPUCommandBuffer* = nullptr) const;
    SDL_GPUBuffer* Index(SDL_GPUCommandBuffer* = nullptr) const;
private:
    friend class Store;
    View(std::shared_ptr<detail::State>, std::shared_ptr<const Geometry::MeshData>);
    std::shared_ptr<detail::State> state_;
    std::shared_ptr<const Geometry::MeshData> source_;
};
using Lease = std::shared_ptr<const View>;
class Store final {
public:
    explicit Store(SDL_GPUDevice*, Config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // One immutable Mesh owns one vertex/index pair. No per-draw buffers.
    // Bounded allocation only; source memcpy occurs before EncodeCopy's pass.
    Lease Acquire(std::shared_ptr<const Geometry::MeshData>);
    void EncodeCopy(SDL_GPUCommandBuffer*, std::span<const Lease>);
    // AFTER successful submit: allocation-free move of preallocated pins to
    // the frame's fence lifetime. Unknown/partial submission is terminal.
    std::vector<Lease> CommitEncoded(SDL_GPUCommandBuffer*);
    // AFTER cancelling the unsubmitted CB; permits safe copy retry.
    void RollbackEncoded(SDL_GPUCommandBuffer*);
    void Reclaim(); // Owner thread; release completed unpinned transfer buffers and dead-source entries.
    Stats GetStats() const;
    void Shutdown(); // Owner thread; caller drains GPU first. Retained facades become invalid.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
