#include "Package.h"

#include <SDL3/SDL_error.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace core::modules::File {
namespace {
    constexpr std::array<unsigned char, 8> Magic{'S', 'L', 'N', 'G', 'P', 'K', 'G', 0};
    constexpr std::uint32_t Version = 1, HeaderBytes = 64;
    constexpr std::uint32_t MinChunk = 4 * 1024, MaxChunk = 4 * 1024 * 1024;
    constexpr std::uint32_t MaxEntries = 262144, MaxPathBytes = 4096;
    constexpr std::uint64_t MaxIndexBytes = 64 * 1024 * 1024;
    constexpr auto MaxOffset = std::numeric_limits<Sint64>::max();

    [[noreturn]] void Invalid(const char* message) {
        throw std::runtime_error(std::string("Resource package: ") + message);
    }
    void Require(bool condition, const char* message) { if (!condition) Invalid(message); }

    std::string Utf8(const std::filesystem::path& path) {
        const auto value = path.generic_u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    bool IsLink(const std::filesystem::path& path) {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path))) return true;
#ifdef _WIN32
        const auto flags = GetFileAttributesW(path.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES) Invalid("cannot inspect filesystem path");
        return (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        return false;
#endif
    }

    bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& root) {
        auto a = child.begin();
        for (auto b = root.begin(); b != root.end(); ++a, ++b) {
            if (a == child.end()) return false;
#ifdef _WIN32
            if (CompareStringOrdinal(a->c_str(),-1,b->c_str(),-1,TRUE)!=CSTR_EQUAL) return false;
#else
            if (*a != *b) return false;
#endif
        }
        return true;
    }

    bool ValidPath(std::string_view path) {
        if (path.empty() || path.size() > MaxPathBytes) return false;
        // Package keys must already be canonical and addressable through VFS.
        // Share its UTF-8, portable filename and device-alias validation rather
        // than accepting archive entries that Lua can never subsequently open.
        const auto normalized = Normalize(path);
        return normalized && *normalized == path && std::count(path.begin(), path.end(), '/') < 128;
    }

    struct CloseStream {
        void operator()(SDL_IOStream* stream) const noexcept { if (stream) SDL_CloseIO(stream); }
    };
    using Stream = std::unique_ptr<SDL_IOStream, CloseStream>;
    struct FreeDecoder { void operator()(ZSTD_DCtx* value) const noexcept { ZSTD_freeDCtx(value); } };
    struct FreeEncoder { void operator()(ZSTD_CCtx* value) const noexcept { ZSTD_freeCCtx(value); } };

    void ZstdCheck(std::size_t code) {
        if (ZSTD_isError(code))
            throw std::runtime_error(std::string("Resource package Zstd: ") + ZSTD_getErrorName(code));
    }
    void ReadExact(SDL_IOStream* stream, void* data, std::size_t size) {
        if (size && SDL_ReadIO(stream, data, size) != size) Invalid("truncated or unreadable data");
    }
    void WriteExact(SDL_IOStream* stream, const void* data, std::size_t size) {
        if (size && SDL_WriteIO(stream, data, size) != size) Invalid("cannot write package");
    }
    template<class T> T Get(const unsigned char* bytes) {
        T result{};
        for (std::size_t i = 0; i < sizeof(T); ++i) result |= static_cast<T>(bytes[i]) << (i * 8);
        return result;
    }
    template<class T> void Put(unsigned char* bytes, T value) {
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
    }
    struct Cursor {
        const std::vector<unsigned char>& data;
        std::size_t position{};
        const unsigned char* Take(std::size_t size) {
            Require(size <= data.size() - position, "truncated index");
            const auto* result = data.data() + position;
            position += size;
            return result;
        }
        template<class T> T Number() { return Get<T>(Take(sizeof(T))); }
        std::size_t Remaining() const { return data.size() - position; }
    };
    struct Block { std::uint64_t offset; std::uint32_t compressed, decoded; };
    struct PackageEntry {
        NodeType type{NodeType::Directory};
        std::uint64_t size{};
        std::vector<Block> blocks;
    };
    using Entries = std::map<std::string, PackageEntry, std::less<>>;

    void ValidateParents(const Entries& entries, std::string_view path) {
        for (auto slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
            const auto found = entries.find(path.substr(0, slash));
            Require(found != entries.end() && found->second.type == NodeType::Directory,
                "missing directory parent or file/directory conflict");
        }
    }

    void ReportCallbackError(const char* message) noexcept { SDL_SetError("Resource package: %s", message); }
}

