#include "MeshBuffers.h"
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace core::modules::Graphic::Shader::MeshBuffers {
namespace detail {
struct Gate {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
    explicit Gate(SDL_GPUDevice* value) : device(value) {}
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("Mesh buffer access requires the render owner thread");
    }
    void Check() const { Thread(); if (!live) throw std::logic_error("Mesh buffer store is shut down"); }
};
enum class Phase { Prepared, Encoded, Ready };
struct State {
    std::shared_ptr<Gate> gate;
    std::weak_ptr<const Geometry::MeshData> source;
    const Geometry::MeshData* identity{};
    SDL_GPUBuffer* vertex{};
    SDL_GPUBuffer* index{};
    SDL_GPUTransferBuffer* transfer{};
    SDL_GPUCommandBuffer* encodedIn{};
    std::uint32_t vertices{}, indices{}, vertexBytes{}, indexBytes{};
    std::uint64_t lastUse{};
    Phase phase{Phase::Prepared};
    void Drawable(SDL_GPUCommandBuffer* command) const {
        gate->Check();
        if (!vertex || !index || phase == Phase::Prepared || (phase == Phase::Encoded && (!command || command != encodedIn)))
            throw std::logic_error("Mesh copy must precede drawing in the same command buffer or be committed");
    }
};
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
        if (!device || !c.maxMeshes || c.maxMeshes > 65536 || !c.maxBytes)
            throw std::invalid_argument("Mesh buffer store requires a device and bounded nonzero limits");
        cache.reserve(c.maxMeshes); encoded.reserve(c.maxMeshes);
    }
    std::uint64_t Payload(const detail::State& state) const noexcept { return std::uint64_t(state.vertexBytes) + state.indexBytes; }
    void Release(detail::State& state) noexcept {
        if (state.transfer) SDL_ReleaseGPUTransferBuffer(gate->device, state.transfer);
        if (state.index) SDL_ReleaseGPUBuffer(gate->device, state.index);
        if (state.vertex) SDL_ReleaseGPUBuffer(gate->device, state.vertex);
        state.transfer = nullptr; state.index = state.vertex = nullptr;
    }
    void ReleaseTransfer(detail::State& state) noexcept {
        if (!state.transfer) return;
        SDL_ReleaseGPUTransferBuffer(gate->device, state.transfer); state.transfer = nullptr;
        stats.bytes -= Payload(state); --stats.transfers;
    }
    void Erase(std::size_t index) noexcept {
        auto& state = *cache[index];
        stats.bytes -= Payload(state) * (state.transfer ? 2 : 1);
        if (state.transfer) --stats.transfers;
        Release(state); cache.erase(cache.begin() + index);
    }
    void Space(std::uint64_t bytes) {
        if (bytes > config.maxBytes) throw std::length_error("Mesh buffers exceed GPU/transfer byte budget");
        while (cache.size() >= config.maxMeshes || bytes > config.maxBytes - stats.bytes) {
            std::size_t oldest = cache.size();
            for (std::size_t i = 0; i < cache.size(); ++i)
                if (cache[i].use_count() == 1 && (oldest == cache.size() || cache[i]->lastUse < cache[oldest]->lastUse)) oldest = i;
            if (oldest == cache.size()) throw std::length_error("Mesh buffer capacity exhausted by live draw/copy pins");
            Erase(oldest); ++stats.evictions;
        }
    }
    void Transaction(SDL_GPUCommandBuffer* value) const {
        gate->Check();
        if (!value) throw std::invalid_argument("Mesh copy requires a command buffer");
        if (command && command != value) throw std::logic_error("Mesh copies already belong to another command buffer");
    }
};
View::View(std::shared_ptr<detail::State> state, std::shared_ptr<const Geometry::MeshData> source)
    : state_(std::move(state)), source_(std::move(source)) {}
