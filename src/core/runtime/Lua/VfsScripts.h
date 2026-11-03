#pragma once
#include <lua.hpp>
#include <memory>
#include <string_view>
namespace core::modules::File { class FileSystem; }

namespace core::runtime::Lua {
enum class ScriptStatus { Success, Unreadable, Error };
// Success: pushes the compiled function. Error: pushes the Lua error string.
// Unreadable: leaves the stack unchanged. Never throws a Lua error itself.
ScriptStatus LoadScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path);
// Success: ignores script results. Error: leaves one syntax error / traceback.
// Unreadable: leaves the stack unchanged. Executes in the caller's Lua VM.
ScriptStatus RunScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path);
// Keeps package.preload and package.loaded; replaces script/C search fallbacks
// with one VFS searcher. loadfile/dofile use this same immutable mount snapshot.
// Default package.path: "?.lua;?/init.lua", relative to the VFS root. The entry
// script does not implicitly change that root or append its own directory.
// Lua may set package.path explicitly. There is no physical C-module fallback.
// loadfile(path) returns function or nil,error; dofile(path) returns all script
// values and propagates errors. Neither accepts a missing path / stdin input.
// This is script-resolution policy, not a security sandbox for Lua io/os/ffi.
void InstallSearchers(lua_State* L, std::shared_ptr<const modules::File::FileSystem> filesystem);
}