struct Package::Impl {
    std::filesystem::path source;
    std::array<unsigned char, HeaderBytes> header{};
    std::uint64_t physicalSize{};
    std::uint32_t chunkBytes{};
    Entries entries;

    struct Reader {
        std::shared_ptr<const Package> owner;
        const PackageEntry* entry{};
        Stream file;
        std::unique_ptr<ZSTD_DCtx, FreeDecoder> decoder{ZSTD_createDCtx()};
        std::vector<unsigned char> compressed, decoded;
        std::size_t cachedBlock = std::numeric_limits<std::size_t>::max();
        Sint64 position{};

        void LoadBlock(std::size_t index) {
            if (index == cachedBlock) return;
            // A failed decode can overwrite the buffer. Never leave an older
            // cache key pointing at partially decoded data after an error.
            cachedBlock = std::numeric_limits<std::size_t>::max();
            const auto& block = entry->blocks.at(index);
            compressed.resize(block.compressed);
            Require(SDL_SeekIO(file.get(), static_cast<Sint64>(block.offset), SDL_IO_SEEK_SET)
                == static_cast<Sint64>(block.offset), "cannot seek payload");
            ReadExact(file.get(), compressed.data(), compressed.size());
            ZSTD_frameHeader frame{};
            const auto headerResult = ZSTD_getFrameHeader(&frame, compressed.data(), compressed.size());
            ZstdCheck(headerResult);
            Require(headerResult == 0 && frame.frameType == ZSTD_frame && frame.dictID == 0
                && frame.checksumFlag == 1 && frame.windowSize <= MaxChunk,
                "invalid, unchecked, dictionary-based or oversized-window Zstd frame");
            Require(ZSTD_getFrameContentSize(compressed.data(), compressed.size()) == block.decoded,
                "Zstd frame size does not match index");
            const auto frameSize = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
            ZstdCheck(frameSize);
            Require(frameSize == compressed.size(), "block must contain exactly one Zstd frame");
            decoded.resize(block.decoded);
            const auto result = ZSTD_decompressDCtx(decoder.get(), decoded.data(), decoded.size(),
                compressed.data(), compressed.size());
            ZstdCheck(result);
            Require(result == decoded.size(), "incorrect decoded block length");
            cachedBlock = index; // Publish only after a successful complete decode.
        }