View::~View() = default;
std::uint32_t View::VertexCount() const noexcept { return state_->vertices; }
std::uint32_t View::IndexCount() const noexcept { return state_->indices; }
SDL_GPUBuffer* View::Vertex(SDL_GPUCommandBuffer* command) const { state_->Drawable(command); return state_->vertex; }
SDL_GPUBuffer* View::Index(SDL_GPUCommandBuffer* command) const { state_->Drawable(command); return state_->index; }
Store::Store(SDL_GPUDevice* device, Config config) : impl_(std::make_unique<Impl>(device, config)) {}
Store::~Store() { Shutdown(); }
Lease Store::Acquire(std::shared_ptr<const Geometry::MeshData> source) {
    impl_->gate->Check();
    if (!source) throw std::invalid_argument("Mesh buffer acquisition requires immutable MeshData");
    const auto bytes = source->ByteSize();
    if (bytes > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("Mesh upload exceeds SDL transfer size range");
    Reclaim();
    for (const auto& state : impl_->cache)
        if (state->identity == source.get() && !state->source.owner_before(source) && !source.owner_before(state->source)) {
            auto lease = Lease(new View(state, std::move(source)));
            state->lastUse = ++impl_->sequence; ++impl_->stats.hits; return lease;
        }
    impl_->Space(2 * std::uint64_t(bytes));
    auto state = std::make_shared<detail::State>();
    state->gate = impl_->gate; state->source = source; state->identity = source.get();
    state->vertices = static_cast<std::uint32_t>(source->Vertices().size());
    state->indices = static_cast<std::uint32_t>(source->Indices().size());
    state->vertexBytes = static_cast<std::uint32_t>(source->Vertices().size_bytes());
    state->indexBytes = static_cast<std::uint32_t>(source->Indices().size_bytes());
    state->lastUse = ++impl_->sequence;
    auto lease = Lease(new View(state, std::move(source)));
    try {
        SDL_GPUBufferCreateInfo buffer{}; buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX; buffer.size = state->vertexBytes;
        state->vertex = SDL_CreateGPUBuffer(impl_->gate->device, &buffer);
        if (!state->vertex) throw std::runtime_error(std::string("Mesh vertex buffer creation failed: ") + SDL_GetError());
        buffer.usage = SDL_GPU_BUFFERUSAGE_INDEX; buffer.size = state->indexBytes;
        state->index = SDL_CreateGPUBuffer(impl_->gate->device, &buffer);
        if (!state->index) throw std::runtime_error(std::string("Mesh index buffer creation failed: ") + SDL_GetError());
        SDL_GPUTransferBufferCreateInfo transfer{};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer.size = static_cast<Uint32>(bytes);
        state->transfer = SDL_CreateGPUTransferBuffer(impl_->gate->device, &transfer);
        if (!state->transfer) throw std::runtime_error(std::string("Mesh transfer creation failed: ") + SDL_GetError());
    } catch (...) { impl_->Release(*state); throw; }
    impl_->cache.push_back(std::move(state)); impl_->stats.bytes += 2 * bytes; ++impl_->stats.transfers;
    return lease;
}
void Store::EncodeCopy(SDL_GPUCommandBuffer* command, std::span<const Lease> leases) {
    impl_->Transaction(command);
    for (const auto& lease : leases) {
        if (!lease || lease->state_->gate != impl_->gate) throw std::invalid_argument("Mesh copy requires a lease from this store");
        if (lease->state_->phase == detail::Phase::Encoded && lease->state_->encodedIn != command)
            throw std::logic_error("Mesh copy belongs to another command buffer");
    }
    if (std::none_of(leases.begin(), leases.end(), [](const auto& lease) { return lease->state_->phase == detail::Phase::Prepared; })) return;
    // All memory allocation and memcpy complete before opening a copy pass.
    // The committed vector is moved to the frame, so refill its capacity here.
    impl_->encoded.reserve(impl_->config.maxMeshes);
    std::vector<Lease> prepared; prepared.reserve(std::min(leases.size(), impl_->config.maxMeshes));
    for (const auto& lease : leases) if (lease->state_->phase == detail::Phase::Prepared &&
        std::none_of(prepared.begin(), prepared.end(), [&](const auto& item) { return item->state_ == lease->state_; })) {
        const auto& state = *lease->state_;
        auto* bytes = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(impl_->gate->device, state.transfer, false));
        if (!bytes) throw std::runtime_error(std::string("Mesh transfer map failed: ") + SDL_GetError());
        std::memcpy(bytes, lease->source_->Vertices().data(), state.vertexBytes);
        std::memcpy(bytes + state.vertexBytes, lease->source_->Indices().data(), state.indexBytes);
        SDL_UnmapGPUTransferBuffer(impl_->gate->device, state.transfer); prepared.push_back(lease);
    }
    auto* pass = SDL_BeginGPUCopyPass(command);
    if (!pass) throw std::runtime_error(std::string("Mesh copy pass failed: ") + SDL_GetError());
    impl_->command = command;
    for (const auto& lease : prepared) {
        auto& state = *lease->state_;
        impl_->encoded.push_back(lease);
        SDL_GPUTransferBufferLocation source{state.transfer, 0};
        SDL_GPUBufferRegion destination{state.vertex, 0, state.vertexBytes};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        source.offset = state.vertexBytes; destination = {state.index, 0, state.indexBytes};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        state.phase = detail::Phase::Encoded; state.encodedIn = command; ++impl_->stats.copies;
    }
    SDL_EndGPUCopyPass(pass);
}
std::vector<Lease> Store::CommitEncoded(SDL_GPUCommandBuffer* command) {
    impl_->Transaction(command);
    for (const auto& lease : impl_->encoded) { lease->state_->phase = detail::Phase::Ready; lease->state_->encodedIn = nullptr; }
    impl_->command = nullptr;
    std::vector<Lease> result; result.swap(impl_->encoded); return result;
}
void Store::RollbackEncoded(SDL_GPUCommandBuffer* command) {
    impl_->Transaction(command);
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
    impl_->gate->Check(); auto result = impl_->stats; result.meshes = impl_->cache.size(); result.encoded = impl_->encoded.size(); return result;
}
void Store::Shutdown() {
    impl_->gate->Thread(); if (!impl_->gate->live) return;
    impl_->gate->live = false;
    for (const auto& state : impl_->cache) impl_->Release(*state);
    impl_->encoded.clear(); impl_->cache.clear(); impl_->command = nullptr; impl_->stats = {}; impl_->gate->device = nullptr;
}
}
