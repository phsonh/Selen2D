#include "ImageSources.h"
#include "core/modules/File/Vfs.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace core::modules::Resource {
namespace image_detail {
namespace Jobs = core::runtime::Jobs;
namespace Image = core::modules::Graphic::Image;
namespace File = core::modules::File;

struct Lifetime { std::atomic_bool active{true}; };
struct Metrics { std::atomic_size_t reads{}, decodes{}; };
struct SourceState {
    std::atomic<SourceStage> phase{SourceStage::Queued};
    std::atomic_size_t subscribers{};
    std::atomic_bool abandoned{};
    std::mutex mutex;
    std::shared_ptr<const SourcePixels> pixels;
    std::optional<Jobs::Ticket> task; // Assigned once before subscriptions escape.
};
SourceStage Stage(const SourceState& source) {
    if (!source.task) return SourceStage::Queued;
    switch (source.task->GetState()) {
    case Jobs::State::Queued: return SourceStage::Queued;
    case Jobs::State::Running: {
        const auto phase = source.phase.load(std::memory_order_acquire);
        // CPU result adoption precedes the worker's terminal transition; a
        // cancellation accepted in this gap must not expose a successful result.
        return phase == SourceStage::CpuReady ? SourceStage::Decoding : phase;
    }
    case Jobs::State::Succeeded: return SourceStage::CpuReady;
    case Jobs::State::Failed: return SourceStage::Failed;
    default: return SourceStage::Cancelled;
    }
}
bool Terminal(SourceStage stage) {
    return stage == SourceStage::CpuReady || stage == SourceStage::Failed || stage == SourceStage::Cancelled;
}
struct SourceClosed {};
struct Subscription {
    std::shared_ptr<SourceState> source;
    std::shared_ptr<Lifetime> lifetime;
    std::atomic_bool released{};
    Subscription(std::shared_ptr<SourceState> source_, std::shared_ptr<Lifetime> lifetime_)
        : source(std::move(source_)), lifetime(std::move(lifetime_)) {
        const std::lock_guard lock(source->mutex);
        if (source->abandoned.load(std::memory_order_acquire) ||
            !lifetime->active.load(std::memory_order_acquire)) throw SourceClosed{};
        source->subscribers.fetch_add(1, std::memory_order_relaxed);
    }
    void Release() {
        if (released.exchange(true, std::memory_order_acq_rel)) return;
        std::optional<Jobs::Ticket> cancel;
        {
            const std::lock_guard lock(source->mutex);
            // Join and last-leave use the same lock. A returning subscriber
            // cannot attach between the count reaching zero and closure.
            if (source->subscribers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
                source->task && !source->task->IsFinished()) {
                source->abandoned.store(true, std::memory_order_release);
                cancel = source->task;
                source->pixels.reset();
            }
        }
        if (cancel) cancel->Cancel();
    }
    ~Subscription() { Release(); }
};
std::size_t ImageBytes(Image::Dimensions size) {
    if (!size.width || !size.height || size.height > std::numeric_limits<std::size_t>::max() / 4 / size.width)
        throw Image::PngError(Image::PngFailure::TooLarge, "image byte count overflow");
    return static_cast<std::size_t>(size.width) * size.height * 4;
}
std::size_t DecodeCap(std::size_t imageBytes, std::size_t encodedBytes, std::size_t maximum) {
    const auto max = std::numeric_limits<std::size_t>::max();
    if (imageBytes > (max - 65536) / 3 || encodedBytes > (max - 65536 - imageBytes * 3) / 2)
        throw Image::PngError(Image::PngFailure::TooLarge, "decode reservation overflow");
    // This is an admission ceiling, not an assertion that IHDR predicts every
    // allocator request. The PNG allocator enforces the ceiling for malformed
    // streams too, including realloc's temporarily overlapping old/new blocks.
    return std::min(maximum, std::max<std::size_t>(4 * 1024 * 1024, imageBytes * 3 + encodedBytes * 2 + 65536));
}
void Prepare(const std::shared_ptr<SourceState>& source, const std::shared_ptr<Metrics>& metrics,
    Jobs::MemoryBudget memory, SourceConfig config, std::string path, std::uint64_t generation,
    const Jobs::StopToken& stop) {
    stop.Check();
    source->phase.store(SourceStage::Reading, std::memory_order_release);
    Jobs::Reservation credit;
    std::size_t allocationLimit{};
    File::ReadOptions options;
    options.maxBytes = config.maxFileBytes;
    options.cancelled = [&] { return stop.IsStopRequested(); };
    options.admit = [&](std::uint64_t size, std::string_view prefix) {
        stop.Check();
        const auto dimensions = Image::InspectPng({reinterpret_cast<const std::uint8_t*>(prefix.data()), prefix.size()}, config.maxDimension);
        const auto bytes = ImageBytes(dimensions);
        allocationLimit = DecodeCap(bytes, static_cast<std::size_t>(size), config.decodeAllocationLimit);
        if (bytes > allocationLimit || size > std::numeric_limits<std::size_t>::max() - allocationLimit)
            throw Image::PngError(Image::PngFailure::TooLarge, "PNG cannot fit source decode limits");
        // Reserve complete encoded + decoder footprint before allocating the
        // file snapshot. No worker holds half a reservation while waiting.
        credit = memory.Acquire(static_cast<std::size_t>(size) + allocationLimit, stop);
    };
    ++metrics->reads;
    File::Snapshot snapshot;
    try { snapshot = config.files->ReadSnapshot(path, options); }
    catch (const File::ReadCancelled&) { throw Jobs::Cancelled(); }
    stop.Check();
    source->phase.store(SourceStage::Decoding, std::memory_order_release);
    ++metrics->decodes;
    auto pixels = Image::DecodePng({reinterpret_cast<const std::uint8_t*>(snapshot.bytes.data()), snapshot.bytes.size()},
        config.maxDimension, allocationLimit,
        [](void* data) noexcept { return static_cast<const Jobs::StopToken*>(data)->IsStopRequested(); },
        const_cast<Jobs::StopToken*>(&stop));
    stop.Check();
    std::string{}.swap(snapshot.bytes); // Free capacity BEFORE returning memory credit.
    // PNG decoding preserves source bytes. GPU working pixels instead filter
    // in premultiplied RGBA8, before extrusion and mip generation. Quantization
    // is explicit; source files, dimensions and logical metadata stay unchanged.
    for(std::uint32_t y=0;y<pixels.height;++y) {
        stop.Check();
        auto* row=pixels.pixels.get()+std::size_t(y)*pixels.width*4;
        for(std::uint32_t x=0;x<pixels.width;++x) {
            auto* p=row+std::size_t(x)*4;
            for(int channel=0;channel<3;++channel)
                p[channel]=static_cast<std::uint8_t>((unsigned(p[channel])*p[3]+127)/255);
        }
    }
    if (!pixels.residentBytes || pixels.residentBytes > allocationLimit)
        throw std::runtime_error("PNG decoder returned invalid resident allocation accounting");
    credit.Shrink(pixels.residentBytes);
    auto result = std::make_shared<SourcePixels>();
    result->credit = std::move(credit);
    result->image = std::move(pixels);
    result->source = std::move(path);
    result->generation = generation;
    result->modified = snapshot.modified;
    stop.Check();
    {
        const std::lock_guard lock(source->mutex);
        stop.Check();
        if (source->abandoned.load(std::memory_order_acquire)) throw Jobs::Cancelled();
        source->pixels = std::move(result);
    }
    source->phase.store(SourceStage::CpuReady, std::memory_order_release);
}

struct SourceCore {
    struct Entry {
        std::string path;
        std::shared_ptr<SourceState> state;
        std::size_t residentBytes{}; // Owner-thread accounting, adopted once.
    };
    using Entries = std::list<Entry>;
    Jobs::Executor& executor;
    Jobs::MemoryBudget memory;
    SourceConfig config;
    const std::thread::id owner{std::this_thread::get_id()};
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::shared_ptr<Metrics> metrics = std::make_shared<Metrics>();
    Entries entries;
    Entries::iterator cursor;
    std::unordered_map<std::string, Entries::iterator> index;
    std::uint64_t nextGeneration{1};
    std::size_t cacheHits{}, evictions{};
    std::size_t residentBytes{};

