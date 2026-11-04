#include "ImageViews.h"
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace core::modules::Graphic::Shader::ImageViews {
namespace detail {
struct Gate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit Gate(SDL_GPUDevice* value) : device(value) {}
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Image view access requires the render owner thread");
    }
    void Check() const { Thread(); if (!live) throw std::logic_error("Image view store is shut down"); }
};
struct Key {
    std::weak_ptr<const void> lifetime;
    SDL_GPUTexture* texture{};
    std::uint32_t textureWidth{}, textureHeight{}, x{}, y{}, width{}, height{}, physicalLevels{}, stripLevels{}, extrusion{};
    bool operator==(const Key& rhs) const noexcept {
        return !lifetime.owner_before(rhs.lifetime) && !rhs.lifetime.owner_before(lifetime)
            && texture == rhs.texture && textureWidth == rhs.textureWidth && textureHeight == rhs.textureHeight
            && x == rhs.x && y == rhs.y && width == rhs.width && height == rhs.height
            && physicalLevels == rhs.physicalLevels && stripLevels == rhs.stripLevels && extrusion == rhs.extrusion;
    }
};
enum class Phase { Prepared, Encoded, Ready };
struct State {
    std::shared_ptr<Gate> gate;
    Key key;
    SDL_GPUTexture* texture{};
    SDL_GPUCommandBuffer* encodedIn{};
    std::uint32_t levels{};
    std::uint64_t bytes{}, lastUse{};
    Phase phase{Phase::Prepared};
    bool borrowed{};
    // No GPU destructor: Store is the sole SDL release authority.
};
}
namespace {
using detail::Phase;
detail::Key Key(const Render::Image& image) {
    Render::detail::Validate(image, {});
    // A hardware Texture2D view alone cannot encode a fractional sampling
    // period. Keep this unsupported case explicit until the shader compiler
    // routes every supported sampling expression through logical metadata.
    if (image.insetX != 0 || image.insetY != 0 || image.LogicalWidth() != image.width ||
        image.LogicalHeight() != image.height)
        throw std::invalid_argument("Custom Shader Image sampling currently requires integer crop bounds");
    if (image.alpha != Render::Alpha::Straight)
        throw std::invalid_argument("Image views require Straight RGBA8 Image data; Canvas uses a separate conversion path");
    if (image.textureWidth > 16384 || image.textureHeight > 16384 || !image.textureLevels ||
        image.textureLevels > static_cast<std::uint32_t>(std::bit_width(std::max(image.textureWidth, image.textureHeight))))
        throw std::invalid_argument("Image view physical texture extent or mip count is invalid");
    const bool whole = !image.x && !image.y && image.width == image.textureWidth && image.height == image.textureHeight;
    if (image.mipLevels && image.textureLevels != 1)
        throw std::invalid_argument("Image mip strips must occupy physical mip level zero only");
    if (!image.mipLevels && !whole && image.textureLevels != 1)
        throw std::invalid_argument("Image views do not infer crop regions in a physical mip chain");
    return {image.lifetime, image.texture, image.textureWidth, image.textureHeight,
        image.x, image.y, image.width, image.height, image.textureLevels, image.mipLevels,
        image.mipLevels ? image.mipExtrusion : 0};
}
std::uint64_t Bytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
    std::uint64_t result{};
    for (std::uint32_t level = 0; level < levels; ++level)
        result += std::uint64_t(std::max(1u, width >> level)) * std::max(1u, height >> level) * 4;
    return result;
}
}
struct Store::Impl {
    std::shared_ptr<detail::Gate> gate;
    Config config;
    std::vector<std::shared_ptr<detail::State>> cache;
    std::vector<Lease> encoded;
    SDL_GPUCommandBuffer* command{};
    Stats stats;
    std::uint64_t sequence{};
    Impl(SDL_GPUDevice* device, Config c) : gate(std::make_shared<detail::Gate>(device)), config(c) {
        if (!device || !c.maxViews || c.maxViews > 65536 || c.maxBytes < 4)
            throw std::invalid_argument("Image view store requires a device and bounded nonzero limits");
        if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
            throw std::runtime_error("GPU does not support sampled RGBA8 Image views");
        cache.reserve(c.maxViews);
        encoded.reserve(c.maxViews);
    }
    void Release(detail::State& state) noexcept {
        if (!state.borrowed && state.texture) SDL_ReleaseGPUTexture(gate->device, state.texture);
        state.texture = nullptr;
    }
    void Erase(std::size_t index) noexcept {
        auto& state = *cache[index];
        stats.bytes -= state.bytes;
        if (!state.borrowed) --stats.owned;
        Release(state); cache.erase(cache.begin() + index);
    }
    void Space(std::uint64_t bytes) {
        if (bytes > config.maxBytes) throw std::length_error("Image view exceeds private texture byte budget");
        while (cache.size() >= config.maxViews || bytes > config.maxBytes - stats.bytes) {
            std::size_t oldest = cache.size();
            for (std::size_t i = 0; i < cache.size(); ++i)
                if (cache[i].use_count() == 1 && (oldest == cache.size() || cache[i]->lastUse < cache[oldest]->lastUse)) oldest = i;
            if (oldest == cache.size()) throw std::length_error("Image view capacity exhausted by live draw/copy pins");
            Erase(oldest);
        }
    }
    void Transaction(SDL_GPUCommandBuffer* value) const {
        gate->Check();
        if (!value) throw std::invalid_argument("Image view copy requires a command buffer");
        if (command && command != value) throw std::logic_error("Image view copies already belong to another command buffer");
    }
};
View::View(std::shared_ptr<detail::State> state, std::shared_ptr<const void> source)
    : state_(std::move(state)), source_(std::move(source)) {}
