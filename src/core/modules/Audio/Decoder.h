#pragma once
#include "Audio.h"
#include "core/runtime/Jobs/Executor.h"
#include <span>
#include <vector>

namespace core::modules::Audio::detail {
struct ResourceData {
    ResourceInfo info;
    // Members are destroyed in reverse declaration order: PCM storage is freed
    // before this resident allocation credit returns to the shared budget.
    core::runtime::Jobs::Reservation credit;
    // Streaming playback opens a fresh independent cursor from this exact
    // mount snapshot, even after the loading caller releases its reference.
    std::shared_ptr<const File::FileSystem> files;
    std::string path;
    std::vector<float> pcm; // Decoded only, immutable interleaved F32 stereo.
};
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual ResourceInfo GetInfo() const = 0;
    virtual void Seek(std::uint64_t frame) = 0;
    // Always F32 stereo, including mono sources. Return source PCM frame count.
    virtual std::size_t Read(std::span<float> destination) = 0;
};
std::unique_ptr<Decoder> OpenDecoder(std::shared_ptr<const File::FileSystem> files, const std::string& path);
std::shared_ptr<const ResourceData> PrepareResource(std::shared_ptr<const File::FileSystem> files,
    const std::string& path, const LoadOptions& options);
std::shared_ptr<const ResourceData> PrepareResource(std::shared_ptr<const File::FileSystem> files,
    const std::string& path, const LoadOptions& options,
    core::runtime::Jobs::MemoryBudget memory, const core::runtime::Jobs::StopToken& stop);
class LoopingReader final {
public:
    explicit LoopingReader(Decoder& decoder);
    void SetLoop(std::optional<LoopRange> range);
    void Reset(std::uint64_t frame);
    std::size_t Read(std::span<float> destination);
    std::uint64_t GetPosition() const { return position_; }
private:
    Decoder& decoder_;
    ResourceInfo info_;
    std::optional<LoopRange> loop_;
    std::uint64_t position_{};
};
}
