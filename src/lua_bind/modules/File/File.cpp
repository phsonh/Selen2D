#include "File.h"
#include "Handle.h"
#include "core/modules/File/Vfs.h"
#include "core/runtime/Lua/VfsScripts.h"
#include "lua_bind/Guard.h"
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace lua_bind::modules::File {
namespace {
namespace Native = core::modules::File;
constexpr const char* contextType = "solongo.file.context";
constexpr const char* handleType = "solongo.file.handle";
char contextKey;
struct Context {
    static constexpr std::uint64_t Tag = 0x5346494c45435458ULL;
    std::uint64_t tag{Tag};
    std::shared_ptr<const Native::FileSystem> filesystem;
};
using detail::Handle;

template<class T> T* TestUserdata(lua_State* L, int index, const char* type) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(T)) return nullptr;
    auto* value = static_cast<T*>(luaL_testudata(L, index, type));
    return value && value->tag == T::Tag ? value : nullptr;
}

void Count(lua_State* L, int minimum, int maximum) {
    if (lua_gettop(L) < minimum || lua_gettop(L) > maximum) throw std::invalid_argument("wrong File argument count");
}
Context& FileContext(lua_State* L) {
    auto* context = TestUserdata<Context>(L, lua_upvalueindex(1), contextType);
    if (!context || !context->filesystem) throw std::runtime_error("File service is unavailable");
    return *context;
}
const Native::FileSystem& Filesystem(lua_State* L) { return *FileContext(L).filesystem; }
Handle& OpenHandle(lua_State* L) {
    auto* handle = TestUserdata<Handle>(L, 1, handleType);
    if (!handle) throw std::invalid_argument("File method requires a file handle");
    if (!handle->stream) throw std::invalid_argument("file is already closed");
    return *handle;
}
double Integer(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TNUMBER) throw std::invalid_argument("file byte offset/size must be an integer");
    const double value = lua_tonumber(L, index);
    if (!std::isfinite(value) || value != std::floor(value)) throw std::invalid_argument("file byte offset/size must be a finite integer");
    return value;
}
int Failure(lua_State* L) { lua_pushnil(L); lua_pushboolean(L, false); return 2; }
int Bytes(lua_State* L, const std::string& bytes) {
    lua_pushlstring(L, bytes.data(), bytes.size()); lua_pushboolean(L, true); return 2;
}
int Read(lua_State* L) {
    Count(L, 1, 2);
    auto& handle = OpenHandle(L);
    std::size_t remaining = std::numeric_limits<std::size_t>::max();
    if (!lua_isnoneornil(L, 2)) {
        const auto size = Integer(L, 2);
        if (size < 0 || size >= std::ldexp(1., std::numeric_limits<std::size_t>::digits))
            throw std::invalid_argument("read size is outside the supported byte range");
        remaining = static_cast<std::size_t>(size);
    }
    if (!handle.readable) return Failure(L);
    std::string output;
    std::array<char, 64 * 1024> buffer{};
    while (remaining > 0) {
        const auto wanted = std::min(remaining, buffer.size());
        const auto count = SDL_ReadIO(handle.stream, buffer.data(), wanted);
        if (SDL_GetIOStatus(handle.stream) == SDL_IO_STATUS_ERROR) return Failure(L);
        output.append(buffer.data(), count);
        remaining -= count;
        if (count < wanted) {
            const auto status = SDL_GetIOStatus(handle.stream);
            if (status == SDL_IO_STATUS_EOF) break;
            if (status != SDL_IO_STATUS_READY || count == 0) return Failure(L);
        }
    }
    return Bytes(L, output);
}
enum class LineStatus { Line, End, Error };
LineStatus NextLine(Handle& handle, std::string& line) {
    line.clear();
    if (!handle.readable) return LineStatus::Error;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = SDL_ReadIO(handle.stream, buffer.data(), buffer.size());
        if (SDL_GetIOStatus(handle.stream) == SDL_IO_STATUS_ERROR) return LineStatus::Error;
        const auto newline = std::find(buffer.begin(), buffer.begin() + count, '\n');
        if (newline != buffer.begin() + count) {
            const auto used = static_cast<std::size_t>(newline - buffer.begin()) + 1;
            line.append(buffer.data(), used - 1);
            // Return surplus bytes to the seekable VFS stream. Mixed Read and
            // ReadLine calls, Tell and writes therefore share one exact cursor.
            if (used < count && SDL_SeekIO(handle.stream, -static_cast<Sint64>(count - used), SDL_IO_SEEK_CUR) < 0)
                return LineStatus::Error;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineStatus::Line;
        }
        line.append(buffer.data(), count);
        if (count < buffer.size()) {
            const auto status = SDL_GetIOStatus(handle.stream);
            if (status == SDL_IO_STATUS_EOF) return line.empty() ? LineStatus::End : LineStatus::Line;
            if (status != SDL_IO_STATUS_READY || count == 0) return LineStatus::Error;
        }
    }
}
int ReadLine(lua_State* L) {
    Count(L, 1, 1);
    std::string line;
    switch (NextLine(OpenHandle(L), line)) {
    case LineStatus::Line: return Bytes(L, line);
    case LineStatus::End: lua_pushnil(L); lua_pushboolean(L, true); return 2;
    case LineStatus::Error: return Failure(L);
    }
    return Failure(L);
}
int ReadLines(lua_State* L) {
    Count(L, 1, 1);
    auto& handle = OpenHandle(L);
    lua_newtable(L);
    std::string line;
    int index = 0;
    for (;;) {
        const auto status = NextLine(handle, line);
        if (status == LineStatus::Error) { lua_pop(L, 1); return Failure(L); }
        if (status == LineStatus::End) break;
        if (index == std::numeric_limits<int>::max()) throw std::runtime_error("too many lines for a Lua array");
        lua_pushlstring(L, line.data(), line.size());
        lua_rawseti(L, -2, ++index);
    }
    lua_pushboolean(L, true);
    return 2;
}
int Write(lua_State* L) {
    Count(L, 2, 2);
    auto& handle = OpenHandle(L);
    const auto bytes = CheckString(L, 2);
    const bool success = handle.writable && (bytes.empty() ||
        (SDL_WriteIO(handle.stream, bytes.data(), bytes.size()) == bytes.size() &&
         SDL_GetIOStatus(handle.stream) == SDL_IO_STATUS_READY));
    lua_pushboolean(L, success);
    return 1;
}
int Seek(lua_State* L) {
    Count(L, 2, 3);
    auto& handle = OpenHandle(L);
    const auto offset = Integer(L, 2);
    if (offset < -9223372036854775808.0 || offset >= 9223372036854775808.0)
        throw std::invalid_argument("seek offset must fit a signed 64-bit byte offset");
    const auto whence = lua_isnoneornil(L, 3) ? std::string_view("set") : CheckString(L, 3);
    SDL_IOWhence origin;
    if (whence == "set") origin = SDL_IO_SEEK_SET;
    else if (whence == "cur") origin = SDL_IO_SEEK_CUR;
    else if (whence == "end") origin = SDL_IO_SEEK_END;
    else throw std::invalid_argument("seek whence must be set, cur, or end");
    if (origin == SDL_IO_SEEK_SET && offset < 0) { lua_pushboolean(L,false); return 1; }
    // SDL's Windows stream in the pinned version returns false (0) on a
    // SetFilePointerEx error. Clear/check its error as well as the offset.
    SDL_ClearError();
    const auto position = SDL_SeekIO(handle.stream, static_cast<Sint64>(offset), origin);
    lua_pushboolean(L, position >= 0 && SDL_GetError()[0] == '\0');
    return 1;
}
int Tell(lua_State* L) {
    Count(L, 1, 1);
    auto& handle=OpenHandle(L);
    SDL_ClearError();
    const auto offset = SDL_TellIO(handle.stream);
    if (offset < 0 || SDL_GetError()[0] != '\0') lua_pushnil(L); else lua_pushnumber(L, static_cast<double>(offset));
    return 1;
}
int Close(lua_State* L) {
    Count(L, 1, 1);
    auto& handle = OpenHandle(L);
    auto* stream = std::exchange(handle.stream, nullptr);
    const bool success = SDL_CloseIO(stream); // Invalid even if flush/close fails.
    handle.filesystem.reset();
    lua_pushboolean(L, success);
    return 1;
}
int HandleGc(lua_State* L) {
    auto* handle = TestUserdata<Handle>(L, 1, handleType);
    if (handle) {
        handle->tag = 0;
        if (auto* stream = std::exchange(handle->stream, nullptr)) SDL_CloseIO(stream);
        handle->filesystem.reset();
    }
    return 0;
}
int ContextGc(lua_State* L) {
    auto* context = TestUserdata<Context>(L, 1, contextType);
    if (context) { context->tag = 0; context->filesystem.reset(); }
    return 0;
}
int Open(lua_State* L) {
    Count(L, 2, 2);
    const auto path = CheckString(L, 1);
    const auto mode = Native::ParseMode(CheckString(L, 2));
    // Allocation may run a Lua finalizer which invalidates the service. Do
    // not borrow Context/FileSystem across it; install an empty GC-safe handle
    // first, then validate and pin its native owner before any further work.
    auto* handle = &detail::PushHandle(L);
    handle->filesystem = FileContext(L).filesystem;
    handle->readable = mode.readable;
    handle->writable = mode.writable;
    handle->stream = handle->filesystem->Open(path, mode);
    if (!handle->stream) { handle->filesystem.reset(); lua_pop(L, 1); lua_pushnil(L); }
    return 1;
}
int Exists(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type != Native::NodeType::Missing); return 1; }
int IsFile(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type == Native::NodeType::File); return 1; }
int IsDirectory(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type == Native::NodeType::Directory); return 1; }
int ListDirectory(lua_State* L) {
    Count(L, 1, 1);
    const auto entries = Filesystem(L).ListDirectory(CheckString(L, 1));
    if (!entries) { lua_pushnil(L); return 1; }
    if (entries->size() > std::size_t(std::numeric_limits<int>::max())) throw std::runtime_error("too many directory entries for Lua");
    lua_createtable(L, static_cast<int>(entries->size()), 0);
    int index = 0;
    for (const auto& entry : *entries) { lua_pushlstring(L, entry.data(), entry.size()); lua_rawseti(L, -2, ++index); }
    return 1;
}
int CreateDirectory(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).CreateDirectory(CheckString(L, 1))); return 1; }
int Remove(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Remove(CheckString(L, 1))); return 1; }
int Rename(lua_State* L) { Count(L, 3, 3); lua_pushboolean(L, Filesystem(L).Rename(CheckString(L, 1), CheckString(L, 2), CheckString(L, 3))); return 1; }
int Copy(lua_State* L) { Count(L, 2, 2); lua_pushboolean(L, Filesystem(L).Copy(CheckString(L, 1), CheckString(L, 2))); return 1; }
int Move(lua_State* L) { Count(L, 2, 2); lua_pushboolean(L, Filesystem(L).Move(CheckString(L, 1), CheckString(L, 2))); return 1; }
int Replace(lua_State* L) { Count(L, 2, 2); lua_pushboolean(L, Filesystem(L).Replace(CheckString(L, 1), CheckString(L, 2))); return 1; }
int RunLuaFileChecked(lua_State* L) {
    Count(L, 1, 1);
    const auto status = core::runtime::Lua::RunScript(L, Filesystem(L), CheckString(L, 1));
    if (status == core::runtime::Lua::ScriptStatus::Error) return -2;
    lua_pushboolean(L, status == core::runtime::Lua::ScriptStatus::Success);
    return 1;
}
int RunLuaFile(lua_State* L) {
    char error[4096]{};
    const int result = TryFunction<RunLuaFileChecked>(L, error, sizeof(error));
    // All C++ scopes have ended before propagating a Lua error on Windows.
    if (result == -2) return lua_error(L);
    if (result < 0) { lua_pushstring(L, error); return lua_error(L); }
    return result;
}
}
detail::Handle& detail::PushHandle(lua_State* L) {
    auto* handle = new(lua_newuserdata(L, sizeof(Handle))) Handle{};
    luaL_getmetatable(L, handleType); lua_setmetatable(L, -2);
    return *handle;
}
void Register(lua_State* L, std::shared_ptr<const Native::FileSystem> filesystem) {
    if (!filesystem) throw std::invalid_argument("File requires a VFS service");
    if (luaL_newmetatable(L, contextType)) {
        lua_pushcfunction(L, ContextGc); lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "File context"); lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, handleType)) {
        const luaL_Reg methods[] = {{"Read", Guard<Read>}, {"ReadLine", Guard<ReadLine>}, {"ReadLines", Guard<ReadLines>},
            {"Write", Guard<Write>}, {"Seek", Guard<Seek>}, {"Tell", Guard<Tell>}, {"Close", Guard<Close>}, {nullptr, nullptr}};
        lua_newtable(L); luaL_register(L, nullptr, methods); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, HandleGc); lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "File handle"); lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
    new(lua_newuserdata(L, sizeof(Context))) Context{Context::Tag, std::move(filesystem)};
    luaL_getmetatable(L, contextType); lua_setmetatable(L, -2);
    lua_pushlightuserdata(L, &contextKey); lua_pushvalue(L, -2); lua_rawset(L, LUA_REGISTRYINDEX);
    const luaL_Reg functions[] = {
        {"Open", Guard<Open>}, {"Exists", Guard<Exists>}, {"IsFile", Guard<IsFile>}, {"IsDirectory", Guard<IsDirectory>},
        {"ListDirectory", Guard<ListDirectory>}, {"CreateDirectory", Guard<CreateDirectory>}, {"Remove", Guard<Remove>},
        {"Rename", Guard<Rename>}, {"Copy", Guard<Copy>}, {"Move", Guard<Move>}, {"Replace", Guard<Replace>},
        {"RunLuaFile", RunLuaFile}, {nullptr, nullptr}};
    luaL_openlib(L, "File", functions, 1);
    lua_pop(L, 1);
}
}
