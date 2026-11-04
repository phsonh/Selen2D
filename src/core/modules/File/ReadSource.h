#pragma once
#include <SDL3/SDL_iostream.h>
#include <cstdint>
#include <memory>
#include <string>

namespace core::modules::File {
class FileSystem;
// Internal capability for a source already authorized and opened by the VFS.
// It retains that actual stream; creating a cursor never reopens a pathname.
class ReadSource final {
public:
    ~ReadSource();
    ReadSource(const ReadSource&) = delete;
    ReadSource& operator=(const ReadSource&) = delete;
    SDL_IOStream* Open() const; // Caller owns an independent read-only cursor.
    std::uint64_t Size() const noexcept;
private:
    friend class FileSystem;
    ReadSource(std::shared_ptr<SDL_IOStream> stream, std::string virtualPath);
    struct State;
    struct Cursor;
    std::shared_ptr<State> state_;
};
}