    SourceCore(Jobs::Executor& executor_, Jobs::MemoryBudget memory_, SourceConfig config_)
        : executor(executor_), memory(std::move(memory_)), config(std::move(config_)) {
        if (!config.files || !config.capacity || !config.maxFileBytes || !config.decodeAllocationLimit || !config.maxDimension)
            throw std::invalid_argument("invalid image source configuration");
        index.reserve(config.capacity);
        cursor = entries.end();
    }
    void Owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("image source service requires owner thread");
    }
    void Running() const {
        Owner();
        if (!lifetime->active.load(std::memory_order_acquire)) throw std::logic_error("image source service shut down");
    }
    void Erase(Entries::iterator entry) {
        if (cursor == entry) cursor = std::next(entry);
        residentBytes -= entry->residentBytes;
        index.erase(entry->path);
        entries.erase(entry);
        ++evictions;
    }
    void Collect(std::size_t budget, bool needSlot = false) {
        // One cache entry per unit. A result's external references, if any, keep
        // its pixels and memory credit safe after dropping this cache entry.
        for (std::size_t i = 0; i < budget && !entries.empty(); ++i) {
            if (cursor == entries.end()) cursor = entries.begin();
            const auto entry = cursor++;
            const auto& source = *entry->state;
            const auto stage = Stage(source);
            if (stage == SourceStage::CpuReady && entry->residentBytes == 0) {
                const std::lock_guard lock(entry->state->mutex);
                if (source.pixels) {
                    entry->residentBytes = source.pixels->credit.Size();
                    residentBytes += entry->residentBytes;
                }
            }
            if (stage == SourceStage::Failed || stage == SourceStage::Cancelled) {
                // Cancellation can win after CPU adoption but before Executor's
                // terminal state. An old failed/cancelled ticket must not pin
                // an inaccessible image after its cache entry is removed.
                entry->state->abandoned.store(true, std::memory_order_release);
                const std::lock_guard lock(entry->state->mutex);
                entry->state->pixels.reset();
            }
            if (stage == SourceStage::Failed || stage == SourceStage::Cancelled ||
                (source.subscribers.load(std::memory_order_acquire) == 0 &&
                    (source.abandoned.load(std::memory_order_acquire) ||
                        // The shared budget also includes transient encoded/
                        // decoder reservations. Those are not cached pixels:
                        // using total "used" here evicted tiny reusable sources
                        // whenever another worker reserved its decode scratch.
                        (stage == SourceStage::CpuReady && (needSlot || residentBytes > config.cacheBytes ||
                            memory.GetStats().waiting != 0)))))
                Erase(entry);
            if (needSlot && entries.size() < config.capacity) needSlot = false;
        }
    }
};
}

