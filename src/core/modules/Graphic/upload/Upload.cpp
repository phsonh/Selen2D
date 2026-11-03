#include "Upload.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <list>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace core::modules::Graphic::Upload {
namespace detail {
struct Gate {
    const std::thread::id owner{std::this_thread::get_id()};
    bool active{true};
    bool busy{};
    // A facade may lose its last shared owner outside the render thread. Its
    // destructor reports ownership changes only; all GPU release stays here.
    std::atomic_uint64_t retirementEpoch{};
    void Owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("GPU upload access requires its owner thread");
    }
};
struct Mutation {
    Gate& gate;
    explicit Mutation(Gate& gate_) : gate(gate_) {
        gate.Owner();
        if (gate.busy) throw std::logic_error("GPU upload queue mutation must not be reentrant");
        gate.busy = true;
    }
    ~Mutation() { gate.busy = false; }
};
struct TextureData {
    std::shared_ptr<Gate> gate;
    SDL_GPUTexture* native{};
    std::uint32_t width{}, height{}, levels{1};
    bool ready{};
    // GPU release is owned by Queue's retirement list, never this destructor.
};
struct Result {
    std::shared_ptr<Gate> gate;
    State state{State::Queued};
    Progress progress;
    std::string error;
    std::shared_ptr<const Texture> texture;
};
bool Terminal(State state) {
    return state == State::Ready || state == State::Failed || state == State::Cancelled;
}
std::uint32_t Pitch(std::uint32_t width) {
    // A portable conservative SDL transfer layout; not native D3D12 commands.
    return static_cast<std::uint32_t>((std::uint64_t(width) * 4 + 255) / 256 * 256);
}
struct Shape { std::uint32_t levels{}; std::uint64_t bytes{}; };
Shape TextureShape(std::uint32_t width, std::uint32_t height, bool mipmaps) {
    // Called only after dimensions are bounded. Count rectangular/non-power-of-
    // two chains exactly, including the levels where one axis has reached one.
    Shape result;
    for (;;) {
        ++result.levels;
        result.bytes += std::uint64_t(width) * height * 4;
        if (!mipmaps || (width == 1 && height == 1)) return result;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
}
struct QueueCore {
    struct Work {
        std::shared_ptr<Result> result;
        Pixels pixels;
        std::shared_ptr<TextureData> texture;
        std::size_t inFlight{};
    };
    struct Slot {
        SDL_GPUTransferBuffer* transfer{};
        SDL_GPUFence* fence{};
        Work* work{};
        std::uint32_t rows{};
        std::uint64_t bytes{};
        bool quarantined{};
        bool mipmaps{};
        std::chrono::steady_clock::time_point submittedAt;
    };
    std::unique_ptr<TransferDevice> device;
    const Config config;
    const std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::list<Work> work;
    std::list<Work>::iterator cursor{work.end()};
    std::list<std::shared_ptr<TextureData>> textures;
    std::list<std::shared_ptr<TextureData>>::iterator retire{textures.end()};
    std::vector<Slot> slots;
    std::size_t slotCursor{}, pollCursor{};
    std::size_t reservedTextures{};
    std::size_t capacityScanned{};
    bool capacityProbe{};
    std::uint64_t capacityEpoch{};
    Stats stats;
    std::string fatal;
    bool closed{}, shutdownOk{true};

    QueueCore(std::unique_ptr<TransferDevice> device_, Config config_)
        : device(std::move(device_)), config(config_) {
        if (!device || !config.capacity || !config.maxTextures || !config.transferSlots || config.transferSlots > 16 ||
            config.transferBytes < 256 || !config.maxDimension || config.maxDimension > 16384 || !config.maxTextureBytes ||
            !config.maxPendingBytes || !config.fenceTimeoutMs)
            throw std::invalid_argument("invalid GPU upload queue configuration");
        slots.resize(config.transferSlots); // Fixed, never cycle to hidden buffers.
        fatal.reserve(256);
    }
    void Owner() const { gate->Owner(); }
    void Running() const {
        Owner();
        if (closed) throw std::logic_error("GPU upload queue is shut down");
        if (!gate->active) throw std::runtime_error("GPU upload queue failed: " + fatal);
    }
    void ReleasePixels(Work& item) {
        stats.cpuBytes -= item.pixels.rgba.size();
        item.pixels = {};
    }
    void Fail(Work& item, std::string message) {
        if (!Terminal(item.result->state)) {
            item.result->state = State::Failed;
            ++stats.failures;
            item.result->texture.reset();
            item.result->error = std::move(message);
        }
        ReleasePixels(item); // Transfer buffers own already-submitted bytes.
    }
    void CloseFailed() noexcept {
        gate->active = false;
        for (auto& item : work) {
            if (!Terminal(item.result->state)) {
                item.result->state = State::Failed;
                ++stats.failures;
                item.result->texture.reset();
            }
            ReleasePixels(item);
        }
    }
    void Fatal(std::string message) {
        CloseFailed(); // No diagnostic allocation before every gate is closed.
        fatal = std::move(message);
        for (auto& item : work)
            if (item.result->state == State::Failed && item.result->error.empty()) item.result->error = fatal;
    }
    void Poll(std::size_t budget) {
        // Examine each slot at most once, including idle ones. Separate cursor
        // prevents a tiny poll budget permanently starving higher-number slots.
        std::size_t scanned{};
        while (scanned++ < slots.size() && budget) {
            auto& slot = slots[pollCursor];
            pollCursor = (pollCursor + 1) % slots.size();
            if (!slot.fence) continue;
            --budget;
            ++stats.polls;
            if (!device->Poll(slot.fence)) {
                if (std::chrono::steady_clock::now() - slot.submittedAt >= std::chrono::milliseconds(config.fenceTimeoutMs)) {
                    Fatal("GPU upload fence timed out; transfers quarantined until device shutdown");
                    return;
                }
                continue;
            }
            device->Release(slot.fence);
            slot.fence = nullptr;
            auto& item = *slot.work;
            item.result->progress.completedRows += slot.rows;
            if (slot.mipmaps) {
                item.result->progress.mipmapsCompleted = true;
                ++stats.completedMipmaps;
            }
            --item.inFlight;
            --stats.inFlight;
            stats.completedBytes += slot.bytes;
            slot.work = nullptr;
            slot.rows = 0;
            slot.bytes = 0;
            slot.mipmaps = false;
        }
    }
    bool TextureCapacityFull() const {
        return textures.size() >= config.maxTextures - reservedTextures;
    }
    void CapacityChanged() noexcept { capacityProbe = false; capacityScanned = 0; }
    void ObserveRetirement() {
        const auto epoch = gate->retirementEpoch.load(std::memory_order_acquire);
        if (capacityProbe && capacityEpoch != epoch) {
            capacityEpoch = epoch;
            capacityScanned = 0;
        }
    }
    void Reclaim(std::size_t budget) {
        if (!TextureCapacityFull()) CapacityChanged();
        ObserveRetirement();
        const auto count = std::min(budget, textures.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (retire == textures.end()) retire = textures.begin();
            const auto current = retire++;
            if (current->use_count() != 1) {
                if (capacityProbe && capacityScanned < textures.size()) ++capacityScanned;
                continue;
            }
            // No pending work, Ticket facade, or accepted rendering command
            // retains this record. SDL still handles actual GPU deferred free.
            device->Release((*current)->native);
            (*current)->native = nullptr;
            textures.erase(current);
            CapacityChanged();
            ++stats.retiredTextures;
        }
    }
    bool AdmitTexture() {
        if (!TextureCapacityFull()) { CapacityChanged(); return true; }
        // A full live texture set cannot make progress merely by waiting for
        // upload queue space. In particular, an atomic Resource job may retain
        // its prepared pages while its remaining pages still need admission.
        // Before rejecting, inspect retirement in bounded chunks. The regular
        // Tick shares this cursor and contributes to the same complete sweep.
        if (!capacityProbe) {
            capacityProbe = true; capacityScanned = 0;
            capacityEpoch = gate->retirementEpoch.load(std::memory_order_acquire);
        }
        Reclaim(16);
        if (!TextureCapacityFull()) return true;
        // A release during/between chunks invalidates the already inspected
        // prefix. Continue a fresh bounded sweep rather than reject a slot
        // that became reclaimable behind the cursor.
        ObserveRetirement();
        if (capacityScanned < textures.size()) return false;
        // Cancelled/failed work (and a just-completed work record) can still
        // occupy a slot until Tick drains its fences and removes the record.
        // This is transient retirement, unlike live resources/reservations.
        for (const auto& item : work)
            if (Terminal(item.result->state)) return false;
        CapacityChanged(); // A later retry must examine ownership afresh.
        throw std::length_error("GPU texture capacity exhausted: all texture slots are held by live textures or upload reservations");
    }
    bool Create(Work& item, Budget& budget) {
        if (item.texture) return true;
        if (!budget.textures || textures.size() >= config.maxTextures) return false;
        --budget.textures;
        auto data = std::make_shared<TextureData>();
        data->gate = gate;
        data->width = item.pixels.width;
        data->height = item.pixels.height;
        data->levels = item.result->progress.mipLevels;
        auto facade = std::make_shared<Texture>();
        facade->data = data;
        // Allocate the tracking node BEFORE creating a native object. Every
        // successfully-created texture is then known to Shutdown/retirement.
        const auto record = textures.insert(textures.end(), data);
        CapacityChanged();
        try { data->native = device->CreateTexture(data->width, data->height, data->levels); }
        catch (...) { textures.erase(record); throw; }
        if (!data->native) {
            textures.erase(record);
            Fail(item, "GPU texture creation failed: " + device->Error());
            return false;
        }
        item.texture = std::move(data);
        --reservedTextures;
        item.result->texture = std::move(facade);
        item.result->state = State::Uploading;
        return true;
    }
    Slot* AvailableSlot() {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[slotCursor];
            slotCursor = (slotCursor + 1) % slots.size();
            if (!slot.fence && !slot.quarantined) return &slot;
        }
        return nullptr;
    }
    bool Track(Work& item, Slot& slot, Submission submission,
        std::uint32_t rows, std::uint64_t bytes, bool mipmaps) {
        if (!submission.fence) {
            if (submission.uncertain) {
                // May have executed. Keep its work and native texture alive,
                // including a mip-only command that has no transfer buffer.
                slot.quarantined = true;
                slot.work = &item;
                ++item.inFlight;
                ++stats.inFlight;
                CloseFailed(); // Close before any diagnostic can allocate/throw.
                fatal = "GPU upload submission result is uncertain; no retry";
                Fatal("GPU upload submission result is uncertain; no retry: " + device->Error());
            } else Fail(item, std::string(mipmaps ? "GPU mipmap command recording failed: " :
                "GPU upload command recording failed: ") + device->Error());
            return false;
        }
        slot.fence = submission.fence;
        slot.submittedAt = std::chrono::steady_clock::now();
        slot.work = &item;
        slot.rows = rows;
        slot.bytes = bytes;
        slot.mipmaps = mipmaps;
        ++item.inFlight;
        ++stats.inFlight;
        ++stats.submissions;
        stats.submittedBytes += bytes;
        if (mipmaps) ++stats.mipmapSubmissions;
        return true;
    }
    void GenerateMipmaps(Work& item, Budget& budget) {
        if (!budget.submissions || !budget.mipmaps || item.inFlight ||
            item.result->progress.mipmapsSubmitted) return;
        auto* slot = AvailableSlot();
        if (!slot) return;
        --budget.submissions;
        --budget.mipmaps;
        const auto submission = device->GenerateMipmaps(item.texture->native);
        if (Track(item, *slot, submission, 0, 0, true)) item.result->progress.mipmapsSubmitted = true;
    }
    void Upload(Work& item, Budget& budget) {
        if (!budget.submissions) return;
        const auto pitch = Pitch(item.pixels.width);
        const auto remaining = item.result->progress.rows - item.result->progress.submittedRows;
        const auto rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining,
            std::min<std::uint64_t>(config.transferBytes / pitch, budget.bytes / pitch)));
        if (!rows) return; // A future larger tick budget may admit one row.
        auto* slot = AvailableSlot();
        if (!slot) return;
        if (!slot->transfer) {
            if (!budget.transferAllocations) return;
            --budget.transferAllocations;
            slot->transfer = device->CreateTransfer(config.transferBytes);
            if (!slot->transfer) {
                Fail(item, "GPU upload transfer creation failed: " + device->Error());
                return;
            }
            ++stats.transferBuffers;
        }
        auto* destination = static_cast<std::uint8_t*>(device->Map(slot->transfer));
        if (!destination) { Fail(item, "GPU upload map failed: " + device->Error()); return; }
        const auto firstRow = item.result->progress.submittedRows;
        const auto rowBytes = static_cast<std::size_t>(item.pixels.width) * 4;
        for (std::uint32_t row = 0; row < rows; ++row) {
            auto* output = destination + static_cast<std::size_t>(row) * pitch;
            const auto* input = item.pixels.rgba.data() + (static_cast<std::size_t>(firstRow) + row) * rowBytes;
            std::memcpy(output, input, rowBytes);
            std::memset(output + rowBytes, 0, pitch - rowBytes);
        }
        device->Unmap(slot->transfer); // Must precede CopyPass recording.
        const auto bytes = std::uint64_t(pitch) * rows;
        budget.bytes -= bytes;
        --budget.submissions;
        const auto submission = device->Submit(slot->transfer, item.texture->native,
            firstRow, item.pixels.width, rows, pitch);
        if (!Track(item, *slot, submission, rows, bytes, false)) return;
        item.result->progress.submittedRows += rows;
        if (item.result->progress.submittedRows == item.result->progress.rows) {
            item.result->state = State::GpuPending;
            ReleasePixels(item); // Release may cancel Ticket; never revive it.
        }
    }
    void Tick(Budget budget) {
        Running();
        Poll(budget.polls); // Newly submitted work is never polled this tick.
        Reclaim(budget.reclaims);
        while (budget.visits && !work.empty() && gate->active) {
            --budget.visits;
            if (cursor == work.end()) cursor = work.begin();
            const auto current = cursor++;
            auto& item = *current;
            if (!Terminal(item.result->state) &&
                item.result->progress.submittedRows == item.result->progress.rows && !item.inFlight &&
                item.result->progress.mipmapsCompleted) {
                item.texture->ready = true;
                item.result->state = State::Ready;
            }
            if (Terminal(item.result->state)) {
                if (item.result->state != State::Ready) ReleasePixels(item);
                if (!item.inFlight) {
                    if (!item.texture) --reservedTextures;
                    work.erase(current);
                    CapacityChanged();
                }
                continue;
            }
            if (item.result->progress.submittedRows == item.result->progress.rows) {
                GenerateMipmaps(item, budget);
                continue;
            }
            if (!Create(item, budget)) continue;
            Upload(item, budget);
        }
    }
    bool Shutdown() {
        Owner();
        if (closed) return shutdownOk;
        closed = true;
        gate->active = false;
        for (auto& item : work) {
            if (!Terminal(item.result->state)) item.result->state = State::Cancelled;
            item.result->texture.reset();
            ReleasePixels(item);
        }
        // Sole blocking synchronization point. A false result is a device-level
        // failure: host must not resume this device; tear it down after cleanup.
        shutdownOk = device->WaitIdle();
        if (!shutdownOk) {
            try { fatal = "GPU shutdown drain failed: " + device->Error(); }
            catch (...) { fatal = "GPU shutdown drain failed"; }
        }
        for (auto& slot : slots) {
            if (slot.fence) device->Release(slot.fence);
            if (slot.transfer) device->Release(slot.transfer);
            slot = {};
        }
        for (auto& texture : textures) {
            device->Release(texture->native);
            texture->native = nullptr;
            texture->ready = false;
        }
        work.clear(); cursor = work.end();
        textures.clear(); retire = textures.end();
        stats.inFlight = 0;
        stats.transferBuffers = 0;
        reservedTextures = 0;
        return shutdownOk;
    }
};
}

