#pragma once
#include "core/modules/Graphic/image/PngDecode.h"
#include "core/runtime/Jobs/Executor.h"
#include <cstdint>
#include <memory>
#include <string>

namespace core::modules::File { class FileSystem; }

namespace core::modules::Resource {
namespace image_detail { struct SourceState; struct Subscription; struct SourceCore; }
enum class SourceStage { Queued, Reading, Decoding, CpuReady, Failed, Cancelled };
struct SourcePixels {
    // Credit outlives pixel allocation. Destruction frees pixels BEFORE making
    // the admitted byte credit available to another worker.
    core::runtime::Jobs::Reservation credit;
    core::modules::Graphic::Image::PngImage image;
    std::string source;
    std::uint64_t generation{}, modified{};
};
class SourceTicket {
public:
    SourceStage GetStage() const;
    bool IsFinished() const;
    bool Cancel(); // This subscription only; last pending subscriber cancels work.
    std::exception_ptr GetError() const;
    std::shared_ptr<const SourcePixels> GetPixels() const; // CpuReady only, not drawable.
private:
    friend class ImageSources;
    std::shared_ptr<image_detail::Subscription> subscription;
};
struct SourceConfig {
    std::size_t capacity{128};
    std::size_t maxFileBytes{32 * 1024 * 1024};
    std::size_t decodeAllocationLimit{128 * 1024 * 1024};
    std::size_t cacheBytes{16 * 1024 * 1024};
    std::uint32_t maxDimension{16384};
    // Required immutable virtual view. Retained by workers and source cache.
    std::shared_ptr<const core::modules::File::FileSystem> files;
};
struct SourceStats {
    std::size_t entries{}, cacheHits{}, reads{}, decodes{}, evictions{};
};
class ImageSources {
public:
    ImageSources(core::runtime::Jobs::Executor&, core::runtime::Jobs::MemoryBudget, SourceConfig = {});
    ~ImageSources();
    ImageSources(const ImageSources&) = delete;
    ImageSources& operator=(const ImageSources&) = delete;
    // Owner thread; no I/O/decode. nullopt is bounded backpressure, not failure.
    std::optional<SourceTicket> TryRequest(std::string path);
    void Collect(std::size_t budget = 8);
    SourceStats GetStats() const;
    void Shutdown(); // Nonblocking; external Executor owns final worker join.
private:
    std::unique_ptr<image_detail::SourceCore> core;
};
// CPU-only region view for Atlas construction. Copy into the final page once,
// instead of allocating another full region before extrusion. A row never
// includes source-sheet gutters. This is not a public Lua Image/Texture view.
struct ImageRegion {
    std::shared_ptr<const SourcePixels> source;
    std::uint32_t x{}, y{}, width{}, height{};
    std::span<const std::uint8_t> Row(std::uint32_t row) const;
};
ImageRegion SelectRegion(std::shared_ptr<const SourcePixels>, std::uint32_t x, std::uint32_t y,
    std::uint32_t width, std::uint32_t height);
}
