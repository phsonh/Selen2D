#include "ReadSource.h"
#include "IoError.h"
#include "Path.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <mutex>

namespace core::modules::File {
struct ReadSource::State {
    std::shared_ptr<SDL_IOStream> stream;
    std::string path;
    std::uint64_t size{};
    std::mutex mutex;
    State(std::shared_ptr<SDL_IOStream> value, std::string name)
        : stream(std::move(value)), path(std::move(name)) {
        const auto length = SDL_GetIOSize(stream.get());
        if(length < 0) throw GetStreamError(stream.get(), "retain source size", path);
        if(static_cast<std::uint64_t>(length) > MaxFilePosition)
            throw IoError("out_of_range", "retain source '" + path + "': out_of_range");
        size = static_cast<std::uint64_t>(length);
        if(SDL_SeekIO(stream.get(), 0, SDL_IO_SEEK_SET) != 0)
            throw GetStreamError(stream.get(), "retain seekable source", path);
    }
};
struct ReadSource::Cursor {
    std::shared_ptr<State> owner;
    std::shared_ptr<StreamErrorState> error{std::make_shared<StreamErrorState>()};
    std::uint64_t position{};
    explicit Cursor(std::shared_ptr<State> source) : owner(std::move(source)) { error->code.reserve(64); }
    void Fail(std::string_view code) noexcept {
        try { error->code.assign(code); } catch(...) {}
        SDL_SetError("Retained source I/O failed: %.*s", static_cast<int>(code.size()), code.data());
    }
    static Sint64 SDLCALL Size(void* value) {
        return static_cast<Sint64>(static_cast<Cursor*>(value)->owner->size);
    }
    static Sint64 SDLCALL Seek(void* value, Sint64 offset, SDL_IOWhence whence) {
        auto& cursor = *static_cast<Cursor*>(value);
        std::uint64_t base{};
        if(whence == SDL_IO_SEEK_CUR) base = cursor.position;
        else if(whence == SDL_IO_SEEK_END) base = cursor.owner->size;
        else if(whence != SDL_IO_SEEK_SET) { cursor.Fail("out_of_range"); return -1; }
        if(offset < -static_cast<Sint64>(MaxFilePosition) || offset > static_cast<Sint64>(MaxFilePosition) ||
            (offset < 0 && static_cast<std::uint64_t>(-offset) > base) ||
            (offset >= 0 && static_cast<std::uint64_t>(offset) > MaxFilePosition - base)) {
            cursor.Fail("out_of_range"); return -1;
        }
        cursor.position = offset < 0 ? base - static_cast<std::uint64_t>(-offset) : base + static_cast<std::uint64_t>(offset);
        return static_cast<Sint64>(cursor.position);
    }
    static std::size_t SDLCALL Read(void* value, void* bytes, std::size_t requested, SDL_IOStatus* status) {
        auto& cursor = *static_cast<Cursor*>(value);
        std::size_t read{};
        try {
            if(cursor.position >= cursor.owner->size) { *status = SDL_IO_STATUS_EOF; return 0; }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(requested, cursor.owner->size - cursor.position));
            // The retained backend may have only one physical cursor. Keep its
            // seek/read pair indivisible while each view owns a logical cursor.
            const std::lock_guard lock(cursor.owner->mutex);
            auto* source = cursor.owner->stream.get();
            if(SDL_SeekIO(source, static_cast<Sint64>(cursor.position), SDL_IO_SEEK_SET) != static_cast<Sint64>(cursor.position))
                throw GetStreamError(source, "seek retained source", cursor.owner->path);
            read = SDL_ReadIO(source, bytes, count);
            cursor.position += read;
            if(SDL_GetIOStatus(source) == SDL_IO_STATUS_ERROR) {
                cursor.Fail(GetStreamError(source, "read retained source", cursor.owner->path).code);
                *status = SDL_IO_STATUS_ERROR;
            } else if(read < count) {
                // EOF is fixed when the capability is captured. A shorter
                // backend now is an I/O failure, not a new successful EOF.
                cursor.Fail("io_error"); *status = SDL_IO_STATUS_ERROR;
            } else *status = read < requested ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
            return read;
        } catch(const IoError& error) { cursor.Fail(error.code); }
        catch(...) { cursor.Fail("io_error"); }
        *status = SDL_IO_STATUS_ERROR;
        return read;
    }
    static std::size_t SDLCALL Write(void* value, const void*, std::size_t, SDL_IOStatus* status) {
        static_cast<Cursor*>(value)->Fail("read_only");
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    static bool SDLCALL Flush(void*, SDL_IOStatus*) { return true; }
    static bool SDLCALL Close(void* value) {
        delete static_cast<Cursor*>(value);
        return true;
    }
};
ReadSource::ReadSource(std::shared_ptr<SDL_IOStream> stream, std::string path)
    : state_(std::make_shared<State>(std::move(stream), std::move(path))) {}
ReadSource::~ReadSource() = default;
std::uint64_t ReadSource::Size() const noexcept { return state_->size; }
SDL_IOStream* ReadSource::Open() const {
    auto cursor = std::make_unique<Cursor>(state_);
    SDL_IOStreamInterface api{}; SDL_INIT_INTERFACE(&api);
    api.size = Cursor::Size; api.seek = Cursor::Seek; api.read = Cursor::Read;
    api.write = Cursor::Write; api.flush = Cursor::Flush; api.close = Cursor::Close;
    auto* stream = SDL_OpenIO(&api, cursor.get());
    if(!stream) throw IoError("io_error", "open retained source '" + state_->path + "': cannot allocate cursor");
    const auto error = cursor->error;
    cursor.release();
    try { SetStreamErrorState(stream, error); }
    catch(...) { SDL_CloseIO(stream); throw; }
    return stream;
}
}