        static Sint64 SDLCALL Size(void* data) noexcept {
            return static_cast<Sint64>(static_cast<Reader*>(data)->entry->size);
        }
        static Sint64 SDLCALL Seek(void* data, Sint64 offset, SDL_IOWhence whence) noexcept {
            auto& reader = *static_cast<Reader*>(data);
            Sint64 base;
            switch (whence) {
            case SDL_IO_SEEK_SET: base = 0; break;
            case SDL_IO_SEEK_CUR: base = reader.position; break;
            case SDL_IO_SEEK_END: base = static_cast<Sint64>(reader.entry->size); break;
            default: ReportCallbackError("invalid seek origin"); return -1;
            }
            if (offset < -base || offset > MaxOffset - base) {
                ReportCallbackError("seek outside supported byte positions");
                return -1;
            }
            reader.position = base + offset; // Like a disk read stream, seeking past EOF is legal.
            return reader.position;
        }
        static std::size_t SDLCALL Read(void* data, void* output, std::size_t size, SDL_IOStatus* status) noexcept {
            auto& reader = *static_cast<Reader*>(data);
            std::size_t copied = 0;
            try {
                while (copied < size && static_cast<std::uint64_t>(reader.position) < reader.entry->size) {
                    const auto chunk = reader.owner->impl_->chunkBytes;
                    const auto index = static_cast<std::size_t>(reader.position / chunk);
                    const auto offset = static_cast<std::size_t>(reader.position % chunk);
                    reader.LoadBlock(index);
                    const auto take = std::min(size - copied, reader.decoded.size() - offset);
                    std::memcpy(static_cast<unsigned char*>(output) + copied, reader.decoded.data() + offset, take);
                    reader.position += static_cast<Sint64>(take);
                    copied += take;
                }
                if (copied < size) *status = SDL_IO_STATUS_EOF;
            } catch (const std::exception& error) {
                *status = SDL_IO_STATUS_ERROR;
                ReportCallbackError(error.what());
            } catch (...) {
                *status = SDL_IO_STATUS_ERROR;
                ReportCallbackError("unknown read failure");
            }
            return copied;
        }
        static std::size_t SDLCALL Write(void*, const void*, std::size_t, SDL_IOStatus* status) noexcept {
            *status = SDL_IO_STATUS_READONLY;
            ReportCallbackError("package streams are read-only");
            return 0;
        }
        static bool SDLCALL Flush(void*, SDL_IOStatus*) noexcept { return true; }
        static bool SDLCALL Close(void* data) noexcept {
            try {
                std::unique_ptr<Reader> reader(static_cast<Reader*>(data));
                return SDL_CloseIO(reader->file.release());
            } catch (const std::exception& error) { ReportCallbackError(error.what()); }
            catch (...) { ReportCallbackError("unknown close failure"); }
            return false;
        }
    };
};

