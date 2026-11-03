#include "CanvasViews.h"
#include "CanvasViewShaders.h"
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace core::modules::Graphic::Shader::CanvasViews {
namespace detail {
struct Gate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit Gate(SDL_GPUDevice* value) : device(value) {}
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Canvas view access requires the render owner thread");
    }
    void Check() const { Thread(); if (!live) throw std::logic_error("Canvas view store is shut down"); }
};
struct State {
    std::shared_ptr<Gate> gate;
    std::weak_ptr<Canvas::detail::Attachment> source;
    Canvas::detail::Attachment* identity{};
    Kind kind{};
    std::uint32_t width{}, height{};
    SDL_GPUTexture* texture{};
    SDL_GPUCommandBuffer* encodedIn{};
    std::uint64_t bytes{}, committedSerial{}, pendingSerial{}, lastUse{};
    // Store owns GPU release, even if the final external Lease dies elsewhere.
};
}
namespace {
using Attachment = Canvas::detail::Attachment;
SDL_GPUTexture* Source(const Attachment& source, Kind kind) {
    if (kind == Kind::Color) return source.texture;
    if (kind == Kind::Distance) return source.info.threeD ? source.distance : nullptr;
    throw std::invalid_argument("invalid Canvas view kind");
}
SDL_GPUTextureFormat Format(Kind kind) {
    return kind == Kind::Color ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
}
struct Pipeline {
    SDL_GPUDevice* device;
    SDL_GPUShader* vertex{};
    SDL_GPUShader* fragment{};
    SDL_GPUGraphicsPipeline* pipeline{};
    SDL_GPUSampler* sampler{};
    explicit Pipeline(SDL_GPUDevice* d) : device(d) {}
    ~Pipeline() {
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (fragment) SDL_ReleaseGPUShader(device, fragment);
        if (vertex) SDL_ReleaseGPUShader(device, vertex);
    }
    void Init() {
        if (!(SDL_GetGPUShaderFormats(device) & canvas_view_shader_bytecode::format))
            throw std::runtime_error("Canvas conversion bytecode is unavailable for the active SDL_GPU device");
        SDL_GPUShaderCreateInfo shader{};
        shader.entrypoint = "main"; shader.format = canvas_view_shader_bytecode::format;
        shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        shader.code = canvas_view_shader_bytecode::vertex; shader.code_size = sizeof(canvas_view_shader_bytecode::vertex);
        vertex = SDL_CreateGPUShader(device, &shader);
        if (!vertex) throw std::runtime_error(std::string("Canvas conversion vertex shader failed: ") + SDL_GetError());
        shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; shader.num_samplers = 1;
        shader.code = canvas_view_shader_bytecode::fragment; shader.code_size = sizeof(canvas_view_shader_bytecode::fragment);
        fragment = SDL_CreateGPUShader(device, &shader);
        if (!fragment) throw std::runtime_error(std::string("Canvas conversion fragment shader failed: ") + SDL_GetError());
        SDL_GPUColorTargetDescription target{}; target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        target.blend_state.enable_blend = false;
        SDL_GPUGraphicsPipelineCreateInfo create{};
        create.vertex_shader = vertex; create.fragment_shader = fragment;
        create.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        create.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        create.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        create.rasterizer_state.enable_depth_clip = true;
        create.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        create.target_info.color_target_descriptions = &target; create.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &create);
        if (!pipeline) throw std::runtime_error(std::string("Canvas conversion pipeline failed: ") + SDL_GetError());
        SDL_GPUSamplerCreateInfo sample{};
        sample.min_filter = sample.mag_filter = SDL_GPU_FILTER_NEAREST;
        sample.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sample.address_mode_u = sample.address_mode_v = sample.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SDL_CreateGPUSampler(device, &sample);
        if (!sampler) throw std::runtime_error(std::string("Canvas conversion sampler failed: ") + SDL_GetError());
    }
};
}
struct Store::Impl {
    std::shared_ptr<detail::Gate> gate;
    Config config;
    std::vector<std::shared_ptr<detail::State>> cache;
    std::vector<Lease> encoded;
    SDL_GPUCommandBuffer* command{};
    std::unique_ptr<Pipeline> pipeline;
    Stats stats;
    std::uint64_t sequence{};
    Impl(SDL_GPUDevice* d, Config c) : gate(std::make_shared<detail::Gate>(d)), config(c) {
        if (!d || !c.maxViews || c.maxViews > 65536 || c.maxBytes < 4)
            throw std::invalid_argument("Canvas view store requires a device and bounded nonzero limits");
        cache.reserve(c.maxViews); encoded.reserve(c.maxViews);
    }
    void Release(detail::State& state) noexcept {
        if (state.texture) SDL_ReleaseGPUTexture(gate->device, state.texture);
        state.texture = nullptr;
    }
    void Erase(std::size_t index) noexcept {
        stats.bytes -= cache[index]->bytes; Release(*cache[index]); cache.erase(cache.begin() + index);
    }
    void Space(std::uint64_t bytes) {
        if (bytes > config.maxBytes) throw std::length_error("Canvas view exceeds private texture byte budget");
        while (cache.size() >= config.maxViews || bytes > config.maxBytes - stats.bytes) {
            auto oldest = cache.size();
            for (std::size_t i = 0; i < cache.size(); ++i)
                if (cache[i].use_count() == 1 && (oldest == cache.size() || cache[i]->lastUse < cache[oldest]->lastUse)) oldest = i;
            if (oldest == cache.size()) throw std::length_error("Canvas view capacity exhausted by live draw/conversion pins");
            Erase(oldest);
        }
    }
    void Transaction(SDL_GPUCommandBuffer* cb) const {
        gate->Check();
        if (!cb) throw std::invalid_argument("Canvas conversion requires a command buffer");
        if (command && command != cb) throw std::logic_error("Canvas views already have pending work in another command buffer");
    }
    Pipeline& Conversion() {
        if (!pipeline) { auto value = std::make_unique<Pipeline>(gate->device); value->Init(); pipeline = std::move(value); }
        return *pipeline;
    }
};
View::View(std::shared_ptr<detail::State> state, std::shared_ptr<Attachment> source)
    : state_(std::move(state)), source_(std::move(source)) {}
