#pragma once
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace core::modules::Graphic::Text { struct GlyphBitmap; }
namespace core::modules::Graphic::Render::GlyphTextures {
namespace detail { struct State; }
struct Config {
    std::size_t maxEntries{4096};
    // Two RGBA8 texture payloads plus their live, row-aligned upload buffers.
    std::uint64_t maxBytes{128ull * 1024 * 1024};
};
struct Stats {
    std::size_t entries{}, transfers{}, encoded{};
    std::uint64_t bytes{}, hits{}, uploads{}, evictions{};
};
// A draw/fence pin. Destruction is CPU-only, including on worker threads.
// The immutable CPU bitmap survives until the last pin is released.
class Entry final {
public:
    ~Entry();
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;
    // Upload must precede sampling in the same CB, or have been committed on
    // this device queue. Unsubmitted uploads are never globally ready.
    SDL_GPUTexture* Fill(SDL_GPUCommandBuffer* = nullptr) const;
    SDL_GPUTexture* Ring(SDL_GPUCommandBuffer* = nullptr) const;
private:
    friend class Store;
    Entry(std::shared_ptr<detail::State>, std::shared_ptr<const Text::GlyphBitmap>);
    std::shared_ptr<detail::State> state_;
    std::shared_ptr<const Text::GlyphBitmap> source_;
};
using Lease = std::shared_ptr<const Entry>;
class Store final {
public:
    explicit Store(SDL_GPUDevice*, Config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // Empty glyphs must be skipped by the caller. One bitmap identity owns
    // one immutable pair; RGBA8 RGB=255/A=coverage, one level, linear+clamp.
    Lease Acquire(std::shared_ptr<const Text::GlyphBitmap>);
    SDL_GPUSampler* Sampler() const;
    // Owner thread, device-matching CB with no active pass. Several calls in
    // one CB are allowed. Any encoding failure requires cancelling that CB
    // before RollbackEncoded; a partially encoded pass is closed before throw.
    void EncodePending(SDL_GPUCommandBuffer*, std::span<const Lease>);
    // AFTER successful submit. This moves preallocated pins without allocating;
    // keep them AND every draw's leases until the submission fence completes.
    std::vector<Lease> CommitEncoded(SDL_GPUCommandBuffer*);
    // AFTER cancelling an unsubmitted CB. Unknown submission is terminal:
    // drain the device and Shutdown instead of pretending it was rolled back.
    void RollbackEncoded(SDL_GPUCommandBuffer*);
    void Reclaim();
    Stats GetStats() const;
    // Owner thread; cancel pending CBs and drain GPU work first. Existing
    // facades become invalid. Store never waits or submits command buffers.
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
