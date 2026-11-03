#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

// Native loading coordination only. No Object, Lua VM, graphics device or file
// format dependency. Concrete providers are introduced in subsequent stages.
namespace core::modules::Resource {

enum class Kind { Image, Sound, Music, Shader, Font };
struct Id {
    Kind kind{};
    std::string name;
    bool operator==(const Id&) const = default;
};
struct Handle {
    std::uint32_t slot{};
    std::uint64_t generation{};
    std::uint64_t owner{}; // Resource service identity, not a GPU/context pointer.
    bool operator==(const Handle&) const = default;
};
struct Vec2 {
    double x{}, y{};
    bool operator==(const Vec2&) const = default;
};
enum class Filter { Point, Linear };
enum class Address { Clamp, Repeat };
struct ImageOptions {
    std::optional<Vec2> anchor; // Normalized to the region's pixel center on Add.
    Vec2 scale{1, 1};
    Filter filter{Filter::Linear};
    Address address{Address::Clamp};
    bool mipmap{};
    bool operator==(const ImageOptions&) const = default;
};
struct ImageRequest {
    std::uint32_t x{}, y{}, width{}, height{}; // Source pixels, top-left origin.
    ImageOptions options;
    bool wholeSource{}; // Resolve dimensions/default center after source decode.
    bool operator==(const ImageRequest&) const = default;
};
enum class AudioMode { Decoded, Stream };
struct Loop {
    double start{};
    std::optional<double> finish; // Absent means source end; convert once to frames.
    bool operator==(const Loop&) const = default;
};
struct AudioRequest {
    AudioMode mode{AudioMode::Decoded};
    std::optional<Loop> loop;
    bool operator==(const AudioRequest&) const = default;
};
struct FileRequest { bool operator==(const FileRequest&) const = default; };
struct Request {
    Id id;
    std::string source;
    std::variant<ImageRequest, AudioRequest, FileRequest> data;
    bool operator==(const Request&) const = default;
};

enum class State { Queued, Loading, Publishing, Ready, Failed, Cancelled };
enum class Stage { Queued, Reading, Decoding, CpuReady, Packing, UploadQueued,
                   Uploading, GpuPending, Prepared };
struct Error {
    std::optional<Id> id;
    std::string file, code, message;
    Stage stage{Stage::Queued};
};
struct Progress {
    std::size_t requests{}, registered{}, prepared{};
};

// A provider returns an anonymous immutable payload, never publishes names.
// Holding one protects native use, but does not grant permission to start new
// use after this resource has been explicitly unloaded.
struct Asset {
    virtual ~Asset() = default;
    // Logical unload, on the owner thread. Release active uses such as audio
    // voices immediately; retained GPU commands may still pin the payload.
    virtual void Retire() const noexcept {}
};
struct PollResult {
    Stage stage{Stage::Queued};
    std::shared_ptr<const Asset> asset;
    std::optional<Error> error;
};
class Operation {
public:
    virtual ~Operation() = default;
    // Nonblocking, owner thread. Only Prepared + nonnull asset means ready;
    // CpuReady / submitted uploads / incomplete fences must remain pending.
    virtual PollResult Poll() = 0;
    virtual void Cancel() noexcept = 0;
};
class Provider {
public:
    virtual ~Provider() = default;
    // Nonblocking scheduling only; physical operation belongs to this handle,
    // not to whichever Job first requested it. No worker may touch the registry.
    // The request reference is borrowed for Start only; copy immutable inputs
    // needed by detached work, never retain a reference into a retired Record.
    virtual std::unique_ptr<Operation> Start(Handle, const Request&) = 0;
    // Cancel must be safe with an in-flight operation. Deferred workers/GPU work
    // remain provider-owned until quiescent; operation destruction cannot join.
    // Shutdown may drain/join and is called before registry payloads are released.
    virtual void Shutdown() noexcept = 0;
};

struct Limits {
    std::size_t manifests{128}, activeJobs{64}, records{8192};
    std::size_t declarations{32768}, declarationBytes{16 * 1024 * 1024};
};
struct Budget {
    std::size_t polls{32}, maintenance{64}, commits{8};
};
struct Stats {
    std::size_t manifests{}, activeJobs{}, records{};
    std::size_t declarations{}, declarationBytes{}, pendingRetirement{};
    std::uint64_t starts{}, polls{}, commits{}, cancellations{}, retirements{};
};
namespace detail { struct Core; struct ManifestState; struct Transaction; }
class Service;
class Manifest {
public:
    Manifest& LoadImage(std::string path, std::string name, std::uint32_t x,
        std::uint32_t y, std::uint32_t width, std::uint32_t height,
        ImageOptions options = {});
    Manifest& LoadImage(std::string path, std::string name, ImageOptions options = {});
    Manifest& LoadAudio(std::string path, std::string name, AudioRequest options);
    Manifest& LoadFont(std::string path, std::string name);
    Manifest& LoadShader(std::string path, std::string name);
    bool IsSealed() const;
private:
    friend class Service;
    std::weak_ptr<detail::Core> owner;
    std::shared_ptr<detail::ManifestState> state;
};
class Job {
public:
    State GetState() const;
    bool IsReady() const { return GetState() == State::Ready; }
    bool IsFinished() const;
    bool Cancel();
    Progress GetProgress() const;
    std::optional<Error> GetError() const;
    void CheckResult() const; // Explicit error for pending, failed or cancelled.
private:
    friend class Service;
    std::weak_ptr<detail::Core> owner;
    std::shared_ptr<detail::Transaction> state;
};

class Service {
public:
    explicit Service(std::unique_ptr<Provider>, Limits = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    Manifest CreateManifest();
    // Validates/reserves all names atomically. No I/O, decode or GPU work here.
    Job Submit(Manifest&);
    // Ready assets are owned by this manager, not by Manifest/Job handles.
    // Pending resources cannot be queried or unloaded; this never cancels Jobs.
    // Physical work and payload retirement are deferred to budgeted Tick.
    bool Unload(const Id&);
    bool Unload(const std::string& name);
    void Tick(Budget = {});
    std::optional<Handle> Find(const Id&);
    std::optional<Handle> Find(const std::string& name);
    bool IsResident(Handle);
    std::shared_ptr<const Asset> Acquire(Handle); // Throws on stale/nonresident.
    Request GetInfo(Handle); // Detached normalized declaration; no GPU identity.
    // Owner-only metadata snapshot replacement; already queued draws pin the
    // previous immutable payload. This does not retire the backing resource.
    void ReplaceAsset(Handle, std::shared_ptr<const Asset>);
    Stats GetStats() const;
    void Shutdown(); // Deterministic owner-thread stop; may join/drain providers.
private:
    std::shared_ptr<detail::Core> core;
};
}
