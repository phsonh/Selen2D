#include "Font.h"
#include "core/modules/File/Vfs.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H
#include FT_FONT_FORMATS_H
#include FT_MODULE_H
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace core::modules::Resource::Fonts {
namespace {
namespace Jobs = core::runtime::Jobs;
constexpr std::size_t MaxFileBytes = 32 * 1024 * 1024;
constexpr std::size_t ParserBytes = 32 * 1024 * 1024;

struct BytesOwner {
    Jobs::Reservation credit; // Destroy string storage before returning credit.
    std::string bytes;
};

// FreeType calls these C functions on this one preparation task. Rejecting an
// allocation, including a realloc's overlapping old/new blocks, never throws
// through FreeType and never waits while holding a partial memory reservation.
struct ParserMemory {
    struct alignas(std::max_align_t) Block { std::size_t size; };
    const Jobs::StopToken& stop;
    std::size_t used{};
    bool limitReached{};
    FT_MemoryRec_ callbacks{this, Allocate, Free, Reallocate};

    static void* Allocate(FT_Memory memory, long size) noexcept {
        auto& self = *static_cast<ParserMemory*>(memory->user);
        if (size <= 0 || self.stop.IsStopRequested()) return nullptr;
        const auto amount = static_cast<std::size_t>(size);
        if (amount > ParserBytes - sizeof(Block) || amount + sizeof(Block) > ParserBytes - self.used) {
            self.limitReached = true;
            return nullptr;
        }
        auto* block = static_cast<Block*>(std::malloc(sizeof(Block) + amount));
        if (!block) return nullptr;
        block->size = amount;
        self.used += sizeof(Block) + amount;
        return block + 1;
    }
    static void Free(FT_Memory memory, void* pointer) noexcept {
        if (!pointer) return;
        auto& self = *static_cast<ParserMemory*>(memory->user);
        auto* block = static_cast<Block*>(pointer) - 1;
        self.used -= sizeof(Block) + block->size;
        std::free(block);
    }
    static void* Reallocate(FT_Memory memory, long, long size, void* pointer) noexcept {
        if (size <= 0) { Free(memory, pointer); return nullptr; }
        void* replacement = Allocate(memory, size);
        if (!replacement) return nullptr; // Caller still owns the old block.
        if (pointer) {
            const auto oldSize = (static_cast<Block*>(pointer) - 1)->size;
            std::memcpy(replacement, pointer, std::min(oldSize, static_cast<std::size_t>(size)));
            Free(memory, pointer);
        }
        return replacement;
    }
};
struct FreeType {
    ParserMemory memory;
    FT_Library library{};
    FT_Face face{};
    explicit FreeType(const Jobs::StopToken& stop) : memory{stop} {}
    ~FreeType() {
        if (face) FT_Done_Face(face);
        if (library) FT_Done_Library(library);
    }
    void Check(FT_Error error) const {
        memory.stop.Check();
        if (!error) return;
        if (memory.limitReached) throw std::runtime_error("font parser exceeded its 32 MiB allocation limit");
        const auto* reason = FT_Error_String(error);
        throw std::runtime_error("FreeType font preparation failed (" + std::to_string(error) + "): " +
            (reason ? reason : "invalid or unsupported font"));
    }
};

Format InspectSignature(std::string_view prefix) {
    if (prefix.size() < 12) throw std::runtime_error("truncated SFNT font header");
    if (prefix.substr(0, 4) == "OTTO") return Format::OpenTypeCff;
    if (prefix.substr(0, 4) == std::string_view("\0\1\0\0", 4) || prefix.substr(0, 4) == "true")
        return Format::TrueType;
    // This is only format admission, not a home-grown table/glyph validator.
    // FreeType below parses the actual SFNT tables and prepares the font face.
    throw std::runtime_error("expected a standalone TTF or OTF font; collections, WOFF and bitmap formats are not supported");
}
}

std::shared_ptr<const FontAsset> Prepare(std::shared_ptr<const File::FileSystem> files,
    std::string_view path, Jobs::MemoryBudget memory, const Jobs::StopToken& stop) {
    stop.Check();
    if (!files) throw std::invalid_argument("font preparation requires a VFS service");
    try {
        auto owner = std::make_shared<BytesOwner>();
        Format format{};
        File::ReadOptions options;
        options.maxBytes = MaxFileBytes;
        options.prefixBytes = 12;
        options.cancelled = [&] { return stop.IsStopRequested(); };
        options.admit = [&](std::uint64_t size, std::string_view prefix) {
            stop.Check();
            format = InspectSignature(prefix);
            auto credit = memory.TryAcquire(static_cast<std::size_t>(size) + 1 + ParserBytes, stop);
            // Earlier fonts can retain their bytes until the whole Loader
            // publishes. Waiting for those reservations here can deadlock an
            // atomic batch; lack of headroom is a recoverable load failure.
            if (!credit) throw std::runtime_error("font memory budget has insufficient available capacity");
            owner->credit = std::move(*credit);
        };
        auto snapshot = files->ReadSnapshot(path, options);
        owner->bytes = std::move(snapshot.bytes);
        stop.Check();
        auto result = std::make_shared<FontAsset>();
        {
            FreeType parser(stop);
            parser.Check(FT_New_Library(&parser.memory.callbacks, &parser.library));
            FT_Add_Default_Modules(parser.library);
            parser.Check(FT_New_Memory_Face(parser.library,
                reinterpret_cast<const FT_Byte*>(owner->bytes.data()),
                static_cast<FT_Long>(owner->bytes.size()), 0, &parser.face));
            const auto* actualFormat = FT_Get_Font_Format(parser.face);
            const auto expectedFormat = format == Format::TrueType ? "TrueType" : "CFF";
            if (!FT_IS_SFNT(parser.face) || !FT_IS_SCALABLE(parser.face) || !actualFormat ||
                std::strcmp(actualFormat, expectedFormat) != 0 || parser.face->num_faces != 1 ||
                parser.face->num_glyphs <= 0 ||
                static_cast<unsigned long>(parser.face->num_glyphs) > std::numeric_limits<std::uint32_t>::max() ||
                !parser.face->units_per_EM)
                throw std::runtime_error("font is not a usable standalone scalable TTF/OTF face");
            result->format = format;
            result->glyphCount = static_cast<std::uint32_t>(parser.face->num_glyphs);
            result->unitsPerEm = parser.face->units_per_EM;
        } // All parser memory is gone before returning its temporary credit.
        stop.Check();
        owner->credit.Shrink(owner->bytes.capacity() + 1);
        result->bytes = std::shared_ptr<const std::string>(owner, &owner->bytes);
        return result;
    } catch (const Jobs::Cancelled&) { throw; }
    catch (const File::ReadCancelled&) { throw Jobs::Cancelled(); }
    catch (const std::exception& error) {
        throw std::runtime_error("font '" + std::string(path) + "': " + error.what());
    }
}
}
