#pragma once
#include <lua.hpp>
#include <SDL3/SDL_iostream.h>
#include <cstdint>
#include <memory>
#include <string>
namespace core::modules::File { class FileSystem; }
namespace lua_bind::modules::File::detail {
// Binding-private ownership, not a Lua API. Allocate and install GC before
// opening a stream, so a partially constructed file is always safe to collect.
struct Handle {
    static constexpr std::uint64_t Tag = 0x5346494c45484e44ULL;
    std::uint64_t tag{Tag};
    std::shared_ptr<const core::modules::File::FileSystem> filesystem;
    SDL_IOStream* stream{};
    std::string path;
    bool readable{}, writable{};
};
Handle& PushHandle(lua_State* L);
}
