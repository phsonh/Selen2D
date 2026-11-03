#include "ShaderPipeline.h"
#include "ImagePipeline.h"
#include "core/modules/Graphic/geometry/Mesh.h"
#include <SDL3/SDL_error.h>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace core::modules::Graphic::backend {
namespace detail {
struct ShaderPipelineGate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit ShaderPipelineGate(SDL_GPUDevice* value) : device(value) {}
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Shader pipeline access requires the render owner thread");
    }
    void Check() const { Thread(); if (!live) throw std::logic_error("Shader pipeline cache is shut down"); }
};
struct ShaderPipelineState {
    std::shared_ptr<ShaderPipelineGate> gate;
    std::weak_ptr<const Shader::Program> program;
    const Shader::Program* identity{};
    SDL_GPUTextureFormat color{}, depth{};
    bool depthEnabled{}, fullscreen{};
    SDL_GPUGraphicsPipeline* pipeline{};
    std::uint64_t lastUse{};
    // No GPU destructor. The cache releases SDL objects on its owner thread.
};
}
struct ShaderPipeline::Impl {
    std::shared_ptr<detail::ShaderPipelineGate> gate;
    Config config;
    std::vector<std::shared_ptr<detail::ShaderPipelineState>> cache;
    Stats stats;
    std::uint64_t sequence{};
    Impl(SDL_GPUDevice* device, Config c) : gate(std::make_shared<detail::ShaderPipelineGate>(device)), config(c) {
        if (!device || !c.maxPipelines || c.maxPipelines > 65536)
            throw std::invalid_argument("Shader pipeline cache requires a device and bounded nonzero capacity");
        cache.reserve(c.maxPipelines);
    }
    void Release(detail::ShaderPipelineState& state) noexcept {
        if (state.pipeline) SDL_ReleaseGPUGraphicsPipeline(gate->device, state.pipeline);
        state.pipeline = nullptr;
    }
    void Erase(std::size_t index) noexcept { Release(*cache[index]); cache.erase(cache.begin() + index); }
    void Space() {
        if (cache.size() < config.maxPipelines) return;
        std::size_t oldest = cache.size();
        for (std::size_t i = 0; i < cache.size(); ++i)
            if (cache[i].use_count() == 1 && (oldest == cache.size() || cache[i]->lastUse < cache[oldest]->lastUse)) oldest = i;
        if (oldest == cache.size()) throw std::length_error("Shader pipeline capacity exhausted by live frame pins");
        Erase(oldest); ++stats.evictions;
    }
};
ShaderPipeline::View::View(std::shared_ptr<detail::ShaderPipelineState> state, std::shared_ptr<const Shader::Program> program)
    : state_(std::move(state)), program_(std::move(program)) {}
ShaderPipeline::View::~View() = default;
SDL_GPUGraphicsPipeline* ShaderPipeline::View::Native() const {
    state_->gate->Check();
    // ProgramStore can be explicitly shut down while a facade is retained.
    // Do not let that facade authorize new draws through an old cached PSO.
    (void)program_->Vertex();
    if (!state_->pipeline) throw std::logic_error("Shader pipeline is no longer live");
    return state_->pipeline;
}
ShaderPipeline::ShaderPipeline(SDL_GPUDevice* device) : ShaderPipeline(device, Config{}) {}
ShaderPipeline::ShaderPipeline(SDL_GPUDevice* device, Config config) : impl_(std::make_unique<Impl>(device, config)) {}
ShaderPipeline::~ShaderPipeline() { Shutdown(); }
ShaderPipeline::Lease ShaderPipeline::Get(std::shared_ptr<const Shader::Program> program, SDL_GPUTextureFormat color,
    SDL_GPUTextureFormat depth, bool depthEnabled, bool fullscreen) {
    impl_->gate->Check();
    if (!program) throw std::invalid_argument("Shader pipeline requires a live Program");
    if (color == SDL_GPU_TEXTUREFORMAT_INVALID) throw std::invalid_argument("Shader pipeline requires a color target format");
    auto* vertex = program->Vertex(); auto* fragment = program->Fragment();
    const bool threeD = depth != SDL_GPU_TEXTUREFORMAT_INVALID;
    depthEnabled = threeD && depthEnabled && !fullscreen;
    Reclaim();
    for (const auto& state : impl_->cache)
        if (state->identity == program.get() && !state->program.owner_before(program) && !program.owner_before(state->program) &&
            state->color == color && state->depth == depth && state->depthEnabled == depthEnabled && state->fullscreen == fullscreen) {
            auto lease = Lease(new View(state, std::move(program)));
            state->lastUse = ++impl_->sequence; ++impl_->stats.hits; return lease;
        }
    impl_->Space();
    auto state = std::make_shared<detail::ShaderPipelineState>();
    state->gate = impl_->gate; state->program = program; state->identity = program.get();
    state->color = color; state->depth = depth; state->depthEnabled = depthEnabled; state->fullscreen = fullscreen;
    state->lastUse = ++impl_->sequence;
    auto lease = Lease(new View(state, std::move(program)));

    using Vertex = Geometry::MeshVertex;
    static_assert(sizeof(Vertex) == 36 && offsetof(Vertex, uv) == 12 && offsetof(Vertex, color) == 20);
    const SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const std::array<SDL_GPUVertexAttribute, 3> attributes{{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, color)},
    }};
    std::array<SDL_GPUColorTargetDescription, 2> targets{};
    targets[0].format = color; targets[0].blend_state = MakeBlendState(Render::Blend::MulAlpha);
    targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    targets[1].blend_state.enable_color_write_mask = true;
    targets[1].blend_state.color_write_mask = depthEnabled ? SDL_GPU_COLORCOMPONENT_R : 0;
    SDL_GPUGraphicsPipelineCreateInfo create{};
    create.vertex_shader = vertex; create.fragment_shader = fragment;
    create.vertex_input_state = {&buffer, 1, attributes.data(), static_cast<Uint32>(attributes.size())};
    create.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    create.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    create.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    create.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    create.rasterizer_state.enable_depth_clip = true;
    create.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    create.target_info.color_target_descriptions = targets.data(); create.target_info.num_color_targets = threeD ? 2 : 1;
    create.target_info.has_depth_stencil_target = threeD; create.target_info.depth_stencil_format = depth;
    create.depth_stencil_state.enable_depth_test = depthEnabled;
    create.depth_stencil_state.enable_depth_write = depthEnabled;
    create.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    state->pipeline = SDL_CreateGPUGraphicsPipeline(impl_->gate->device, &create);
    if (!state->pipeline) throw std::runtime_error(std::string("Shader graphics pipeline creation failed: ") + SDL_GetError());
    // Both facade and cache capacity exist before the GPU call. No allocation
    // can strand a successfully created PSO during publication.
    impl_->cache.push_back(std::move(state)); ++impl_->stats.created; return lease;
}
void ShaderPipeline::Reclaim() {
    impl_->gate->Check();
    for (std::size_t i = 0; i < impl_->cache.size();)
        if (impl_->cache[i].use_count() == 1 && impl_->cache[i]->program.expired()) impl_->Erase(i);
        else ++i;
}
ShaderPipeline::Stats ShaderPipeline::GetStats() const {
    impl_->gate->Check(); auto result = impl_->stats; result.entries = impl_->cache.size(); return result;
}
void ShaderPipeline::Shutdown() {
    impl_->gate->Thread(); if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& state : impl_->cache) impl_->Release(*state);
    impl_->cache.clear(); impl_->stats = {}; impl_->gate->device = nullptr;
}
}