Package::Package(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Package::~Package() = default;

std::shared_ptr<Package> Package::Load(const std::filesystem::path& source) {
    Require(!IsLink(source) && std::filesystem::is_regular_file(source), "source must be a regular non-link file");
    auto impl = std::make_unique<Impl>();
    impl->source = std::filesystem::absolute(source);
    Stream stream(SDL_IOFromFile(Utf8(impl->source).c_str(), "rb"));
    Require(stream != nullptr, "cannot open source");
    const auto size = SDL_GetIOSize(stream.get());
    Require(size >= HeaderBytes, "header is truncated");
    impl->physicalSize = static_cast<std::uint64_t>(size);
    ReadExact(stream.get(), impl->header.data(), impl->header.size());
    const auto* header = impl->header.data();
    Require(std::equal(Magic.begin(), Magic.end(), header), "invalid magic");
    Require(Get<std::uint32_t>(header + 8) == Version && Get<std::uint32_t>(header + 12) == HeaderBytes,
        "unsupported version or header size");
    impl->chunkBytes = Get<std::uint32_t>(header + 16);
    Require(impl->chunkBytes >= MinChunk && impl->chunkBytes <= MaxChunk, "invalid chunk size");
    const auto count = Get<std::uint32_t>(header + 20);
    const auto indexOffset = Get<std::uint64_t>(header + 24);
    const auto indexSize = Get<std::uint64_t>(header + 32);
    Require(count <= MaxEntries && indexSize <= MaxIndexBytes && count <= indexSize / 33,
        "index exceeds metadata limits");
    Require(indexOffset >= HeaderBytes && indexOffset <= impl->physicalSize
        && indexSize == impl->physicalSize - indexOffset, "index outside package");
    Require(Get<std::uint64_t>(header + 40) == impl->physicalSize
        && Get<std::uint64_t>(header + 48) == 0 && Get<std::uint64_t>(header + 56) == 0,
        "invalid file size or reserved header fields");
    std::vector<unsigned char> index(static_cast<std::size_t>(indexSize));
    Require(SDL_SeekIO(stream.get(), static_cast<Sint64>(indexOffset), SDL_IO_SEEK_SET)
        == static_cast<Sint64>(indexOffset), "cannot seek index");
    ReadExact(stream.get(), index.data(), index.size());
    Cursor cursor{index};
    std::string previous;
    std::uint64_t payloadCursor = HeaderBytes;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto nameBytes = cursor.Number<std::uint32_t>();
        const auto type = cursor.Number<std::uint32_t>();
        const auto logicalSize = cursor.Number<std::uint64_t>();
        const auto modified = cursor.Number<std::uint64_t>();
        const auto blocks = cursor.Number<std::uint32_t>();
        Require(cursor.Number<std::uint32_t>() == 0, "invalid reserved entry field");
        Require(nameBytes > 0 && nameBytes <= MaxPathBytes, "invalid path length");
        const auto* name = cursor.Take(nameBytes);
        std::string path(reinterpret_cast<const char*>(name), nameBytes);
        Require(ValidPath(path) && (previous.empty() || previous < path), "invalid, duplicate or unordered path");
        Require((type == 1 || type == 2) && modified == 0 && logicalSize <= static_cast<std::uint64_t>(MaxOffset),
            "invalid entry fields");
        ValidateParents(impl->entries, path);
        PackageEntry entry;
        entry.type = type == 1 ? NodeType::File : NodeType::Directory;
        entry.size = logicalSize;
        if (entry.type == NodeType::Directory) Require(logicalSize == 0 && blocks == 0, "directory has payload");
        else Require(blocks == (logicalSize + impl->chunkBytes - 1) / impl->chunkBytes, "block count does not match size");
        Require(blocks <= cursor.Remaining() / 16, "truncated block index");
        entry.blocks.reserve(blocks);
        for (std::uint32_t blockIndex = 0; blockIndex < blocks; ++blockIndex) {
            Block block{cursor.Number<std::uint64_t>(), cursor.Number<std::uint32_t>(), cursor.Number<std::uint32_t>()};
            const auto expected = static_cast<std::uint32_t>(std::min<std::uint64_t>(impl->chunkBytes,
                logicalSize - static_cast<std::uint64_t>(blockIndex) * impl->chunkBytes));
            Require(block.decoded == expected && block.compressed > 0
                && block.compressed <= ZSTD_compressBound(block.decoded), "invalid block lengths");
            Require(block.offset == payloadCursor && block.offset <= indexOffset
                && block.compressed <= indexOffset - block.offset, "overlapping or out-of-range block");
            payloadCursor += block.compressed;
            entry.blocks.push_back(block);
        }
        impl->entries.emplace(path, std::move(entry));
        previous = std::move(path);
    }
    Require(cursor.Remaining() == 0 && payloadCursor == indexOffset, "unindexed bytes or trailing index data");
    return std::shared_ptr<Package>(new Package(std::move(impl)));
}

NodeInfo Package::Stat(std::string_view path) const {
    if (path.empty()) return {NodeType::Directory, 0, 0};
    const auto found = impl_->entries.find(path);
    if (found == impl_->entries.end()) return {};
    return {found->second.type, found->second.size, 0};
}

std::optional<std::vector<std::string>> Package::ListDirectory(std::string_view path) const {
    if (Stat(path).type != NodeType::Directory) return std::nullopt;
    std::vector<std::string> result;
    const std::string prefix = path.empty() ? "" : std::string(path) + "/";
    for (auto it = impl_->entries.lower_bound(prefix); it != impl_->entries.end(); ++it) {
        const auto& name = it->first;
        if (!name.starts_with(prefix)) break;
        const auto rest = std::string_view(name).substr(prefix.size());
        if (!rest.empty() && rest.find('/') == std::string_view::npos) result.emplace_back(rest);
    }
    return result;
}