using namespace image_detail;

SourceStage SourceTicket::GetStage() const {
    if (!subscription) throw std::logic_error("invalid image source ticket");
    if (subscription->released.load(std::memory_order_acquire) ||
        !subscription->lifetime->active.load(std::memory_order_acquire)) return SourceStage::Cancelled;
    return Stage(*subscription->source);
}
bool SourceTicket::IsFinished() const { return Terminal(GetStage()); }
bool SourceTicket::Cancel() {
    if (IsFinished()) return false;
    subscription->Release();
    return true;
}
std::exception_ptr SourceTicket::GetError() const {
    if (GetStage() != SourceStage::Failed) return {};
    return subscription->source->task->GetError();
}
std::shared_ptr<const SourcePixels> SourceTicket::GetPixels() const {
    if (GetStage() != SourceStage::CpuReady) {
        if (const auto error = GetError()) std::rethrow_exception(error);
        throw std::logic_error("image source has not successfully completed CPU preparation");
    }
    const std::lock_guard lock(subscription->source->mutex);
    if (subscription->released.load(std::memory_order_acquire) ||
        !subscription->lifetime->active.load(std::memory_order_acquire) || !subscription->source->pixels)
        throw Jobs::Cancelled();
    return subscription->source->pixels;
}
ImageSources::ImageSources(Jobs::Executor& executor, Jobs::MemoryBudget memory, SourceConfig config)
    : core(std::make_unique<SourceCore>(executor, std::move(memory), std::move(config))) {}
