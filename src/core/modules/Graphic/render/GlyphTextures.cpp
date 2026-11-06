#include "GlyphTextures.h"
#include "core/modules/Graphic/text/Text.h"
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace core::modules::Graphic::Render::GlyphTextures {
namespace detail {
struct Gate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit Gate(SDL_GPUDevice* value) : device(value) {}
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Glyph textures require the render owner thread");
    }
    void Check() const { Thread(); if (!live) throw std::logic_error("Glyph texture store is shut down"); }
};
enum class Phase { Prepared, Encoded, Ready };
struct State {
    std::shared_ptr<Gate> gate;
    std::weak_ptr<const Text::GlyphBitmap> source;
    std::weak_ptr<const Entry> lease;
    const Text::GlyphBitmap* identity{};
    SDL_GPUTexture* fill{};
    SDL_GPUTexture* ring{};
    SDL_GPUTransferBuffer* transfer{};
    SDL_GPUCommandBuffer* encodedIn{};
    std::uint32_t width{}, height{}, rowPitch{}, ringOffset{}, transferBytes{};
    std::uint64_t textureBytes{}, lastUse{};
    Phase phase{Phase::Prepared};
    void Drawable(SDL_GPUCommandBuffer* cb) const {
        gate->Check();
        if (!fill || !ring || phase == Phase::Prepared || (phase == Phase::Encoded && (!cb || cb != encodedIn)))
            throw std::logic_error("Glyph upload must precede sampling in the same command buffer or be committed");
    }
};
}
struct Store::Impl {
    std::shared_ptr<detail::Gate> gate;
    Config config;
    std::vector<std::shared_ptr<detail::State>> cache;
    // The raw address narrows the lookup only. Shared ownership is checked on
    // every hit, including address reuse and distinct aliasing owners.
    std::unordered_multimap<const Text::GlyphBitmap*, std::weak_ptr<detail::State>> index;
    std::vector<Lease> encoded;
    SDL_GPUCommandBuffer* command{};
    SDL_GPUSampler* sampler{};
    Stats stats;
    std::uint64_t sequence{};
    Impl(SDL_GPUDevice* device, Config c) : gate(std::make_shared<detail::Gate>(device)), config(c) {
        if (!device || !c.maxEntries || c.maxEntries > 65536 || !c.maxBytes)
            throw std::invalid_argument("Glyph textures require a device and bounded nonzero limits");
        cache.reserve(c.maxEntries); encoded.reserve(c.maxEntries); index.reserve(c.maxEntries);
        SDL_GPUSamplerCreateInfo create{};
        create.min_filter = create.mag_filter = SDL_GPU_FILTER_LINEAR;
        create.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        create.address_mode_u = create.address_mode_v = create.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SDL_CreateGPUSampler(device, &create);
        if (!sampler) throw std::runtime_error(std::string("Glyph sampler creation failed: ") + SDL_GetError());
    }
    void Release(detail::State& state) noexcept {
        if (state.transfer) SDL_ReleaseGPUTransferBuffer(gate->device, state.transfer);
        if (state.ring) SDL_ReleaseGPUTexture(gate->device, state.ring);
        if (state.fill) SDL_ReleaseGPUTexture(gate->device, state.fill);
        state.transfer = nullptr; state.fill = state.ring = nullptr;
    }
    void ReleaseTransfer(detail::State& state) noexcept {
        if (!state.transfer) return;
        SDL_ReleaseGPUTransferBuffer(gate->device, state.transfer); state.transfer = nullptr;
        stats.bytes -= state.transferBytes; --stats.transfers;
    }
    void Erase(std::size_t position) noexcept {
        auto& state = *cache[position];
        const auto range = index.equal_range(state.identity);
        for (auto it = range.first; it != range.second; ++it) if (it->second.lock().get() == &state) {
            index.erase(it); break;
        }
        stats.bytes -= state.textureBytes + (state.transfer ? state.transferBytes : 0);
        if (state.transfer) --stats.transfers;
        Release(state); cache.erase(cache.begin() + position);
    }
    void Space(std::uint64_t bytes) {
        if (bytes > config.maxBytes) throw std::length_error("Glyph textures exceed GPU/transfer byte budget");
        while (cache.size() >= config.maxEntries || bytes > config.maxBytes - stats.bytes) {
            auto oldest = cache.size();
            for (std::size_t i = 0; i < cache.size(); ++i)
                if (cache[i].use_count() == 1 && (oldest == cache.size() || cache[i]->lastUse < cache[oldest]->lastUse)) oldest = i;
            if (oldest == cache.size()) throw std::length_error("Glyph texture capacity exhausted by live draw/upload pins");
            Erase(oldest); ++stats.evictions;
        }
    }
    void Transaction(SDL_GPUCommandBuffer* cb) const {
        gate->Check();
        if (!cb) throw std::invalid_argument("Glyph upload requires a command buffer");
        if (command && command != cb) throw std::logic_error("Glyph uploads already belong to another command buffer");
    }
};
Entry::Entry(std::shared_ptr<detail::State> state, std::shared_ptr<const Text::GlyphBitmap> source)
    : state_(std::move(state)), source_(std::move(source)) {}