SDL_IOStream* Package::Open(std::string_view path) const {
    try {
        const auto found = impl_->entries.find(path);
        if (found == impl_->entries.end() || found->second.type != NodeType::File) {
            ReportCallbackError("path is not a file");
            return nullptr;
        }
        auto reader = std::make_unique<Impl::Reader>();
        reader->owner = shared_from_this();
        reader->entry = &found->second;
        reader->file.reset(SDL_IOFromFile(Utf8(impl_->source).c_str(), "rb"));
        Require(reader->file != nullptr && reader->decoder != nullptr, "cannot allocate package stream");
        Require(SDL_GetIOSize(reader->file.get()) == static_cast<Sint64>(impl_->physicalSize), "package changed after mount");
        std::array<unsigned char, HeaderBytes> header{};
        ReadExact(reader->file.get(), header.data(), header.size());
        Require(header == impl_->header, "package header changed after mount");
        ZstdCheck(ZSTD_DCtx_setParameter(reader->decoder.get(), ZSTD_d_windowLogMax, 22));
        SDL_IOStreamInterface interface{};
        SDL_INIT_INTERFACE(&interface);
        interface.size = Impl::Reader::Size;
        interface.seek = Impl::Reader::Seek;
        interface.read = Impl::Reader::Read;
        interface.write = Impl::Reader::Write;
        interface.flush = Impl::Reader::Flush;
        interface.close = Impl::Reader::Close;
        if (auto* stream = SDL_OpenIO(&interface, reader.get())) { reader.release(); return stream; }
    } catch (const std::exception& error) { ReportCallbackError(error.what()); }
    catch (...) { ReportCallbackError("unknown open failure"); }
    return nullptr;
}

