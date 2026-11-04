#pragma once
#include "core/modules/Graphic/render/Image.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace core::modules::Graphic::Shader::ImageViews {
namespace detail { struct Gate; struct State; }
struct Config {
    std::size_t maxViews{256};
    std::uint64_t maxBytes{128ull * 1024 * 1024}; // Owned RGBA8 mip payloads only.
};
struct Stats {
    std::size_t views{}, owned{}, encoded{};
    std::uint64_t bytes{}, hits{}, copies{};
};
// A draw/fence pin. Its destruction never calls SDL, including on other
// threads. Both the private view and the immutable source backing remain alive.
class View final {
public:
    ~View();
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;
    std::uint32_t Levels() const noexcept;
    bool Borrowed() const noexcept;
    // Prepared views are not sampleable. Encoded (not yet submitted) views can
    // only be read by subsequent commands in that exact command buffer.
    SDL_GPUTexture* Native(SDL_GPUCommandBuffer* = nullptr) const;
private:
    friend class Store;
    View(std::shared_ptr<detail::State>, std::shared_ptr<const void> source);
    std::shared_ptr<detail::State> state_;
    std::shared_ptr<const void> source_;
};
using Lease = std::shared_ptr<const View>;
class Store final {
public:
    explicit Store(SDL_GPUDevice*, Config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // Metadata-only preparation + bounded GPU allocation, no copy submission,
    // pixel decoding, source readback or wait. Source must already be GPU-ready
    // Straight RGBA8; lifetime must identify the canonical immutable backing.
    // Fractional logical crop bounds are explicitly rejected: a hardware view
    // cannot preserve them without compiler-side sampling adaptation.
    Lease Acquire(const Render::Image&);
    // Caller owns a CB from this Store's device and guarantees no active pass.
    // Multiple calls in the same
    // CB are allowed; another CB is rejected until Commit/Rollback. Copies occur
    // before subsequent draws. No GenerateMipmaps is performed on an Atlas.
    void EncodeCopy(SDL_GPUCommandBuffer*, std::span<const Lease>);
    // Call only AFTER successful submission. Valid path is allocation-free;
    // move the returned pins into the submitted frame, retaining to its fence.
    // Ready means ordered on the SDL queue, not that the CPU waited for the GPU.
    std::vector<Lease> CommitEncoded(SDL_GPUCommandBuffer*);
    // Call only AFTER cancellation of an unsubmitted CB. Unknown/partial submit
    // is terminal: drain then Shutdown instead of pretending it was cancelled.
    void RollbackEncoded(SDL_GPUCommandBuffer*);
    void Reclaim(); // Expired-source, unpinned entries; capacity also evicts LRU unpinned entries.
    Stats GetStats() const;
    void Shutdown(); // Owner thread; cancel unsubmitted CBs and drain GPU work first.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
