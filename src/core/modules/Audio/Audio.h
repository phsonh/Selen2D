#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::modules::File { class FileSystem; }
namespace core::runtime::Jobs { class MemoryBudget; class StopToken; }

namespace core::modules::Audio {
enum class StorageMode { Decoded, Stream };
enum class PlaybackState { Starting, Playing, Paused, Stopped, Finished, Failed, Destroyed };
struct LoopRange { std::uint64_t begin{}, end{}; }; // Source frames, end exclusive.
struct LoopSeconds { double start{}; std::optional<double> finish; };
struct LoadOptions {
    StorageMode mode; // Explicit, no default mode inferred from a filename.
    std::optional<LoopSeconds> loop;
};
struct ResourceInfo {
    int sampleRate{}, channels{}; // Original source channels (1 or 2).
    std::uint64_t frames{};
    double duration{};
    StorageMode mode{};
    std::uint64_t loopStartFrame{}, loopEndFrame{}, generation{};
};
namespace detail { struct System; struct ResourceData; struct Record; struct Voice; struct PlayerState; }
namespace testing { struct Access; }

// Immutable CPU-side preparation; Prepare needs no playback device or window.
class PreparedAudio final {
public:
    ResourceInfo GetInfo() const;
private:
    explicit PreparedAudio(std::shared_ptr<const detail::ResourceData> data);
    std::shared_ptr<const detail::ResourceData> data_;
    friend class AudioSystem;
};

// Anonymous resident audio. Resource owns names and holds this source; voices
// keep its immutable data alive until their decoder/callback has detached.
class Source final {
public:
    ResourceInfo GetInfo() const;
    void SetVolume(double volume);
    double GetVolume() const;
    void Retire() noexcept; // Stops every voice, permanently rejects new plays.
private:
    explicit Source(std::shared_ptr<detail::Record> record);
    std::shared_ptr<detail::Record> record_;
    friend class AudioSystem;
    friend class Player;
    friend struct testing::Access;
};

// A player mixes independent resource generations. Lua resolves names through
// Resource before these operations; no native method opens a path or re-loads.
class Player final {
public:
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    void Play(const std::string& name, const std::shared_ptr<Source>& source);
    void Stop(const std::shared_ptr<Source>& source = {});
    void Pause(const std::shared_ptr<Source>& source = {});
    void Resume(const std::shared_ptr<Source>& source = {});
    void SetVolume(double volume);
    double GetVolume() const;
    void SetPan(double pan);
    double GetPan() const;
    std::vector<std::string> GetPlaying() const;
    bool IsPlaying(const std::shared_ptr<Source>& source) const;
    void Destroy() noexcept;
    bool IsDestroyed() const noexcept;
private:
    explicit Player(std::shared_ptr<detail::PlayerState> state);
    std::shared_ptr<detail::PlayerState> state_;
    friend class AudioSystem;
    friend struct testing::Access;
};

class AudioSystem final {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    // Lifecycle and source creation belong to the owner thread. Playback
    // control is serialized and may race decode, never calls Lua from callbacks.
    // Configure the host's immutable virtual view before Initialize. No audio
    // path falls back to the process working directory or a physical filename.
    void SetFileSystem(std::shared_ptr<const File::FileSystem> files);
    void Initialize();
    void Shutdown();
    bool IsInitialized() const;
    static PreparedAudio Prepare(std::shared_ptr<const File::FileSystem> files,
        const std::string& path, const LoadOptions& options);
    // Background preparation: finite PCM admission with no budget wait. The
    // credit stays with the PCM; cancellation is checked between decode blocks.
    static PreparedAudio Prepare(std::shared_ptr<const File::FileSystem> files,
        const std::string& path, const LoadOptions& options,
        core::runtime::Jobs::MemoryBudget memory, const core::runtime::Jobs::StopToken& stop);
    std::shared_ptr<Source> CreateSource(const PreparedAudio& prepared);
    std::shared_ptr<Player> CreatePlayer();
    // Entry uses zero. The host publishes Clock::FrameCount immediately before
    // dispatching a frame callback; neither audio work nor queries advance it.
    void SetRuntimeFrame(std::uint64_t frame);
    // Revoke all players and join in-flight decoder work before Resource/VFS
    // teardown. Device closure remains in Shutdown; this method is idempotent.
    void StopAllPlayersAndDrain();
    // Optional native maintenance hint. Priming, refill, completion and reclaim
    // all advance on the service worker without a game-frame pump or Getter.
    void Update();
    // Owner-thread boundary: consumes one retained asynchronous error and
    // throws with its virtual source path. A reclaimed voice cannot hide it.
    void CheckErrors();
    // Owner-thread shutdown boundary, after consumers/workers have stopped.
    // Consumes every queued error and overflow summary without allocating or
    // throwing; calls the nonthrowing sink outside the control lock. Returns
    // whether an error was reported. A null sink leaves the queue unchanged.
    // Not a Lua API, and never invokes Lua.
    bool DrainErrors(void (*sink)(std::string_view, void*) noexcept, void* context = nullptr) noexcept;
    std::size_t GetActivePlaybackCount() const; // C++ diagnostic, not required Lua API.
private:
    std::shared_ptr<const File::FileSystem> files_;
    std::shared_ptr<detail::System> system_;
    friend struct testing::Access;
};
}
