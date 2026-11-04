#include "File.h"
#include "IoError.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace core::modules::File {
    std::string IoErrorCode(const std::error_code& error) {
#ifdef _WIN32
        if(error.category()==std::system_category()) switch(error.value()) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return "not_found";
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return "already_exists";
        case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return "permission_denied";
        case ERROR_WRITE_PROTECT: return "read_only";
        case ERROR_DIRECTORY: return "not_directory";
        case ERROR_DIR_NOT_EMPTY: return "directory_not_empty";
        case ERROR_NOT_SAME_DEVICE: return "cross_device";
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: case ERROR_DISK_QUOTA_EXCEEDED: return "no_space";
        case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY: return "resource_busy";
        case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: return "unsupported";
        case ERROR_CRC: case ERROR_FILE_CORRUPT: return "corrupt_data";
        default: break;
        }
#endif
        const auto condition=error.default_error_condition();
        if(condition==std::errc::no_such_file_or_directory) return "not_found";
        if(condition==std::errc::file_exists) return "already_exists";
        if(condition==std::errc::permission_denied||condition==std::errc::operation_not_permitted) return "permission_denied";
        if(condition==std::errc::read_only_file_system) return "read_only";
        if(condition==std::errc::not_a_directory) return "not_directory";
        if(condition==std::errc::is_a_directory) return "is_directory";
        if(condition==std::errc::directory_not_empty) return "directory_not_empty";
        if(condition==std::errc::cross_device_link) return "cross_device";
        if(condition==std::errc::no_space_on_device) return "no_space";
        if(condition==std::errc::device_or_resource_busy||condition==std::errc::text_file_busy) return "resource_busy";
        if(condition==std::errc::value_too_large||condition==std::errc::result_out_of_range) return "out_of_range";
        if(condition==std::errc::not_supported||condition==std::errc::function_not_supported) return "unsupported";
        return "io_error";
    }
    namespace {
        constexpr const char* ErrorStateProperty="engine.io.error-state";
        using ErrorStateOwner=std::shared_ptr<StreamErrorState>;
        void SDLCALL DeleteErrorState(void*,void* value) { delete static_cast<ErrorStateOwner*>(value); }
        std::string SafeIoMessage(std::string_view operation,std::string_view path,std::string_view code) {
            std::string message(operation);
            if(!path.empty()) { message+=" '"; message+=path; message+='\''; }
            message+=": "; message+=code;
            return message;
        }
    }
    void SetStreamErrorState(SDL_IOStream* stream,const std::shared_ptr<StreamErrorState>& state) {
        auto* owner=new ErrorStateOwner(state);
        if(!SDL_SetPointerPropertyWithCleanup(SDL_GetIOProperties(stream),ErrorStateProperty,owner,DeleteErrorState,nullptr)) {
            // SDL runs cleanup even if installing the property fails.
            throw IoError("io_error","initialize file error state: io_error");
        }
    }
    IoError GetStreamError(SDL_IOStream* stream,std::string_view operation,std::string_view path) {
        const auto props=stream?SDL_GetIOProperties(stream):0;
        const auto* state=static_cast<ErrorStateOwner*>(SDL_GetPointerProperty(props,ErrorStateProperty,nullptr));
        const std::string code=state?(*state)->code:SDL_GetStringProperty(props,StreamErrorCodeProperty,"io_error");
        return IoError(code,SafeIoMessage(operation,path,code));
    }
    bool CloseStream(SDL_IOStream* stream,std::string_view path) {
        if(!stream) throw std::invalid_argument("file is already closed");
        const auto* stored=static_cast<ErrorStateOwner*>(SDL_GetPointerProperty(SDL_GetIOProperties(stream),ErrorStateProperty,nullptr));
        const ErrorStateOwner state=stored?*stored:ErrorStateOwner{};
        const auto fallback=GetStreamError(stream,"close",path);
        if(!SDL_CloseIO(stream)) {
            const auto code=state?state->code:fallback.code;
            throw IoError(code,SafeIoMessage("close",path,code));
        }
        return true;
    }
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
