#pragma once
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "core/modules/Graphic/geometry/Spatial.h"

// Native graphics facility. No Lua, Object, Resource registry or PNG dependency.
namespace core::modules::Graphic::Canvas {
struct Bounds { double left{}, right{}, bottom{}, top{}; };
struct Point { double x{}, y{}; };
Bounds Centered(std::uint32_t width, std::uint32_t height);
void Validate(Bounds);
Point ToClip(Bounds, Point); // Signed ranges allow explicit axis reversal.

struct Handle {
    std::uint64_t owner{}, generation{};
    std::uint32_t slot{};
    bool operator==(const Handle&) const = default;
};
struct Info {
    std::uint32_t width{}, height{};
    Bounds bounds;
    bool window{}, threeD{};
    std::uint32_t logicalWidth{}, logicalHeight{};
};
struct Config {
    std::size_t maxCanvases{256};
    std::uint32_t maxDimension{16384};
    std::uint64_t maxBytes{256ull * 1024 * 1024}; // Color + optional depth/distance; includes pending retirement.
};
struct Stats { std::size_t live{}, retired{}; std::uint64_t bytes{}; };

namespace detail {
// Accepted commands pin attachments, not public handles. Only the renderer may
// mark initialization after a successful GPU submission. No GPU calls in ~Attachment.
struct Attachment {
    Info info;
    SDL_GPUTexture* texture{};
    SDL_GPUTexture* depth{};
    SDL_GPUTexture* distance{}; // Canvas3D R32_FLOAT Euclidean distance, readable by shaders.
    SDL_GPUTextureFormat depthFormat{SDL_GPU_TEXTUREFORMAT_INVALID};
    Geometry::SpatialState spatial;
    std::uint64_t colorSerial{}, distanceSerial{}; // Queue-ordered content versions; committed by Renderer only.
    bool initialized{}, live{true};
};
}
class Store {
public:
    // Device/window are borrowed. All access, including destruction, is owner-thread only.
    // A null window permits an entirely offscreen renderer.
    explicit Store(SDL_GPUDevice*, SDL_Window* = nullptr, Config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Handle Create(std::uint32_t width, std::uint32_t height, std::optional<Bounds> = {});
    Handle Create3D(std::uint32_t width, std::uint32_t height);
    Handle GetWindowCanvas() const;
    Info Describe(Handle) const;
    void PublishWindowMetrics(std::uint32_t logicalWidth, std::uint32_t logicalHeight);
    bool Destroy(Handle); // Invalidates new access; already accepted commands keep their pin.
    std::shared_ptr<detail::Attachment> Acquire(Handle) const; // Internal renderer integration only.
    Stats GetStats() const;
    void Reclaim(); // Owner-thread deferred release, once no command/read pin remains.
    void Shutdown(); // Caller must first stop submitting work; invalidates every surviving pin.
private:
    Handle Allocate(std::uint32_t, std::uint32_t, std::optional<Bounds>, bool threeD);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