View::~View() = default;
std::uint32_t View::Width() const noexcept { return state_->width; }
std::uint32_t View::Height() const noexcept { return state_->height; }
Kind View::Type() const noexcept { return state_->kind; }
SDL_GPUTexture* View::Native(SDL_GPUCommandBuffer* cb) const {
    state_->gate->Check();
    if (!state_->texture || !Source(*source_, state_->kind)) throw std::logic_error("Canvas view source was shut down");
    if ((state_->encodedIn && state_->encodedIn != cb) || (!state_->encodedIn && !state_->committedSerial))
        throw std::logic_error("Canvas view must be encoded before sampling in the same command buffer or be committed");
    return state_->texture;
}
Store::Store(SDL_GPUDevice* d, Config c) : impl_(std::make_unique<Impl>(d, c)) {}
Store::~Store() { Shutdown(); }
Lease Store::Acquire(const std::shared_ptr<Attachment>& source, Kind kind) {
    impl_->gate->Check();
    if (!source || !source->live || source->info.window || !Source(*source, kind) ||
        !source->info.width || !source->info.height || source->info.width > 16384 || source->info.height > 16384)
        throw std::invalid_argument("Canvas views require a live offscreen Canvas and a valid source attachment");
    Reclaim();
    for (const auto& state : impl_->cache) if (state->identity == source.get() && state->kind == kind &&
        !state->source.owner_before(source) && !source.owner_before(state->source)) {
        auto lease = Lease(new View(state, source)); state->lastUse = ++impl_->sequence; ++impl_->stats.hits; return lease;
    }
    const auto bytes = std::uint64_t(source->info.width) * source->info.height * (kind == Kind::Color ? 8 : 4);
    impl_->Space(bytes);
    const auto usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (kind == Kind::Color ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0u);
    if (!SDL_GPUTextureSupportsFormat(impl_->gate->device, Format(kind), SDL_GPU_TEXTURETYPE_2D, usage))
        throw std::runtime_error("GPU does not support the requested Canvas view format/usage");
    auto state = std::make_shared<detail::State>();
    state->gate = impl_->gate; state->source = source; state->identity = source.get(); state->kind = kind;
    state->width = source->info.width; state->height = source->info.height; state->bytes = bytes; state->lastUse = ++impl_->sequence;
    auto lease = Lease(new View(state, source));
    SDL_GPUTextureCreateInfo create{};
    create.type = SDL_GPU_TEXTURETYPE_2D; create.format = Format(kind); create.usage = usage;
    create.width = state->width; create.height = state->height; create.layer_count_or_depth = 1;
    create.num_levels = 1; create.sample_count = SDL_GPU_SAMPLECOUNT_1;
    state->texture = SDL_CreateGPUTexture(impl_->gate->device, &create);
    if (!state->texture) throw std::runtime_error(std::string("Canvas view texture creation failed: ") + SDL_GetError());
    impl_->cache.push_back(std::move(state)); impl_->stats.bytes += bytes; return lease;
}
void Store::Encode(SDL_GPUCommandBuffer* cb, const Lease& lease, std::uint64_t serial) {
    impl_->Transaction(cb);
    if (!lease || lease->state_->gate != impl_->gate || !serial)
        throw std::invalid_argument("Canvas conversion requires a view from this store and a nonzero content serial");
    auto& state = *lease->state_;
    auto* source = Source(*lease->source_, state.kind);
    if (!source || lease->source_->info.width != state.width || lease->source_->info.height != state.height)
        throw std::logic_error("Canvas source attachment was shut down or changed extent");
    const auto current = state.encodedIn ? state.pendingSerial : state.committedSerial;
    if (current == serial) return;
    // Commit moves pins into the frame; restore capacity before starting any
    // pass. The state appears at most once, even after repeated serial updates.
    impl_->encoded.reserve(impl_->config.maxViews);
    if (state.kind == Kind::Color) {
        const auto& pipeline = impl_->Conversion();
        SDL_GPUColorTargetInfo target{};
        target.texture = state.texture; target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE; target.cycle = false;
        auto* pass = SDL_BeginGPURenderPass(cb, &target, 1, nullptr);
        if (!pass) throw std::runtime_error(std::string("Canvas conversion render pass failed: ") + SDL_GetError());
        SDL_BindGPUGraphicsPipeline(pass, pipeline.pipeline);
        const SDL_GPUTextureSamplerBinding binding{source, pipeline.sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0); SDL_EndGPURenderPass(pass);
        ++impl_->stats.conversions;
    } else {
        auto* pass = SDL_BeginGPUCopyPass(cb);
        if (!pass) throw std::runtime_error(std::string("Canvas distance copy pass failed: ") + SDL_GetError());
        SDL_GPUTextureLocation from{}, to{}; from.texture = source; to.texture = state.texture;
        SDL_CopyGPUTextureToTexture(pass, &from, &to, state.width, state.height, 1, false);
        SDL_EndGPUCopyPass(pass); ++impl_->stats.copies;
    }
    if (!state.encodedIn) impl_->encoded.push_back(lease);
    state.encodedIn = cb; state.pendingSerial = serial; impl_->command = cb;
}
std::vector<Lease> Store::CommitEncoded(SDL_GPUCommandBuffer* cb) {
    impl_->Transaction(cb);
    for (const auto& lease : impl_->encoded) {
        auto& state = *lease->state_;
        state.committedSerial = state.pendingSerial; state.pendingSerial = 0; state.encodedIn = nullptr;
    }
    impl_->command = nullptr;
    std::vector<Lease> result; result.swap(impl_->encoded); return result;
}
void Store::RollbackEncoded(SDL_GPUCommandBuffer* cb) {
    impl_->Transaction(cb);
    for (const auto& lease : impl_->encoded) { lease->state_->pendingSerial = 0; lease->state_->encodedIn = nullptr; }
    impl_->encoded.clear(); impl_->command = nullptr;
}
void Store::Reclaim() {
    impl_->gate->Check();
    for (std::size_t i = 0; i < impl_->cache.size();)
        if (impl_->cache[i].use_count() == 1 && impl_->cache[i]->source.expired()) impl_->Erase(i);
        else ++i;
}
Stats Store::GetStats() const {
    impl_->gate->Check(); auto result = impl_->stats; result.views = impl_->cache.size(); result.encoded = impl_->encoded.size(); return result;
}
void Store::Shutdown() {
    impl_->gate->Thread();
    if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& state : impl_->cache) impl_->Release(*state);
    impl_->encoded.clear(); impl_->cache.clear(); impl_->pipeline.reset();
    impl_->command = nullptr; impl_->stats = {}; impl_->gate->device = nullptr;
}
}
