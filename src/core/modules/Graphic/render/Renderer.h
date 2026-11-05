#pragma once
#include "core/modules/Graphic/canvas/Canvas.h"
#include "Image.h"
#include "ShaderDraw.h"
#include "core/modules/Graphic/text/Text.h"
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
    std::uint64_t parameterBytes{}; // Built-in sprite sampling-buffer uploads.
    std::uint64_t indexBytes{}; // Built-in sprite triangle-index uploads.
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
    Canvas::Info GetTargetInfo(std::optional<Canvas::Handle> output = {}); // Requires an active frame, including for argument layout selection.
    Geometry::SpatialState GetSpatialState(Canvas::Handle) const;
    void SetSpatialState(Canvas::Handle, const Geometry::SpatialState&);
    void Clear(std::uint32_t argb, std::optional<Canvas::Handle> output = {}); // Input straight ARGB; Canvas storage is PMA.
    void RenderImage(const Image&, Transform, Paint = {}, std::optional<Canvas::Handle> output = {});
    void RenderImage(Canvas::Handle, Transform, Paint = {}, std::optional<Canvas::Handle> output = {});
    void RenderRect(const Image&, double left, double right, double bottom, double top, Paint = {}, std::optional<Canvas::Handle> output = {});
    void RenderRect(Canvas::Handle, double left, double right, double bottom, double top, Paint = {}, std::optional<Canvas::Handle> output = {});
    void RenderQuad(const Image&, const Geometry::Quad&, Paint = {}, std::optional<Canvas::Handle> output = {});
    void RenderQuad(Canvas::Handle, const Geometry::Quad&, Paint = {}, std::optional<Canvas::Handle> output = {});
    void Render4V(const Image&, const QuadVertices&, Blend = Blend::MulAlpha, double layer = 0, std::optional<Canvas::Handle> output = {});
    void Render4V(Canvas::Handle, const QuadVertices&, Blend = Blend::MulAlpha, double layer = 0, std::optional<Canvas::Handle> output = {});
    void RenderQuad3D(const Image&, const Geometry::Quad3&, std::uint32_t color = 0xFFFFFFFF, Blend = Blend::MulAlpha, std::optional<Canvas::Handle> output = {});
    void RenderQuad3D(Canvas::Handle, const Geometry::Quad3&, std::uint32_t color = 0xFFFFFFFF, Blend = Blend::MulAlpha, std::optional<Canvas::Handle> output = {});
    void Render4V3D(const Image&, const QuadVertices3&, Blend = Blend::MulAlpha, std::optional<Canvas::Handle> output = {});
    void Render4V3D(Canvas::Handle, const QuadVertices3&, Blend = Blend::MulAlpha, std::optional<Canvas::Handle> output = {});
    void RenderRibbon(const Image&, std::span<const Geometry::Point>, double width, Paint = {}, bool closed = false, std::optional<Canvas::Handle> output = {});
    void RenderRibbon(Canvas::Handle, std::span<const Geometry::Point>, double width, Paint = {}, bool closed = false, std::optional<Canvas::Handle> output = {});
    void RenderShader(ShaderDraw, std::optional<Canvas::Handle> output = {}); // Explicit values; no implicit current Object or shared material state.
    void RenderText(std::shared_ptr<const Text::LayoutResult>, double x, double y, std::uint32_t color,
        Blend blend, double layer, std::optional<Canvas::Handle> output = {});
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