void BuildPackage(const std::filesystem::path& directory, const std::filesystem::path& output,
    std::uint32_t chunkBytes) {
    Require(chunkBytes >= MinChunk && chunkBytes <= MaxChunk, "chunk size must be 4 KiB through 4 MiB");
    Require(!IsLink(directory) && std::filesystem::is_directory(directory), "source must be a non-link directory");
    const auto root = std::filesystem::canonical(directory);
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(output));
    Require(!IsWithin(destination, root), "output must be outside source tree");
    Require(!std::filesystem::exists(destination), "output already exists");

    struct SourceEntry { std::filesystem::path source; PackageEntry entry; };
    std::map<std::string, SourceEntry, std::less<>> sources;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        Require(!IsLink(item.path()), "symbolic links and reparse points cannot be packaged");
        const auto path = Utf8(item.path().lexically_relative(root));
        Require(ValidPath(path) && sources.size() < MaxEntries, "invalid source path or too many entries");
        PackageEntry entry;
        if (item.is_directory()) entry.type = NodeType::Directory;
        else {
            Require(item.is_regular_file(), "only regular files and directories can be packaged");
            entry.type = NodeType::File;
            entry.size = item.file_size();
            Require(entry.size <= static_cast<std::uint64_t>(MaxOffset), "file exceeds seekable size");
        }
        sources.emplace(path, SourceEntry{item.path(), std::move(entry)});
    }

    // Bound metadata before compressing a potentially very large source tree.
    std::uint64_t indexBytes = 0;
    for (const auto& [path, source] : sources) {
        const auto blocks = (source.entry.size + chunkBytes - 1) / chunkBytes;
        Require(blocks <= MaxIndexBytes / 16, "file exceeds block-index limit");
        indexBytes += 32 + path.size() + blocks * 16;
        Require(indexBytes <= MaxIndexBytes, "index exceeds metadata limit");
    }
    Stream target(SDL_IOFromFile(Utf8(destination).c_str(), "w+bx")); // OS-level exclusive create.
    Require(target != nullptr, "cannot create output exclusively");
    try {
        std::array<unsigned char, HeaderBytes> header{};
        WriteExact(target.get(), header.data(), header.size());
        std::unique_ptr<ZSTD_CCtx, FreeEncoder> encoder(ZSTD_createCCtx());
        Require(encoder != nullptr, "cannot allocate Zstd encoder");
        ZstdCheck(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_compressionLevel, 3));
        ZstdCheck(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_checksumFlag, 1));
        ZstdCheck(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_contentSizeFlag, 1));
        std::vector<unsigned char> raw(chunkBytes), compressed(ZSTD_compressBound(chunkBytes));
        std::uint64_t payloadOffset = HeaderBytes;
        for (auto& [path, source] : sources) {
            if (source.entry.type == NodeType::Directory) continue;
            Stream input(SDL_IOFromFile(Utf8(source.source).c_str(), "rb"));
            Require(input != nullptr && SDL_GetIOSize(input.get()) == static_cast<Sint64>(source.entry.size),
                "source changed or cannot be opened");
            for (std::uint64_t position = 0; position < source.entry.size;) {
                const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunkBytes, source.entry.size - position));
                ReadExact(input.get(), raw.data(), take);
                const auto count = ZSTD_compress2(encoder.get(), compressed.data(), compressed.size(), raw.data(), take);
                ZstdCheck(count);
                Require(payloadOffset <= static_cast<std::uint64_t>(MaxOffset) - count - indexBytes, "package too large");
                WriteExact(target.get(), compressed.data(), count);
                source.entry.blocks.push_back({payloadOffset, static_cast<std::uint32_t>(count), take});
                payloadOffset += count;
                position += take;
            }
            Require(SDL_GetIOSize(input.get()) == static_cast<Sint64>(source.entry.size), "source changed while packing");
        }

        std::vector<unsigned char> index(static_cast<std::size_t>(indexBytes));
        std::size_t cursor = 0;
        for (const auto& [path, source] : sources) {
            auto* record = index.data() + cursor;
            const auto& entry = source.entry;
            Put<std::uint32_t>(record, static_cast<std::uint32_t>(path.size()));
            Put<std::uint32_t>(record + 4, entry.type == NodeType::File ? 1 : 2);
            Put<std::uint64_t>(record + 8, entry.size);
            Put<std::uint64_t>(record + 16, 0);
            Put<std::uint32_t>(record + 24, static_cast<std::uint32_t>(entry.blocks.size()));
            Put<std::uint32_t>(record + 28, 0);
            cursor += 32;
            std::memcpy(index.data() + cursor, path.data(), path.size());
            cursor += path.size();
            for (const auto& block : entry.blocks) {
                Put<std::uint64_t>(index.data() + cursor, block.offset);
                Put<std::uint32_t>(index.data() + cursor + 8, block.compressed);
                Put<std::uint32_t>(index.data() + cursor + 12, block.decoded);
                cursor += 16;
            }
        }
        WriteExact(target.get(), index.data(), index.size());
        std::copy(Magic.begin(), Magic.end(), header.begin());
        Put<std::uint32_t>(header.data() + 8, Version);
        Put<std::uint32_t>(header.data() + 12, HeaderBytes);
        Put<std::uint32_t>(header.data() + 16, chunkBytes);
        Put<std::uint32_t>(header.data() + 20, static_cast<std::uint32_t>(sources.size()));
        Put<std::uint64_t>(header.data() + 24, payloadOffset);
        Put<std::uint64_t>(header.data() + 32, indexBytes);
        Put<std::uint64_t>(header.data() + 40, payloadOffset + indexBytes);
        Require(SDL_SeekIO(target.get(), 0, SDL_IO_SEEK_SET) == 0, "cannot write final package header");
        WriteExact(target.get(), header.data(), header.size());
        Require(SDL_CloseIO(target.release()), "output close failed");
    } catch (...) {
        target.reset();
        std::error_code ignored;
        std::filesystem::remove(destination, ignored); // Only this invocation's exclusively-created file.
        throw;
    }
}
}