Texture::~Texture() {
    const auto gate = data ? data->gate : nullptr;
    data.reset(); // Make the record reclaimable before publishing the epoch.
    if (gate) gate->retirementEpoch.fetch_add(1, std::memory_order_release);
}
std::uint32_t Texture::Width() const noexcept { return data ? data->width : 0; }
std::uint32_t Texture::Height() const noexcept { return data ? data->height : 0; }
std::uint32_t Texture::Levels() const noexcept { return data ? data->levels : 0; }
SDL_GPUTexture* Texture::Native() const {
    if (!data) throw std::logic_error("invalid GPU image facade");
    data->gate->Owner();
    if (!data->gate->active || !data->ready || !data->native)
        throw std::logic_error("GPU image is not available on a live device");
    return data->native;
}
namespace {
void Check(const std::shared_ptr<detail::Result>& result) {
    if (!result) throw std::logic_error("invalid GPU upload ticket");
    result->gate->Owner();
}
}
State Ticket::GetState() const { Check(result); return result->state; }
Progress Ticket::GetProgress() const { Check(result); return result->progress; }
std::string Ticket::GetError() const {
    Check(result);
    if (result->state == State::Failed && result->error.empty())
        return "GPU upload failed; queue/device access has been closed";
    return result->error;
}
bool Ticket::Cancel() {
    Check(result);
    if (detail::Terminal(result->state)) return false;
    result->state = State::Cancelled;
    result->texture.reset();
    return true;
}
std::shared_ptr<const Texture> Ticket::GetTexture() const {
    Check(result);
    if (result->state != State::Ready || !result->texture)
        throw std::logic_error("GPU upload has not successfully completed all fences");
    result->texture->Native(); // Validate device gate, not just historical state.
    return result->texture;
}
Queue::Queue(SDL_GPUDevice* device, Config config) : Queue(SdlTransferDevice(device), config) {}
Queue::Queue(std::unique_ptr<TransferDevice> device, Config config)
    : core(std::make_unique<detail::QueueCore>(std::move(device), config)) {}
