#pragma once
#include "core/modules/Graphic/shader/Program.h"
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace core::modules::Graphic::backend {
namespace detail { struct ShaderPipelineState; }
// Renderer-private PSO cache. Leases pin their Program through submission and
// the frame fence; Program ownership/final release is render-owner-thread only.
class ShaderPipeline final {
public:
    struct Config { std::size_t maxPipelines{256}; };
    struct Stats { std::size_t entries{}; std::uint64_t hits{}, created{}, evictions{}; };
    class View final {
    public:
        ~View();
        SDL_GPUGraphicsPipeline* Native() const;
    private:
        friend class ShaderPipeline;
        View(std::shared_ptr<detail::ShaderPipelineState>, std::shared_ptr<const Shader::Program>);
        std::shared_ptr<detail::ShaderPipelineState> state_;
        std::shared_ptr<const Shader::Program> program_;
    };
    using Lease = std::shared_ptr<const View>;
    explicit ShaderPipeline(SDL_GPUDevice*);
    ShaderPipeline(SDL_GPUDevice*, Config);
    ~ShaderPipeline();
    ShaderPipeline(const ShaderPipeline&) = delete;
    ShaderPipeline& operator=(const ShaderPipeline&) = delete;
    Lease Get(std::shared_ptr<const Shader::Program>, SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat, bool depthEnabled, bool fullscreen);
    void Reclaim();
    Stats GetStats() const;
    // Caller drains GPU work first. Shutdown cache before ProgramStore/device;
    // retained leases become unusable, while their Program CPU pins remain safe.
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
