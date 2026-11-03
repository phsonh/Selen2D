#include "File.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

namespace core::modules::File {
    namespace {
        std::string Path(std::string_view value) {
            if (value.empty() || value.find('\0') != std::string_view::npos)
                throw std::invalid_argument("file path must be non-empty UTF-8 without NUL bytes");
            return std::string(value);
        }
        [[noreturn]] void Fail(const char* operation, const std::string& path) {
            throw std::runtime_error(std::string(operation) + " '" + path + "': " + SDL_GetError());
        }
        struct CloseIO { void operator()(SDL_IOStream* stream) const { SDL_CloseIO(stream); } };
        using Stream = std::unique_ptr<SDL_IOStream, CloseIO>;
        void CheckCancelled(const ReadOptions& options) {
            if (options.cancelled && options.cancelled()) throw ReadCancelled();
        }
        [[noreturn]] void Changed(const std::string& path) {
            throw std::runtime_error("file changed during snapshot read '" + path + "'");
        }
        SDL_PathInfo SnapshotInfo(const std::string& path, const ReadOptions& options) {
            CheckCancelled(options);
            SDL_PathInfo info{};
            if (!SDL_GetPathInfo(path.c_str(), &info)) Fail("stat snapshot", path);
            CheckCancelled(options);
            if (info.type != SDL_PATHTYPE_FILE)
                throw std::runtime_error("snapshot source is not a regular file '" + path + "'");
            return info;
        }
        std::uint64_t SnapshotSize(SDL_IOStream* stream, const std::string& path, const ReadOptions& options) {
            CheckCancelled(options);
            const auto size = SDL_GetIOSize(stream);
            if (size < 0) Fail("size snapshot", path);
            CheckCancelled(options);
            return static_cast<std::uint64_t>(size);
        }
        void CheckUnchanged(SDL_IOStream* stream, const std::string& path,
            const SDL_PathInfo& before, const ReadOptions& options) {
            if (SnapshotSize(stream, path, options) != before.size) Changed(path);
            const auto after = SnapshotInfo(path, options);
            if (after.size != before.size || after.modify_time != before.modify_time) Changed(path);
        }
        void ReadExact(SDL_IOStream* stream, char* destination, std::size_t size,
            const std::string& path, const ReadOptions& options) {
            std::size_t done{};
            while (done < size) {
                const auto wanted = std::min(options.chunkBytes, size - done);
                CheckCancelled(options);
                const auto count = SDL_ReadIO(stream, destination + done, wanted);
                const auto status = SDL_GetIOStatus(stream);
                // Capture SDL's per-thread error before a caller-provided
                // cancellation predicate has an opportunity to alter it.
                const auto error = status == SDL_IO_STATUS_ERROR ? std::string(SDL_GetError()) : std::string{};
                CheckCancelled(options);
                if (status == SDL_IO_STATUS_ERROR)
                    throw std::runtime_error("read snapshot '" + path + "': " + error);
                if (!count || (count != wanted && status == SDL_IO_STATUS_EOF)) Changed(path);
                if (count > wanted) throw std::runtime_error("invalid snapshot stream read count");
                done += count;
            }
        }
    }
    Snapshot ReadSnapshot(std::string_view filename, const ReadOptions& options) {
        if (!options.maxBytes || !options.chunkBytes || options.chunkBytes > 16 * 1024 * 1024 ||
            options.prefixBytes > 4096)
            throw std::invalid_argument("invalid file snapshot read limits");
        CheckCancelled(options);
        const auto path = Path(filename);
        const auto before = SnapshotInfo(path, options);
        // Reject an oversized source before either prefix I/O or the large heap
        // allocation. An opened-file size check below also closes stat/open races.
        if (before.size > options.maxBytes || before.size > std::string{}.max_size())
            throw std::length_error("file snapshot exceeds byte limit '" + path + "'");
        CheckCancelled(options);
        Stream stream(SDL_IOFromFile(path.c_str(), "rb"));
        if (!stream) Fail("open snapshot", path);
        CheckCancelled(options);
        if (SnapshotSize(stream.get(), path, options) != before.size) Changed(path);
        const auto size = static_cast<std::size_t>(before.size);
        const auto prefix_size = std::min(size, options.prefixBytes);
        std::array<char, 4096> prefix;
        ReadExact(stream.get(), prefix.data(), prefix_size, path, options);
        CheckCancelled(options);
        if (options.admit) options.admit(before.size, std::string_view(prefix.data(), prefix_size));
        CheckCancelled(options);
        CheckUnchanged(stream.get(), path, before, options);

        Snapshot result;
        result.bytes.resize(size); // Memory admission must already have succeeded.
        CheckCancelled(options);
        std::copy_n(prefix.data(), prefix_size, result.bytes.data());
        ReadExact(stream.get(), result.bytes.data() + prefix_size, size - prefix_size, path, options);
        // Even an exactly filled buffer must not silently truncate a source that
        // grew while being read. This byte is never added to the snapshot.
        CheckCancelled(options);
        char extra{};
        const auto extra_count = SDL_ReadIO(stream.get(), &extra, 1);
        const auto extra_status = SDL_GetIOStatus(stream.get());
        const auto extra_error = extra_status == SDL_IO_STATUS_ERROR ? std::string(SDL_GetError()) : std::string{};
        CheckCancelled(options);
        if (extra_status == SDL_IO_STATUS_ERROR)
            throw std::runtime_error("finish snapshot '" + path + "': " + extra_error);
        if (extra_count) Changed(path);
        if (extra_status != SDL_IO_STATUS_EOF)
            throw std::runtime_error("snapshot source did not reach EOF '" + path + "'");
        CheckUnchanged(stream.get(), path, before, options);
        CheckCancelled(options);
        if (!SDL_CloseIO(stream.release())) Fail("close snapshot", path);
        CheckCancelled(options);
        result.modified = static_cast<std::uint64_t>(before.modify_time);
        return result;
    }
}
