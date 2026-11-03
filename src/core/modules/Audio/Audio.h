#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace core::modules::File { class FileSystem; }
namespace core::runtime::Jobs { class MemoryBudget; class StopToken; }

namespace core::modules::Audio {
enum class StorageMode { Decoded, Stream };
enum class PlayPolicy { Restart, Overlap };
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
struct PlayOptions {
    float volume{1}, pan{};
    std::string bus{"default"};
    PlayPolicy policy{PlayPolicy::Restart};
    bool loop{};
    double position{};
};
struct Position { double seconds{}; const char* basis{"submitted"}; };
namespace detail { struct System; struct ResourceData; struct Record; struct Voice; }
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
};

class Playback final {
public:
    void Pause();
    void Resume();
    void Stop(); // Idempotent; does not overwrite natural Finished state.
    void Destroy(); // Idempotent; control operations subsequently fail.
    void Seek(double seconds); // Asynchronous prime; preserves paused intent.
    void SetVolume(float volume);
    float GetVolume() const;
    void SetPan(float pan);
    float GetPan() const;
    PlaybackState GetState() const;
    Position GetPosition() const; // Submitted source cursor, never a speaker clock.
    std::string GetError() const; // Empty means no error.
private:
    explicit Playback(std::shared_ptr<detail::Voice> voice);
    std::shared_ptr<detail::Voice> voice_;
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
    std::shared_ptr<Playback> Play(const std::shared_ptr<Source>& source, const PlayOptions& options = {});
    void StopSource(const std::shared_ptr<Source>& source);
    void PauseSource(const std::shared_ptr<Source>& source);
    void ResumeSource(const std::shared_ptr<Source>& source);
    void SetMasterVolume(float volume);
    float GetMasterVolume() const;
    void SetBusVolume(const std::string& bus, double volume);
    double GetBusVolume(const std::string& bus) const;
    // Optional native maintenance hint. Priming, refill, completion and reclaim
    // all advance on the service worker without a game-frame pump or Getter.
    void Update();
    // Owner-thread boundary: consumes one retained asynchronous error and
    // throws with its virtual source path. A reclaimed voice cannot hide it.
    void CheckErrors();
    std::size_t GetActivePlaybackCount() const; // C++ diagnostic, not required Lua API.
private:
    void CheckSource(const std::shared_ptr<Source>& source) const;
    std::shared_ptr<const File::FileSystem> files_;
    std::shared_ptr<detail::System> system_;
    friend struct testing::Access;
};
}