View::~View() = default;
std::uint32_t View::Width() const noexcept { return state_->key.width; }
std::uint32_t View::Height() const noexcept { return state_->key.height; }
std::uint32_t View::Levels() const noexcept { return state_->levels; }
bool View::Borrowed() const noexcept { return state_->borrowed; }
SDL_GPUTexture* View::Native(SDL_GPUCommandBuffer* command) const {
    state_->gate->Check();
    if (!state_->texture || state_->phase == Phase::Prepared ||
        (state_->phase == Phase::Encoded && (!command || command != state_->encodedIn)))
        throw std::logic_error("Image view copy must precede sampling in the same command buffer or be committed");
    return state_->texture;
}
Store::Store(SDL_GPUDevice* device, Config config) : impl_(std::make_unique<Impl>(device, config)) {}
Store::~Store() { Shutdown(); }
Lease Store::Acquire(const Render::Image& image) {
    impl_->gate->Check();
    const auto key = Key(image);
    Reclaim();
    for (const auto& state : impl_->cache) if (state->key == key) {
        auto lease = Lease(new View(state, image.lifetime));
        state->lastUse = ++impl_->sequence; ++impl_->stats.hits;
        return lease;
    }
    const bool borrowed = !key.stripLevels && !key.x && !key.y &&
        key.width == key.textureWidth && key.height == key.textureHeight;
    const auto levels = key.stripLevels ? key.stripLevels : key.physicalLevels;
    const auto bytes = borrowed ? 0 : Bytes(key.width, key.height, levels);
    impl_->Space(bytes);
    auto state = std::make_shared<detail::State>();
    state->gate = impl_->gate; state->key = key; state->levels = levels;
    state->bytes = bytes; state->borrowed = borrowed; state->lastUse = ++impl_->sequence;
    // Allocate the facade/control block before SDL allocation. Cache capacity
    // was reserved at construction, so publication cannot fail after creation.
    auto lease = Lease(new View(state, image.lifetime));
    if (borrowed) { state->texture = image.texture; state->phase = Phase::Ready; }
    else {
        SDL_GPUTextureCreateInfo create{};
        create.type = SDL_GPU_TEXTURETYPE_2D; create.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        create.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        create.width = key.width; create.height = key.height;
        create.layer_count_or_depth = 1; create.num_levels = levels; create.sample_count = SDL_GPU_SAMPLECOUNT_1;
        state->texture = SDL_CreateGPUTexture(impl_->gate->device, &create);
        if (!state->texture) throw std::runtime_error(std::string("Image view texture creation failed: ") + SDL_GetError());
    }
    impl_->cache.push_back(std::move(state));
    impl_->stats.bytes += bytes; if (!borrowed) ++impl_->stats.owned;
    return lease;
}
void Store::EncodeCopy(SDL_GPUCommandBuffer* command, std::span<const Lease> leases) {
    impl_->Transaction(command);
    // Validate the complete batch before modifying any state or GPU commands.
    for (const auto& lease : leases) {
        if (!lease || lease->state_->gate != impl_->gate)
            throw std::invalid_argument("Image view copy requires a live view from this store");
        const auto& state = *lease->state_;
        if (state.phase == Phase::Encoded && state.encodedIn != command)
            throw std::logic_error("Image view was encoded in another command buffer");
    }
    if (std::none_of(leases.begin(), leases.end(), [](const auto& lease) { return lease->state_->phase == Phase::Prepared; })) return;
    // Commit moves the previous frame's transaction pins away. Restore this
    // capacity before opening a pass, never after a successful submission.
    impl_->encoded.reserve(impl_->config.maxViews);
    auto* pass = SDL_BeginGPUCopyPass(command);
    if (!pass) throw std::runtime_error(std::string("Image view copy pass failed: ") + SDL_GetError());
    impl_->command = command;
    for (const auto& lease : leases) {
        auto& state = *lease->state_;
        if (state.phase != Phase::Prepared) continue;
        // At most one transaction entry per bounded cache State. All capacity
        // and shared ownership already exist; recording below cannot allocate.
        impl_->encoded.push_back(lease);
        std::uint32_t y = state.key.y;
        for (std::uint32_t level = 0; level < state.levels; ++level) {
            SDL_GPUTextureLocation source{};
            source.texture = state.key.texture; source.x = state.key.x; source.y = y;
            SDL_GPUTextureLocation destination{};
            destination.texture = state.texture; destination.mip_level = level;
            const auto width = std::max(1u, state.key.width >> level);
            const auto height = std::max(1u, state.key.height >> level);
            SDL_CopyGPUTextureToTexture(pass, &source, &destination, width, height, 1, false);
            if (state.key.stripLevels) y += height + 2 * state.key.extrusion;
            ++impl_->stats.copies;
        }
        state.phase = Phase::Encoded; state.encodedIn = command;
    }
    SDL_EndGPUCopyPass(pass);
}
std::vector<Lease> Store::CommitEncoded(SDL_GPUCommandBuffer* command) {
    impl_->Transaction(command);
    for (const auto& lease : impl_->encoded) { lease->state_->phase = Phase::Ready; lease->state_->encodedIn = nullptr; }
    impl_->command = nullptr;
    std::vector<Lease> result;
    result.swap(impl_->encoded); // No allocation after successful GPU submission.
    return result;
}
void Store::RollbackEncoded(SDL_GPUCommandBuffer* command) {
    impl_->Transaction(command);
    for (const auto& lease : impl_->encoded) { lease->state_->phase = Phase::Prepared; lease->state_->encodedIn = nullptr; }
    impl_->encoded.clear(); impl_->command = nullptr;
}
void Store::Reclaim() {
    impl_->gate->Check();
    for (std::size_t i = 0; i < impl_->cache.size();)
        if (impl_->cache[i].use_count() == 1 && impl_->cache[i]->key.lifetime.expired()) impl_->Erase(i);
        else ++i;
}
Stats Store::GetStats() const {
    impl_->gate->Check(); auto result = impl_->stats;
    result.views = impl_->cache.size(); result.encoded = impl_->encoded.size(); return result;
}
void Store::Shutdown() {
    impl_->gate->Thread();
    if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& state : impl_->cache) impl_->Release(*state);
    impl_->encoded.clear(); impl_->cache.clear(); impl_->command = nullptr; impl_->stats = {};
    impl_->gate->device = nullptr;
}
}
