#pragma once
#include "core/modules/Graphic/canvas/Canvas.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace core::modules::Graphic::Shader::CanvasViews {
enum class Kind { Color, Distance };
struct Config { std::size_t maxViews{256}; std::uint64_t maxBytes{128ull * 1024 * 1024}; };
struct Stats {
    std::size_t views{}, encoded{};
    std::uint64_t bytes{}, hits{}, conversions{}, copies{};
};
namespace detail { struct Gate; struct State; }
// Holds source attachments and a private texture through a draw/fence. Its
// destructor is CPU-only; Store performs all GPU retirement on its owner thread.
class View final {
public:
    ~View();
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;
    Kind Type() const noexcept;
    // After Encode, pending contents may only be used later in that exact CB.
    // A committed view is ordered on the GPU queue; no CPU wait is required.
    SDL_GPUTexture* Native(SDL_GPUCommandBuffer* = nullptr) const;
private:
    friend class Store;
    View(std::shared_ptr<detail::State>, std::shared_ptr<Canvas::detail::Attachment>);
    std::shared_ptr<detail::State> state_;
    std::shared_ptr<Canvas::detail::Attachment> source_;
};
using Lease = std::shared_ptr<const View>;
class Store final {
public:
    explicit Store(SDL_GPUDevice*, Config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Lease Acquire(const std::shared_ptr<Canvas::detail::Attachment>&, Kind);
    // Caller supplies a CB from this device with no active pass; source has
    // already been initialized/written BEFORE this call on that same queue.
    // contentSerial must be nonzero and identify the source pixels now visible.
    // initialized/live are not rechecked here: same-CB initialization or already
    // accepted work after Destroy must remain legal. Shutdown-null pointers fail.
    // Repeated serials do no work. A new serial rewrites the SAME private texture
    // with cycle=false, preserving the order of previous draws in this/older CBs.
    void Encode(SDL_GPUCommandBuffer*, const Lease&, std::uint64_t contentSerial);
    // Successful-submit path allocates nothing. Retain returned source/view pins
    // until that submission's fence. Renderer also pins every view used by draws.
    std::vector<Lease> CommitEncoded(SDL_GPUCommandBuffer*);
    // Only after cancelling an unsubmitted CB. Restores old committed version.
    // Unknown/partial submission is terminal: drain and Shutdown, never rollback.
    void RollbackEncoded(SDL_GPUCommandBuffer*);
    void Reclaim();
    Stats GetStats() const;
    void Shutdown(); // Cancel unsubmitted CBs and drain GPU work first.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