ImageSources::~ImageSources() {
    if (core->owner != std::this_thread::get_id()) std::terminate();
    Shutdown();
}
std::optional<SourceTicket> ImageSources::TryRequest(std::string path) {
    core->Running();
    // Registration performs no disk I/O. Source reuse is keyed by the virtual
    // path within this immutable view; physical package filenames never leak.
    const auto normalized = core->config.files->NormalizePath(path);
    if (!normalized || normalized->empty()) throw std::invalid_argument("invalid image source virtual path");
    path = *normalized;
    auto found = core->index.find(path);
    if (found != core->index.end()) {
        auto source = found->second->state;
        const auto stage = Stage(*source);
        if (!source->abandoned.load(std::memory_order_acquire) &&
            stage != SourceStage::Failed && stage != SourceStage::Cancelled) {
            try {
                SourceTicket ticket;
                ticket.subscription = std::make_shared<Subscription>(std::move(source), core->lifetime);
                ++core->cacheHits;
                return ticket;
            } catch (const SourceClosed&) {
                // The previous last subscription closed concurrently. Retry as
                // a fresh source generation instead of reviving cancelled work.
            }
        }
        core->Erase(found->second); // Existing ticket still refers to its old state.
    }
    core->Collect(core->config.capacity, core->entries.size() >= core->config.capacity);
    if (core->entries.size() >= core->config.capacity) return {};
    if (core->nextGeneration == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("image source generation exhausted");
    auto source = std::make_shared<SourceState>();
    const auto generation = core->nextGeneration++;
    core->entries.push_back({path, source});
    const auto entry = std::prev(core->entries.end());
    try { core->index.emplace(path, entry); }
    catch (...) { core->entries.erase(entry); throw; }
    try {
        auto task = core->executor.TrySubmit([source, metrics = core->metrics, memory = core->memory,
            config = core->config, path, generation](const Jobs::StopToken& stop) mutable {
            Prepare(source, metrics, std::move(memory), std::move(config), path, generation, stop);
        });
        if (!task) { core->Erase(entry); return {}; }
        source->task = std::move(*task);
        SourceTicket ticket;
        ticket.subscription = std::make_shared<Subscription>(source, core->lifetime);
        return ticket;
    } catch (...) {
        if (source->task) source->task->Cancel();
        core->Erase(entry);
        throw;
    }
}
void ImageSources::Collect(std::size_t budget) { core->Running(); core->Collect(budget); }
SourceStats ImageSources::GetStats() const {
    core->Owner();
    return {core->entries.size(), core->cacheHits, core->metrics->reads.load(), core->metrics->decodes.load(), core->evictions};
}
void ImageSources::Shutdown() {
    core->Owner();
    if (!core->lifetime->active.exchange(false, std::memory_order_acq_rel)) return;
    for (const auto& entry : core->entries) {
        entry.state->abandoned.store(true, std::memory_order_release);
        if (entry.state->task) entry.state->task->Cancel();
        const std::lock_guard lock(entry.state->mutex);
        entry.state->pixels.reset();
    }
    core->index.clear();
    core->entries.clear();
    core->residentBytes = 0;
    core->cursor = core->entries.end();
}
ImageRegion SelectRegion(std::shared_ptr<const SourcePixels> source, std::uint32_t x, std::uint32_t y,
    std::uint32_t width, std::uint32_t height) {
    if (!source || !source->image.pixels || !width || !height || width > source->image.width ||
        height > source->image.height || x > source->image.width - width || y > source->image.height - height)
        throw std::invalid_argument("image source region out of bounds");
    return {std::move(source), x, y, width, height};
}
std::span<const std::uint8_t> ImageRegion::Row(std::uint32_t row) const {
    // Public native value can be edited by its caller: validate before pointer
    // arithmetic instead of trusting an old SelectRegion check indefinitely.
    if (row >= height) throw std::out_of_range("image region row out of bounds");
    const auto checked = SelectRegion(source, x, y, width, height);
    const auto offset = (static_cast<std::size_t>(checked.y) + row) * source->image.width * 4 + static_cast<std::size_t>(x) * 4;
    return source->image.Bytes().subspan(offset, static_cast<std::size_t>(width) * 4);
}
}
