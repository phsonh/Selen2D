#include "File.h"
#include "Handle.h"
#include "core/modules/File/Vfs.h"
#include "core/modules/File/IoError.h"
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
constexpr const char* contextType = "engine.file.context";
constexpr const char* handleType = "engine.file.handle";
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
    if (!std::isfinite(value) || value != std::floor(value) || std::abs(value) > 9007199254740991.0) throw std::invalid_argument("file byte offset/size must be a finite integer");
    return value;
}
// Only IoError is translated to normal failure. Permission/argument/structure
// exceptions continue through Guard and become Lua errors.
enum class ResultKind { Value, Boolean, Read };
template<int (*F)(lua_State*), ResultKind Kind> int IoResult(lua_State* L) {
    std::string code, message;
    try {
        const int results = F(L);
        if (results < 0) return results;
        lua_pushnil(L);
        return results + 1;
    } catch (const Native::IoError& error) {
        code = error.code; message = error.what();
    }
    // Never allocate or raise a Lua exception while a C++ catch is active.
    if constexpr (Kind == ResultKind::Boolean) lua_pushboolean(L, false);
    else lua_pushnil(L);
    if constexpr (Kind == ResultKind::Read) lua_pushboolean(L, false);
    lua_createtable(L, 0, 2);
    lua_pushlstring(L, code.data(), code.size()); lua_setfield(L, -2, "code");
    lua_pushlstring(L, message.data(), message.size()); lua_setfield(L, -2, "message");
    return Kind == ResultKind::Read ? 3 : 2;
}
void Readable(const Handle& handle) {
    if (!handle.readable) throw Native::IoError("permission_denied", "file was not opened for reading");
}
void Writable(const Handle& handle) {
    if (!handle.writable) throw Native::IoError("permission_denied", "file was not opened for writing");
}
[[noreturn]] void StreamError(const Handle& h, const char* operation) {
    throw Native::GetStreamError(h.stream, operation, h.path);
}
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
    Readable(handle);
    std::string output;
    std::array<char, 64 * 1024> buffer{};
    while (remaining > 0) {
        const auto wanted = std::min(remaining, buffer.size());
        const auto count = SDL_ReadIO(handle.stream, buffer.data(), wanted);
        if (SDL_GetIOStatus(handle.stream) == SDL_IO_STATUS_ERROR) StreamError(handle, "read");
        output.append(buffer.data(), count);
        remaining -= count;
        if (count < wanted) {
            const auto status = SDL_GetIOStatus(handle.stream);
            if (status == SDL_IO_STATUS_EOF) break;
            if (status != SDL_IO_STATUS_READY || count == 0) StreamError(handle, "read");
        }
    }
    return Bytes(L, output);
}
enum class LineStatus { Line, End };
LineStatus NextLine(Handle& handle, std::string& line) {
    line.clear();
    Readable(handle);
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = SDL_ReadIO(handle.stream, buffer.data(), buffer.size());
        if (SDL_GetIOStatus(handle.stream) == SDL_IO_STATUS_ERROR) StreamError(handle, "read line");
        const auto newline = std::find(buffer.begin(), buffer.begin() + count, '\n');
        if (newline != buffer.begin() + count) {
            const auto used = static_cast<std::size_t>(newline - buffer.begin()) + 1;
            line.append(buffer.data(), used - 1);
            // Return surplus bytes to the seekable VFS stream. Mixed Read and
            // ReadLine calls, Tell and writes therefore share one exact cursor.
            if (used < count && SDL_SeekIO(handle.stream, -static_cast<Sint64>(count - used), SDL_IO_SEEK_CUR) < 0)
                StreamError(handle, "read line");
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineStatus::Line;
        }
        line.append(buffer.data(), count);
        if (count < buffer.size()) {
            const auto status = SDL_GetIOStatus(handle.stream);
            if (status == SDL_IO_STATUS_EOF) return line.empty() ? LineStatus::End : LineStatus::Line;
            if (status != SDL_IO_STATUS_READY || count == 0) StreamError(handle, "read line");
        }
    }
}
int ReadLine(lua_State* L) {
    Count(L, 1, 1);
    std::string line;
    switch (NextLine(OpenHandle(L), line)) {
    case LineStatus::Line: return Bytes(L, line);
    case LineStatus::End: lua_pushnil(L); lua_pushboolean(L, true); return 2;
    }
    throw std::logic_error("invalid line state");
}
int ReadLines(lua_State* L) {
    Count(L, 1, 1);
    auto& handle = OpenHandle(L);
    lua_newtable(L);
    std::string line;
    int index = 0;
    for (;;) {
        const auto status = NextLine(handle, line);
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
    Writable(handle);
    if (!bytes.empty() && (SDL_WriteIO(handle.stream, bytes.data(), bytes.size()) != bytes.size() ||
        SDL_GetIOStatus(handle.stream) != SDL_IO_STATUS_READY)) StreamError(handle, "write");
    lua_pushboolean(L, true);
    return 1;
}
int Seek(lua_State* L) {
    Count(L, 2, 3);
    auto& handle = OpenHandle(L);
    const auto offset = Integer(L, 2);
    const auto whence = lua_isnoneornil(L, 3) ? std::string_view("set") : CheckString(L, 3);
    SDL_IOWhence origin;
    if (whence == "set") origin = SDL_IO_SEEK_SET;
    else if (whence == "cur") origin = SDL_IO_SEEK_CUR;
    else if (whence == "end") origin = SDL_IO_SEEK_END;
    else throw std::invalid_argument("seek whence must be set, cur, or end");
    if (SDL_SeekIO(handle.stream, static_cast<Sint64>(offset), origin) < 0) StreamError(handle, "seek");
    lua_pushboolean(L, true);
    return 1;
}
int Tell(lua_State* L) {
    Count(L, 1, 1);
    auto& handle=OpenHandle(L);
    const auto offset = SDL_TellIO(handle.stream);
    if (offset < 0) StreamError(handle, "tell");
    if (offset > 9007199254740991LL) throw Native::IoError("out_of_range", "file position exceeds exact Lua integer range");
    lua_pushnumber(L, static_cast<double>(offset));
    return 1;
}
int Close(lua_State* L) {
    Count(L, 1, 1);
    auto& handle = OpenHandle(L);
    auto* stream = std::exchange(handle.stream, nullptr);
    handle.filesystem.reset();
    Native::CloseStream(stream, handle.path); // Invalid even if flush/close fails.
    lua_pushboolean(L, true);
    return 1;
}
int HandleGc(lua_State* L) {
    auto* handle = TestUserdata<Handle>(L, 1, handleType);
    if (handle) {
        handle->tag = 0;
        if (auto* stream = std::exchange(handle->stream, nullptr)) SDL_CloseIO(stream);
        handle->filesystem.reset();
        handle->path.clear();
        handle->~Handle();
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
    handle->path = std::string(path);
    handle->readable = mode.readable;
    handle->writable = mode.writable;
    handle->stream = handle->filesystem->Open(path, mode);
    if (!handle->stream) throw Native::IoError("io_error", "failed to open VFS file");
    return 1;
}
int Exists(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type != Native::NodeType::Missing); return 1; }
int IsFile(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type == Native::NodeType::File); return 1; }
int IsDirectory(lua_State* L) { Count(L, 1, 1); lua_pushboolean(L, Filesystem(L).Stat(CheckString(L, 1)).type == Native::NodeType::Directory); return 1; }
int ListDirectory(lua_State* L) {
    Count(L, 1, 1);
    const auto entries = Filesystem(L).ListDirectory(CheckString(L, 1));
    if (!entries) throw Native::IoError("io_error", "failed to list VFS directory");
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
    if (status == core::runtime::Lua::ScriptStatus::Unreadable) throw Native::IoError("not_found", "VFS script does not exist");
    lua_pushboolean(L, true);
    return 1;
}
int GetScriptPath(lua_State* L) {
    Count(L, 0, 0);
    return core::runtime::Lua::PushCallingScriptPath(L);
}
int RunLuaFile(lua_State* L) {
    char error[4096]{};
    const int result = TryFunction<IoResult<RunLuaFileChecked, ResultKind::Boolean>>(L, error, sizeof(error));
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
        const luaL_Reg methods[] = {{"Read", Guard<IoResult<Read, ResultKind::Read>>}, {"ReadLine", Guard<IoResult<ReadLine, ResultKind::Read>>}, {"ReadLines", Guard<IoResult<ReadLines, ResultKind::Read>>},
            {"Write", Guard<IoResult<Write, ResultKind::Boolean>>}, {"Seek", Guard<IoResult<Seek, ResultKind::Boolean>>}, {"Tell", Guard<IoResult<Tell, ResultKind::Value>>}, {"Close", Guard<IoResult<Close, ResultKind::Boolean>>}, {nullptr, nullptr}};
        lua_newtable(L); luaL_register(L, nullptr, methods); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, HandleGc); lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "File handle"); lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
    new(lua_newuserdata(L, sizeof(Context))) Context{Context::Tag, std::move(filesystem)};
    luaL_getmetatable(L, contextType); lua_setmetatable(L, -2);
    lua_pushlightuserdata(L, &contextKey); lua_pushvalue(L, -2); lua_rawset(L, LUA_REGISTRYINDEX);
    const luaL_Reg functions[] = {
        {"Open", Guard<IoResult<Open, ResultKind::Value>>}, {"Exists", Guard<IoResult<Exists, ResultKind::Boolean>>}, {"IsFile", Guard<IoResult<IsFile, ResultKind::Boolean>>}, {"IsDirectory", Guard<IoResult<IsDirectory, ResultKind::Boolean>>},
        {"ListDirectory", Guard<IoResult<ListDirectory, ResultKind::Value>>}, {"CreateDirectory", Guard<IoResult<CreateDirectory, ResultKind::Boolean>>}, {"Remove", Guard<IoResult<Remove, ResultKind::Boolean>>},
        {"Rename", Guard<IoResult<Rename, ResultKind::Boolean>>}, {"Copy", Guard<IoResult<Copy, ResultKind::Boolean>>}, {"Move", Guard<IoResult<Move, ResultKind::Boolean>>}, {"Replace", Guard<IoResult<Replace, ResultKind::Boolean>>},
        {"RunLuaFile", RunLuaFile}, {"GetScriptPath", Guard<GetScriptPath>}, {nullptr, nullptr}};
    luaL_openlib(L, "File", functions, 1);
    lua_pop(L, 1);
}
}
