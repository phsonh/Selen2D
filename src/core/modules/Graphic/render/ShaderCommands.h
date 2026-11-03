#pragma once
#include "ShaderDraw.h"
#include "core/modules/Graphic/backend/ShaderPipeline.h"
#include "core/modules/Graphic/backend/ImagePipeline.h"
#include "core/modules/Graphic/shader/ImageViews.h"
#include "core/modules/Graphic/shader/CanvasViews.h"
#include "core/modules/Graphic/shader/MeshBuffers.h"
#include <functional>

namespace core::modules::Graphic::Render::detail {
// Private native draw preparation. Neither resource names, Lua, Object nor
// scene policy enter this component. Prepared draws own all in-flight leases.
struct ShaderCommand {
    ShaderDraw draw;
    Canvas::Info info;
    std::vector<std::shared_ptr<Canvas::detail::Attachment>> canvases;
    std::vector<Shader::ImageViews::Lease> images;
    std::vector<Shader::CanvasViews::Lease> colors;
    Shader::CanvasViews::Lease history;
    Shader::MeshBuffers::Lease mesh;
    backend::ShaderPipeline::Lease pipeline;
    std::vector<ShaderUniform> vertexUniforms, fragmentUniforms;
    std::vector<SDL_GPUTextureSamplerBinding> vertexTextures, fragmentTextures;
};
class ShaderCommands {
public:
    explicit ShaderCommands(SDL_GPUDevice*);
    ~ShaderCommands();
    void Prepare(ShaderCommand&, const Geometry::SpatialState&,
        const std::shared_ptr<Canvas::detail::Attachment>& target, SDL_GPUTextureFormat);
    // No active pass. Upload immutable inputs, then snapshot mutable Canvas
    // colors/history exactly here in ordered command execution.
    void EncodeInputs(SDL_GPUCommandBuffer*, ShaderCommand&,
        const std::function<std::uint64_t(Canvas::detail::Attachment*,bool distance)>& serial);
    void Draw(SDL_GPUCommandBuffer*, SDL_GPURenderPass*, const ShaderCommand&);
    void Commit(SDL_GPUCommandBuffer*);
    void Reclaim();
    void Shutdown(); // Caller drains device and releases prepared draws first.
private:
    SDL_GPUDevice* device_;
    Shader::ImageViews::Store images_;
    Shader::CanvasViews::Store canvases_;
    Shader::MeshBuffers::Store meshes_;
    backend::ShaderPipeline pipelines_;
    backend::ImagePipeline samplers_;
    std::shared_ptr<const Geometry::MeshData> triangle_;
    SDL_GPUTexture* emptyDistance_{};
    bool emptyReady_{}, emptyEncoded_{}, closed_{};
};
}
