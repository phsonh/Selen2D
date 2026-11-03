#pragma once
#include "core/modules/Graphic/canvas/Canvas.h"
#include "Image.h"
#include "ShaderDraw.h"
#include <span>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace core::modules::Graphic::Render {
struct Config {
    Canvas::Config canvases;
    std::size_t framesInFlight{3}, maxCommands{65536};
    std::uint32_t fenceTimeoutMs{30000};
    std::uint32_t maxVertexBytes{64 * 1024 * 1024};
    std::size_t maxShaderBytes{16 * 1024 * 1024}; // Per-frame immutable parameter/source snapshots.
    std::uint64_t maxReadbackBytes{256ull * 1024 * 1024}, maxPngBytes{256ull * 1024 * 1024};
};
struct Stats {
    std::uint64_t submittedFrames{}, completedFrames{}, renderPasses{}, clearPasses{}, preservedPasses{}, backpressure{};
    std::size_t inFlight{}, queuedCommands{};
    std::uint64_t drawCalls{}, sprites{}, textureBindings{}, bufferAllocations{}, vertexBytes{};
    std::uint64_t ribbons{};
    std::uint64_t readbacks{}, readbackSubmissions{};
};

// Native ordered frame and Image batch facility; not a Lua graphics API.
// The renderer owns no Object, Resource registry, Lua VM or application callback.
class Renderer {
public:
    explicit Renderer(SDL_GPUDevice*, SDL_Window* claimedWindow = nullptr, Config = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Canvas::Store& Canvases();
    // Nonblocking fence polling. False means bounded GPU-frame backpressure:
    // no render context was opened, and the application may continue updating.
    bool TryBeginFrame();
    void SetTargetCanvas(Canvas::Handle);
    Canvas::Handle GetTargetCanvas(); // Pure query, valid only inside an engine frame.
    Canvas::Info GetTargetInfo(); // Requires an active frame, including for argument layout selection.
    Geometry::SpatialState GetSpatialState(Canvas::Handle) const;
    void SetSpatialState(Canvas::Handle, const Geometry::SpatialState&);
    void Clear(std::uint32_t argb); // Input straight ARGB; Canvas storage is PMA.
    void RenderImage(const Image&, Transform, Paint = {});
    void RenderImage(Canvas::Handle, Transform, Paint = {});
    void RenderRect(const Image&, double left, double right, double bottom, double top, Paint = {});
    void RenderRect(Canvas::Handle, double left, double right, double bottom, double top, Paint = {});
    void RenderQuad(const Image&, const Geometry::Quad&, Paint = {});
    void RenderQuad(Canvas::Handle, const Geometry::Quad&, Paint = {});
    void Render4V(const Image&, const QuadVertices&, Blend = Blend::MulAlpha, double layer = 0);
    void Render4V(Canvas::Handle, const QuadVertices&, Blend = Blend::MulAlpha, double layer = 0);
    void RenderQuad3D(const Image&, const Geometry::Quad3&, std::uint32_t color = 0xFFFFFFFF, Blend = Blend::MulAlpha);
    void RenderQuad3D(Canvas::Handle, const Geometry::Quad3&, std::uint32_t color = 0xFFFFFFFF, Blend = Blend::MulAlpha);
    void Render4V3D(const Image&, const QuadVertices3&, Blend = Blend::MulAlpha);
    void Render4V3D(Canvas::Handle, const QuadVertices3&, Blend = Blend::MulAlpha);
    void RenderRibbon(const Image&, std::span<const Geometry::Point>, double width, Paint = {}, bool closed = false);
    void RenderRibbon(Canvas::Handle, std::span<const Geometry::Point>, double width, Paint = {}, bool closed = false);
    void RenderShader(ShaderDraw); // Explicit values; no implicit current Object or shared material state.
    std::optional<std::string> GetCanvasPNG(Canvas::Handle);
    bool HasAcquiredSwapchain() const; // Native presentation reconfiguration guard; not a Lua API.
    void EndFrame(); // Encode only after CPU recording succeeds, then submit.
    void AbortFrame(); // Discard unsubmitted tail. An explicit screenshot prefix cannot be undone.
    void Poll();
    Stats GetStats() const;
    bool Shutdown(); // Owner thread, idle wait only here; call before device destruction.
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
