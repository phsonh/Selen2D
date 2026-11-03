#include "VfsScripts.h"
#include "core/modules/File/Vfs.h"
#include "lua_bind/Guard.h"
#include <algorithm>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

namespace core::runtime::Lua {
namespace {
using FileSystem = modules::File::FileSystem;
constexpr const char* contextType = "solongo.vfs.scripts";
char contextKey;
struct Context {
    static constexpr std::uint64_t Tag = 0x5356465353435458ULL;
    std::uint64_t tag{Tag};
    std::shared_ptr<const FileSystem> filesystem;
};
Context* TestContext(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(Context)) return nullptr;
    auto* context = static_cast<Context*>(luaL_testudata(L, index, contextType));
    return context && context->tag == Context::Tag ? context : nullptr;
}
const FileSystem& Filesystem(lua_State* L) {
    auto* context = TestContext(L, lua_upvalueindex(1));
    if (!context || !context->filesystem) throw std::runtime_error("VFS script service is unavailable");
    return *context->filesystem;
}
void Count(lua_State* L, int count) {
    if (lua_gettop(L) != count) throw std::invalid_argument("VFS script function requires one path/module name");
}
int Traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "non-string Lua error", 1);
    return 1;
}
int ExecuteLoaded(lua_State* L, int results) {
    const int function = lua_gettop(L);
    lua_pushcfunction(L, Traceback);
    lua_insert(L, function);
    const int status = lua_pcall(L, 0, results, function);
    lua_remove(L, function);
    return status;
}
template<int (*Function)(lua_State*)> int GuardScript(lua_State* L) {
    char error[4096]{};
    const int results = lua_bind::TryFunction<Function>(L, error, sizeof(error));
    if (results == -2) return lua_error(L);
    if (results < 0) { lua_pushstring(L, error); return lua_error(L); }
    return results;
}
int ContextGc(lua_State* L) {
    auto* context = TestContext(L, 1);
    if (context) { context->tag = 0; context->filesystem.reset(); }
    return 0;
}
int LoadFile(lua_State* L) {
    Count(L, 1);
    const auto path = lua_bind::CheckString(L, 1);
    const auto status = LoadScript(L, Filesystem(L), path);
    if (status == ScriptStatus::Success) return 1;
    if (status == ScriptStatus::Unreadable) {
        const auto error = "cannot read VFS script '" + std::string(path) + "'";
        lua_pushnil(L); lua_pushlstring(L, error.data(), error.size());
    } else { lua_pushnil(L); lua_insert(L, -2); }
    return 2;
}
int DoFile(lua_State* L) {
    Count(L, 1);
    const auto path = lua_bind::CheckString(L, 1);
    const auto status = LoadScript(L, Filesystem(L), path);
    if (status == ScriptStatus::Unreadable) throw std::runtime_error("cannot read VFS script '" + std::string(path) + "'");
    if (status == ScriptStatus::Error) return -2;
    if (ExecuteLoaded(L, LUA_MULTRET) != 0) return -2;
    return lua_gettop(L) - 1;
}
int Search(lua_State* L) {
    Count(L, 1);
    std::string module(lua_bind::CheckString(L, 1));
    if (module.empty() || module.find('\0') != std::string::npos) throw std::invalid_argument("module name must be nonempty and contain no NUL");
    std::replace(module.begin(), module.end(), '.', '/');
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) throw std::invalid_argument("package must remain a table");
    lua_getfield(L, -1, "path");
    const std::string paths(lua_bind::CheckString(L, -1));
    lua_pop(L, 2);
    std::string missing;
    std::size_t begin = 0;
    while (begin <= paths.size()) {
        auto end = paths.find(';', begin);
        if (end == std::string::npos) end = paths.size();
        const auto entry = std::string_view(paths).substr(begin, end - begin);
        if (!entry.empty()) {
            std::string path;
            for (char c : entry) { if (c == '?') path += module; else path += c; }
            const auto status = LoadScript(L, Filesystem(L), path);
            if (status == ScriptStatus::Success) return 1;
            if (status == ScriptStatus::Error) return -2;
            missing += "\n\tno VFS file '" + path + "'";
        }
        if (end == paths.size()) break;
        begin = end + 1;
    }
    if (missing.empty()) missing = "\n\tno VFS search paths";
    lua_pushlstring(L, missing.data(), missing.size());
    return 1;
}
}
ScriptStatus LoadScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path) {
    const auto bytes = filesystem.Read(path);
    if (!bytes) return ScriptStatus::Unreadable;
    const auto name = "@" + std::string(path);
    const int status = luaL_loadbuffer(L, bytes->data(), bytes->size(), name.c_str());
    return status == 0 ? ScriptStatus::Success : ScriptStatus::Error;
}
ScriptStatus RunScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path) {
    const auto status = LoadScript(L, filesystem, path);
    if (status != ScriptStatus::Success) return status;
    return ExecuteLoaded(L, 0) == 0 ? ScriptStatus::Success : ScriptStatus::Error;
}
void InstallSearchers(lua_State* L, std::shared_ptr<const modules::File::FileSystem> filesystem) {
    if (!filesystem) throw std::invalid_argument("script loaders require a VFS service");
    if (luaL_newmetatable(L, contextType)) {
        lua_pushcfunction(L, ContextGc); lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "VFS script context"); lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
    new(lua_newuserdata(L, sizeof(Context))) Context{Context::Tag, std::move(filesystem)};
    luaL_getmetatable(L, contextType); lua_setmetatable(L, -2);
    const int context = lua_gettop(L);
    lua_pushlightuserdata(L, &contextKey); lua_pushvalue(L, context); lua_rawset(L, LUA_REGISTRYINDEX);
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) throw std::runtime_error("Lua package library is not initialized");
    const int package = lua_gettop(L);
    lua_getfield(L, package, "loaders");
    if (!lua_istable(L, -1)) throw std::runtime_error("Lua package loaders are unavailable");
    lua_rawgeti(L, -1, 1);
    if (!lua_isfunction(L, -1)) throw std::runtime_error("Lua preload searcher is unavailable");
    lua_newtable(L);
    lua_pushvalue(L, -2); lua_rawseti(L, -2, 1);
    lua_pushvalue(L, context); lua_pushcclosure(L, GuardScript<Search>, 1); lua_rawseti(L, -2, 2);
    lua_setfield(L, package, "loaders");
    lua_pop(L, 2);
    lua_pushliteral(L, "?.lua;?/init.lua"); lua_setfield(L, package, "path");
    lua_pushliteral(L, ""); lua_setfield(L, package, "cpath");
    lua_pushvalue(L, context); lua_pushcclosure(L, GuardScript<LoadFile>, 1); lua_setglobal(L, "loadfile");
    lua_pushvalue(L, context); lua_pushcclosure(L, GuardScript<DoFile>, 1); lua_setglobal(L, "dofile");
    lua_pop(L, 2);
}
}
