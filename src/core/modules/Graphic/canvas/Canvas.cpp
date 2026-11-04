#include "Canvas.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace core::modules::Graphic::Canvas {
namespace {
std::atomic<std::uint64_t> nextOwner{1};
constexpr auto usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
constexpr auto depthUsage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
std::uint64_t Bytes(const Info& info) { return std::uint64_t(info.width) * info.height * (info.threeD ? 12 : 4); }
}
Bounds Centered(std::uint32_t w, std::uint32_t h) { return {-double(w)/2, double(w)/2, -double(h)/2, double(h)/2}; }
void Validate(Bounds b) {
    if (!std::isfinite(b.left) || !std::isfinite(b.right) || !std::isfinite(b.bottom) || !std::isfinite(b.top) ||
        !std::isfinite(b.right-b.left) || !std::isfinite(b.top-b.bottom) || b.left == b.right || b.bottom == b.top)
        throw std::invalid_argument("Canvas bounds must be finite nonzero ranges");
}
Point ToClip(Bounds b, Point p) {
    Validate(b);
    Point result{2 * ((p.x-b.left)/(b.right-b.left)) - 1, 2 * ((p.y-b.bottom)/(b.top-b.bottom)) - 1};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        throw std::invalid_argument("Canvas coordinates exceed the finite projection range");
    return result;
}
struct Store::Impl {
    struct Slot { std::uint64_t generation{1}; std::shared_ptr<detail::Attachment> attachment; };
    SDL_GPUDevice* device;
    SDL_Window* window;
    Config config;
    const std::thread::id thread{std::this_thread::get_id()};
    const std::uint64_t owner{nextOwner.fetch_add(1)};
    std::vector<Slot> slots;
    std::vector<std::shared_ptr<detail::Attachment>> retired;
    Stats stats;
    bool closed{}, windowPublished{};