Queue::~Queue() { try { Shutdown(); } catch (...) { std::terminate(); } }
std::optional<Ticket> Queue::TryEnqueue(Pixels pixels) {
    core->Running();
    const detail::Mutation mutation(*core->gate);
    if (!pixels.width || !pixels.height || pixels.width > core->config.maxDimension || pixels.height > core->config.maxDimension ||
        std::uint64_t(pixels.width) * pixels.height > std::numeric_limits<std::size_t>::max() / 4 ||
        std::uint64_t(pixels.width) * pixels.height * 4 > core->config.maxPendingBytes ||
        pixels.rgba.size() != std::uint64_t(pixels.width) * pixels.height * 4 || !pixels.lifetime ||
        detail::Pitch(pixels.width) > core->config.transferBytes)
        throw std::invalid_argument("invalid or over-limit GPU RGBA upload input");
    const auto shape = detail::TextureShape(pixels.width, pixels.height, pixels.mipmaps);
    if (shape.bytes > core->config.maxTextureBytes)
        throw std::invalid_argument("GPU texture including mip levels exceeds byte limit");
    if (!core->AdmitTexture()) return {};
    if (core->work.size() >= core->config.capacity ||
        pixels.rgba.size() > core->config.maxPendingBytes - core->stats.cpuBytes) return {};
    auto result = std::make_shared<detail::Result>();
    result->gate = core->gate;
    result->progress.rows = pixels.height;
    result->progress.mipLevels = shape.levels;
    result->progress.mipmapsCompleted = shape.levels == 1;
    core->work.push_back({result, std::move(pixels)});
    ++core->reservedTextures;
    core->CapacityChanged();
    core->stats.cpuBytes += core->work.back().pixels.rgba.size();
    Ticket ticket;
    ticket.result = std::move(result);
    return ticket;
}
void Queue::Tick(Budget budget) {
    core->Running(); // Wrong-thread/closed rejection must not modify the queue.
    const detail::Mutation mutation(*core->gate);
    try { core->Tick(budget); }
    catch (...) {
        core->CloseFailed();
        try { if (core->fatal.empty()) core->fatal = "GPU upload scheduling threw an exception; device access closed"; }
        catch (...) {} // All states are already terminal even under allocation failure.
        throw;
    }
}
Stats Queue::GetStats() const {
    core->Owner();
    auto stats = core->stats;
    stats.pending = core->work.size();
    stats.textures = core->textures.size();
    return stats;
}
std::string Queue::FatalError() const { core->Owner(); return core->fatal; }
bool Queue::Shutdown() {
    const detail::Mutation mutation(*core->gate);
    return core->Shutdown();
}
}