Entry::~Entry() = default;
std::uint32_t Entry::Width() const noexcept { return state_->width; }
std::uint32_t Entry::Height() const noexcept { return state_->height; }
SDL_GPUTexture* Entry::Fill(SDL_GPUCommandBuffer* cb) const { state_->Drawable(cb); return state_->fill; }
SDL_GPUTexture* Entry::Ring(SDL_GPUCommandBuffer* cb) const { state_->Drawable(cb); return state_->ring; }
Store::Store(SDL_GPUDevice* device, Config config) : impl_(std::make_unique<Impl>(device, config)) {}
Store::~Store() { Shutdown(); }
SDL_GPUSampler* Store::Sampler() const { impl_->gate->Check(); return impl_->sampler; }
Lease Store::Acquire(std::shared_ptr<const Text::GlyphBitmap> source) {
    impl_->gate->Check();
    if (!source || !source->width || !source->height || source->width > 8192 || source->height > 8192)
        throw std::invalid_argument("Glyph upload requires a nonempty bitmap no larger than 8192 pixels per dimension");
    const auto pixels = std::uint64_t(source->width) * source->height;
    if (source->fill.size() != pixels || source->ring.size() != pixels)
        throw std::invalid_argument("Glyph masks must match the shared bitmap rectangle");
    const auto range = impl_->index.equal_range(source.get());
    for (auto it = range.first; it != range.second; ++it) {
        const auto state = it->second.lock();
        if (state && !state->source.owner_before(source) && !source.owner_before(state->source)) {
            // Reuse an existing draw/fence pin without keeping it alive in the
            // cache. A strong cached Entry would pin CPU masks beyond their budget.
            auto lease = state->lease.lock();
            if (!lease) {
                lease = Lease(new Entry(state, std::move(source)));
                state->lease = lease;
            }
            state->lastUse = ++impl_->sequence; ++impl_->stats.hits; return lease;
        }
    }
    const auto rowPitch = (std::uint64_t(source->width) * 4 + 255) & ~std::uint64_t(255);
    const auto planeBytes = rowPitch * source->height;
    const auto ringOffset = (planeBytes + 511) & ~std::uint64_t(511);
    const auto transferBytes = ringOffset + planeBytes;
    if (transferBytes > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("Glyph upload exceeds SDL transfer size range");
    const auto textureBytes = pixels * 8;
    // Ordinary hits need no whole-cache walk. The renderer reclaims at frame /
    // fence boundaries; pressure also retires unused transfers before eviction.
    if (impl_->cache.size() >= impl_->config.maxEntries ||
        textureBytes + transferBytes > impl_->config.maxBytes - impl_->stats.bytes) Reclaim();
    impl_->Space(textureBytes + transferBytes);
    if (!SDL_GPUTextureSupportsFormat(impl_->gate->device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
        throw std::runtime_error("GPU does not support RGBA8 glyph textures");
    auto state = std::make_shared<detail::State>();
    state->gate = impl_->gate; state->source = source; state->identity = source.get();
    state->width = source->width; state->height = source->height;
    state->rowPitch = static_cast<std::uint32_t>(rowPitch);
    state->ringOffset = static_cast<std::uint32_t>(ringOffset);
    state->transferBytes = static_cast<std::uint32_t>(transferBytes);
    state->textureBytes = textureBytes; state->lastUse = ++impl_->sequence;
    auto lease = Lease(new Entry(state, std::move(source)));
    state->lease = lease;
    try {
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D; texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; texture.width = state->width; texture.height = state->height;
        texture.layer_count_or_depth = texture.num_levels = 1; texture.sample_count = SDL_GPU_SAMPLECOUNT_1;
        state->fill = SDL_CreateGPUTexture(impl_->gate->device, &texture);
        if (!state->fill) throw std::runtime_error(std::string("Glyph fill texture creation failed: ") + SDL_GetError());
        state->ring = SDL_CreateGPUTexture(impl_->gate->device, &texture);
        if (!state->ring) throw std::runtime_error(std::string("Glyph ring texture creation failed: ") + SDL_GetError());
        SDL_GPUTransferBufferCreateInfo transfer{};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer.size = state->transferBytes;
        state->transfer = SDL_CreateGPUTransferBuffer(impl_->gate->device, &transfer);
        if (!state->transfer) throw std::runtime_error(std::string("Glyph transfer creation failed: ") + SDL_GetError());
        impl_->index.emplace(state->identity, state);
    } catch (...) { impl_->Release(*state); throw; }
    impl_->cache.push_back(std::move(state)); impl_->stats.bytes += textureBytes + transferBytes; ++impl_->stats.transfers;
    return lease;
}
void Store::EncodePending(SDL_GPUCommandBuffer* cb, std::span<const Lease> leases) {
    impl_->Transaction(cb);
    for (const auto& lease : leases) {
        if (!lease || lease->state_->gate != impl_->gate) throw std::invalid_argument("Glyph upload requires a lease from this store");
        if (lease->state_->phase == detail::Phase::Encoded && lease->state_->encodedIn != cb)
            throw std::logic_error("Glyph upload belongs to another command buffer");
    }
    if (std::none_of(leases.begin(), leases.end(), [](const auto& lease) { return lease->state_->phase == detail::Phase::Prepared; })) return;
    // Complete allocations and map writes before opening a copy pass. Commit
    // moves this vector to the frame, so restore its bounded capacity here.
    impl_->encoded.reserve(impl_->config.maxEntries);
    std::vector<Lease> prepared; prepared.reserve(std::min(leases.size(), impl_->config.maxEntries));
    for (const auto& lease : leases) if (lease->state_->phase == detail::Phase::Prepared &&
        std::none_of(prepared.begin(), prepared.end(), [&](const auto& item) { return item->state_ == lease->state_; })) {
        const auto& state = *lease->state_;
        auto* bytes = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(impl_->gate->device, state.transfer, false));
        if (!bytes) throw std::runtime_error(std::string("Glyph transfer map failed: ") + SDL_GetError());
        std::memset(bytes, 0, state.transferBytes);
        for (std::uint32_t y = 0; y < state.height; ++y) for (std::uint32_t x = 0; x < state.width; ++x) {
            const auto pixel = std::size_t(y) * state.width + x;
            auto* fill = bytes + std::size_t(y) * state.rowPitch + std::size_t(x) * 4;
            auto* ring = fill + state.ringOffset;
            fill[0] = fill[1] = fill[2] = ring[0] = ring[1] = ring[2] = 255;
            fill[3] = lease->source_->fill[pixel]; ring[3] = lease->source_->ring[pixel];
        }
        SDL_UnmapGPUTransferBuffer(impl_->gate->device, state.transfer); prepared.push_back(lease);
    }
    auto* pass = SDL_BeginGPUCopyPass(cb);
    if (!pass) throw std::runtime_error(std::string("Glyph upload copy pass failed: ") + SDL_GetError());
    impl_->command = cb;
    try {
        for (const auto& lease : prepared) {
            auto& state = *lease->state_;
            impl_->encoded.push_back(lease);
            state.phase = detail::Phase::Encoded; state.encodedIn = cb;
            SDL_GPUTextureTransferInfo source{};
            source.transfer_buffer = state.transfer; source.pixels_per_row = state.rowPitch / 4; source.rows_per_layer = state.height;
            SDL_GPUTextureRegion destination{};
            destination.texture = state.fill; destination.w = state.width; destination.h = state.height; destination.d = 1;
            SDL_ClearError(); SDL_UploadToGPUTexture(pass, &source, &destination, false);
            if (*SDL_GetError()) throw std::runtime_error(std::string("Glyph fill upload failed: ") + SDL_GetError());
            source.offset = state.ringOffset; destination.texture = state.ring;
            SDL_ClearError(); SDL_UploadToGPUTexture(pass, &source, &destination, false);
            if (*SDL_GetError()) throw std::runtime_error(std::string("Glyph ring upload failed: ") + SDL_GetError());
            ++impl_->stats.uploads;
        }
    } catch (...) { SDL_EndGPUCopyPass(pass); throw; }
    SDL_EndGPUCopyPass(pass);
}
std::vector<Lease> Store::CommitEncoded(SDL_GPUCommandBuffer* cb) {
    impl_->Transaction(cb);
    for (const auto& lease : impl_->encoded) { lease->state_->phase = detail::Phase::Ready; lease->state_->encodedIn = nullptr; }
    impl_->command = nullptr;
    std::vector<Lease> result; result.swap(impl_->encoded); return result;
}
void Store::RollbackEncoded(SDL_GPUCommandBuffer* cb) {
    impl_->Transaction(cb);
    for (const auto& lease : impl_->encoded) { lease->state_->phase = detail::Phase::Prepared; lease->state_->encodedIn = nullptr; }
    impl_->encoded.clear(); impl_->command = nullptr;
}
void Store::Reclaim() {
    impl_->gate->Check();
    for (std::size_t i = 0; i < impl_->cache.size();) {
        auto& state = impl_->cache[i];
        if (state.use_count() == 1 && state->source.expired()) impl_->Erase(i);
        else {
            if (state.use_count() == 1 && state->phase == detail::Phase::Ready) impl_->ReleaseTransfer(*state);
            ++i;
        }
    }
}
Stats Store::GetStats() const {
    impl_->gate->Check(); auto result = impl_->stats;
    result.entries = impl_->cache.size(); result.encoded = impl_->encoded.size(); return result;
}
void Store::Shutdown() {
    impl_->gate->Thread(); if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& state : impl_->cache) impl_->Release(*state);
    if (impl_->sampler) SDL_ReleaseGPUSampler(impl_->gate->device, impl_->sampler);
    impl_->sampler = nullptr;
    impl_->encoded.clear(); impl_->index.clear(); impl_->cache.clear(); impl_->command = nullptr; impl_->stats = {}; impl_->gate->device = nullptr;
}
}