    Impl(SDL_GPUDevice* d, SDL_Window* w, Config c) : device(d), window(w), config(c) {
        if (!device || !c.maxCanvases || c.maxCanvases >= std::numeric_limits<std::uint32_t>::max() ||
            !c.maxDimension || c.maxDimension > 16384 || c.maxBytes < 4)
            throw std::invalid_argument("Invalid Canvas device or limits");
        if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, usage))
            throw std::runtime_error("GPU does not support RGBA8 Canvas attachments");
        slots.resize(c.maxCanvases + 1);
        retired.reserve(c.maxCanvases);
        slots[0].attachment = std::make_shared<detail::Attachment>();
        slots[0].attachment->info.window = true;
    }
    void CheckThread() const {
        if (thread != std::this_thread::get_id()) throw std::logic_error("Canvas access requires the render owner thread");
    }
    void Check() const { CheckThread(); if (closed) throw std::logic_error("Canvas store is shut down"); }
    Slot& Lookup(Handle h) {
        Check();
        if (h.owner != owner || h.slot >= slots.size() || h.generation != slots[h.slot].generation || !slots[h.slot].attachment)
            throw std::invalid_argument("Invalid, retired or foreign Canvas handle");
        return slots[h.slot];
    }
    void Release(detail::Attachment& value) noexcept {
        value.live = false;
        if (value.texture) { SDL_ReleaseGPUTexture(device, value.texture); value.texture = nullptr; }
        if (value.depth) { SDL_ReleaseGPUTexture(device, value.depth); value.depth = nullptr; }
        if (value.distance) { SDL_ReleaseGPUTexture(device, value.distance); value.distance = nullptr; }
    }
};
Store::Store(SDL_GPUDevice* d, SDL_Window* w, Config c) : impl(std::make_unique<Impl>(d,w,c)) {}
Store::~Store() { Shutdown(); }
Handle Store::GetWindowCanvas() const { impl->Check(); return {impl->owner, impl->slots[0].generation, 0}; }
std::shared_ptr<detail::Attachment> Store::Acquire(Handle h) const { return impl->Lookup(h).attachment; }
Info Store::Describe(Handle h) const {
    auto info = Acquire(h)->info;
    if (info.window && impl->window && !impl->windowPublished) {
        int w{}, height{};
        if (!SDL_GetWindowSizeInPixels(impl->window, &w, &height)) throw std::runtime_error(SDL_GetError());
        info.width = static_cast<std::uint32_t>(w); info.height = static_cast<std::uint32_t>(height);
        if (!SDL_GetWindowSize(impl->window, &w, &height)) throw std::runtime_error(SDL_GetError());
        info.logicalWidth=static_cast<std::uint32_t>(std::max(w,1));
        info.logicalHeight=static_cast<std::uint32_t>(std::max(height,1));
        info.bounds = Centered(info.logicalWidth, info.logicalHeight);
    }
    return info;
}
void Store::PublishWindowMetrics(std::uint32_t width,std::uint32_t height) {
    impl->Check();
    if(!width || !height) throw std::invalid_argument("Window logical dimensions must be positive");
    if(!impl->window) return;
    int w{},h{};
    if(!SDL_GetWindowSizeInPixels(impl->window,&w,&h)) throw std::runtime_error(SDL_GetError());
    auto& info=impl->slots[0].attachment->info;
    info.width=static_cast<std::uint32_t>(std::max(w,0)); info.height=static_cast<std::uint32_t>(std::max(h,0));
    info.logicalWidth=width;info.logicalHeight=height;info.bounds=Centered(width,height);
    impl->windowPublished=true;
}
Handle Store::Create(std::uint32_t w, std::uint32_t h, std::optional<Bounds> requested) {
    return Allocate(w,h,requested,false);
}
Handle Store::Create3D(std::uint32_t w, std::uint32_t h) { return Allocate(w,h,{},true); }
Handle Store::Allocate(std::uint32_t w, std::uint32_t h, std::optional<Bounds> requested, bool threeD) {
    impl->Check();
    if (!w || !h || w > impl->config.maxDimension || h > impl->config.maxDimension)
        throw std::invalid_argument("Canvas pixel dimensions exceed the configured range");
    const Info info{w, h, requested.value_or(Centered(w,h)), false, threeD, w, h};
    Validate(info.bounds);
    Reclaim();
    if (Bytes(info) > impl->config.maxBytes - impl->stats.bytes ||
        impl->stats.live + impl->retired.size() >= impl->config.maxCanvases)
        throw std::length_error("Canvas attachment budget exhausted; retire unused canvases first");
    std::uint32_t index = 1;
    while (index < impl->slots.size() && (impl->slots[index].attachment || !impl->slots[index].generation)) ++index;
    if (index == impl->slots.size()) throw std::length_error("Canvas handle capacity exhausted");
    auto value = std::make_shared<detail::Attachment>();
    value->info = info;
    if (threeD && !SDL_GPUTextureSupportsFormat(impl->device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTURETYPE_2D, depthUsage))
        throw std::runtime_error("GPU does not support sampleable Canvas3D D32_FLOAT depth attachments");
    if (threeD && !SDL_GPUTextureSupportsFormat(impl->device, SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
        SDL_GPU_TEXTURETYPE_2D, usage))
        throw std::runtime_error("GPU does not support sampleable Canvas3D R32_FLOAT distance attachments");
    // Until all three attachments succeed, no public handle or budget is
    // committed. This also covers exceptions while constructing diagnostics.
    struct Rollback {
        Impl* store;
        detail::Attachment* value;
        ~Rollback() { if (value) store->Release(*value); }
    } rollback{impl.get(), value.get()};
    SDL_GPUTextureCreateInfo create{};
    create.type = SDL_GPU_TEXTURETYPE_2D; create.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    create.usage = usage; create.width = w; create.height = h;
    create.layer_count_or_depth = 1; create.num_levels = 1; create.sample_count = SDL_GPU_SAMPLECOUNT_1;
    value->texture = SDL_CreateGPUTexture(impl->device, &create);
    if (!value->texture) throw std::runtime_error(std::string("Canvas texture allocation failed: ") + SDL_GetError());
    if (threeD) {
        create.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT; create.usage=depthUsage;
        value->depth=SDL_CreateGPUTexture(impl->device,&create);
        if (!value->depth)
            throw std::runtime_error(std::string("Canvas3D depth allocation failed: ")+SDL_GetError());
        value->depthFormat=create.format;
        create.format=SDL_GPU_TEXTUREFORMAT_R32_FLOAT; create.usage=usage;
        value->distance=SDL_CreateGPUTexture(impl->device,&create);
        if (!value->distance)
            throw std::runtime_error(std::string("Canvas3D distance allocation failed: ")+SDL_GetError());
    }
    impl->slots[index].attachment = std::move(value);
    rollback.value = nullptr;
    ++impl->stats.live; impl->stats.bytes += Bytes(info);
    return {impl->owner, impl->slots[index].generation, index};
}
bool Store::Destroy(Handle h) {
    impl->Check();
    if (h.owner != impl->owner || h.slot >= impl->slots.size()) throw std::invalid_argument("Foreign Canvas handle");
    if (!h.slot) throw std::invalid_argument("Window Canvas cannot be destroyed");
    auto& slot = impl->slots[h.slot];
    if (slot.generation != h.generation || !slot.attachment) return false;
    impl->retired.push_back(slot.attachment); // Allocate before changing logical validity.
    slot.attachment->live = false;
    slot.attachment.reset(); ++slot.generation; // Generation 0 is permanently exhausted.
    --impl->stats.live;
    return true;
}
void Store::Reclaim() {
    impl->Check();
    for (auto it = impl->retired.begin(); it != impl->retired.end();) {
        if (it->use_count() != 1) { ++it; continue; }
        impl->stats.bytes -= Bytes((*it)->info);
        impl->Release(**it); it = impl->retired.erase(it);
    }
}
Stats Store::GetStats() const { impl->Check(); auto s = impl->stats; s.retired = impl->retired.size(); return s; }
void Store::Shutdown() {
    impl->CheckThread();
    if (impl->closed) return;
    impl->closed = true;
    for (auto& slot : impl->slots) {
        if (slot.attachment) impl->Release(*slot.attachment);
        slot.attachment.reset();
    }
    for (auto& value : impl->retired) impl->Release(*value);
    impl->retired.clear(); impl->stats = {};
}
}
