#pragma once
#include "Resource.h"
#include "core/runtime/Jobs/Executor.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace core::modules::File { class FileSystem; }
namespace core::modules::Resource::Fonts {
enum class Format { TrueType, OpenTypeCff };

// CPU-prepared font resource only. No glyph rasterization, font size, Graphics
// dependency or live FreeType Face is exposed/shared between worker threads.
struct FontAsset final : Asset {
    // The shared byte owner also retains resident memory credit. A consumer
    // retaining only bytes therefore cannot outlive that accounting reservation.
    std::shared_ptr<const std::string> bytes;
    Format format{};
    std::uint32_t glyphCount{};
    std::uint16_t unitsPerEm{};
};

// Worker-side VFS load and FreeType preparation. Input is limited to 32 MiB;
// reserve input + bounded parser allocations before the full read begins.
// Insufficient available budget fails without waiting on resident font bytes.
// Errors include the virtual path; cancellation propagates as Jobs::Cancelled.
std::shared_ptr<const FontAsset> Prepare(
    std::shared_ptr<const File::FileSystem> files, std::string_view path,
    runtime::Jobs::MemoryBudget memory, const runtime::Jobs::StopToken& stop);
}
