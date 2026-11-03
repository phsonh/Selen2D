#pragma once
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

// Internal GPU upload facility, not a Lua Texture API. No Object/Resource/Atlas
// dependency. One owner/render thread; borrowed device must outlive Shutdown.
namespace core::modules::Graphic::Upload {
namespace detail { struct Gate; struct TextureData; struct Result; struct QueueCore; }
enum class State { Queued, Uploading, GpuPending, Ready, Failed, Cancelled };
struct Progress {
    std::uint32_t rows{}, submittedRows{}, completedRows{};
    std::uint32_t mipLevels{1};
    bool mipmapsSubmitted{}, mipmapsCompleted{};
};
struct Pixels {
    std::uint32_t width{}, height{};
    std::span<const std::uint8_t> rgba; // Tightly packed, top-left straight RGBA8.
    std::shared_ptr<const void> lifetime; // Owns immutable bytes until copied.
    bool mipmaps{}; // Generate full chain from base level, without atlas gutters.
};
struct Config {
    std::size_t capacity{128}, maxTextures{1024}, transferSlots{3};
    std::uint32_t transferBytes{4 * 1024 * 1024}, maxDimension{16384};
    std::uint64_t maxTextureBytes{256 * 1024 * 1024}; // All RGBA mip levels, excluding driver overhead.
    std::uint64_t maxPendingBytes{256 * 1024 * 1024};
    std::uint32_t fenceTimeoutMs{30000};
};
struct Budget {
    std::uint64_t bytes{8 * 1024 * 1024}; // Padded transfer bytes, not just pixels.
    std::size_t textures{1}, transferAllocations{1}, submissions{3}, polls{3}, visits{32}, reclaims{16};
    std::size_t mipmaps{1}; // Whole-chain submissions; also consumes submissions.
};
struct Stats {
    std::size_t pending{}, textures{}, transferBuffers{}, inFlight{};
    std::uint64_t submittedBytes{}, completedBytes{}, submissions{}, polls{}, failures{}, retiredTextures{};
    std::uint64_t cpuBytes{};
    std::uint64_t mipmapSubmissions{}, completedMipmaps{};
};
class Texture {
public:
    ~Texture(); // Ownership notification only; never performs GPU work.
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;
    std::uint32_t Levels() const noexcept;
    // Native rendering integration only. Owner thread, Ready, live queue/device.
    // Existing recorded GPU work is protected by SDL deferred release. Retaining
    // this C++ facade after Shutdown does not permit access to the old pointer.
    SDL_GPUTexture* Native() const;
private:
    friend struct detail::QueueCore;
    friend class Ticket;
    std::shared_ptr<detail::TextureData> data;
};
class Ticket {
public:
    State GetState() const;
    Progress GetProgress() const;
    std::string GetError() const;
    bool Cancel(); // Immediate logical close; no GPU wait or worker join.
    std::shared_ptr<const Texture> GetTexture() const; // Ready only.
private:
    friend struct detail::QueueCore;
    friend class Queue;
    std::shared_ptr<detail::Result> result;
};

// Narrow SDL transfer adapter permits deterministic failure/fence tests. It is
// not another graphics backend abstraction: production has only SDL_GPU here.
// All functions run on Queue's owner thread. It must not throw in release/wait.
struct Submission { SDL_GPUFence* fence{}; bool uncertain{}; };
class TransferDevice {
public:
    virtual ~TransferDevice() = default;
    virtual SDL_GPUTexture* CreateTexture(std::uint32_t width, std::uint32_t height, std::uint32_t levels) = 0;
    virtual SDL_GPUTransferBuffer* CreateTransfer(std::uint32_t bytes) = 0;
    virtual void* Map(SDL_GPUTransferBuffer*) = 0;
    virtual void Unmap(SDL_GPUTransferBuffer*) = 0;
    // Unmapped input; offset=0, rowPitch=Align(width*4,256), cycle=false. A null
    // fence with uncertain=true means the CB may have executed: never retry it.
    virtual Submission Submit(SDL_GPUTransferBuffer*, SDL_GPUTexture*, std::uint32_t y,
        std::uint32_t width, std::uint32_t rows, std::uint32_t rowPitch) = 0;
    // All base-level fences have completed. Record outside passes; consumes no
    // transfer bytes but requires its own completion fence and submission budget.
    virtual Submission GenerateMipmaps(SDL_GPUTexture*) = 0;
    virtual bool Poll(SDL_GPUFence*) = 0;
    virtual void Release(SDL_GPUFence*) noexcept = 0;
    virtual void Release(SDL_GPUTexture*) noexcept = 0;
    virtual void Release(SDL_GPUTransferBuffer*) noexcept = 0;
    virtual bool WaitIdle() noexcept = 0; // Shutdown only, never a normal Tick.
    virtual std::string Error() const = 0;
};
std::unique_ptr<TransferDevice> SdlTransferDevice(SDL_GPUDevice*);
class Queue {
public:
    explicit Queue(SDL_GPUDevice*, Config = {});
    explicit Queue(std::unique_ptr<TransferDevice>, Config = {});
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    // Validates small metadata; no creation, memcpy, I/O, submission or GPU wait.
    // A full texture set first receives a bounded retirement scan. nullopt is
    // temporary queue/byte pressure, an incomplete scan, or pending retirement.
    // Throws length_error when all slots remain occupied by live textures or
    // reservations after a complete scan; callers must fail rather than wait
    // indefinitely for resources retained by their own atomic loading job.
    std::optional<Ticket> TryEnqueue(Pixels);
    void Tick(Budget = {});
    Stats GetStats() const;
    std::string FatalError() const;
    bool Shutdown(); // Invalidate facades, cancel, drain, release before device.
    // Queue mutation/destruction is owner-thread serialized, never reentrant.
    // Pixels owners must have nonthrowing deleters; Ticket.Cancel is permitted
    // during owner release, but callbacks must not destroy the active Queue.
private:
    std::unique_ptr<detail::QueueCore> core;
};
}
