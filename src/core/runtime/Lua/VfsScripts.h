#pragma once
#include <lua.hpp>
#include "TrustedScripts.h"
#include <memory>
#include <string_view>
namespace core::modules::File { class FileSystem; }

namespace core::runtime::Lua {
enum class ScriptStatus { Success, Unreadable, Error };
// Success: pushes the compiled function. Error: pushes the Lua error string.
// Unreadable: leaves the stack unchanged. Never throws a Lua error itself.
ScriptStatus LoadScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path, const char* mode = nullptr);
// Success: ignores script results. Error: leaves one syntax error / traceback.
// Unreadable: leaves the stack unchanged. Executes in the caller's Lua VM.
ScriptStatus RunScript(lua_State* L, const modules::File::FileSystem& filesystem, std::string_view path);
// Preserves native require/preload/cache and custom searchers. Replaces only
// file and disk-C searchers; all filename entry points share VFS admission.
// Dynamic load/loadstring, stdio, popen, os.execute and FFI remain native and
// are explicitly not a complete untrusted-code sandbox.
void InstallSearchers(lua_State* L, std::shared_ptr<const modules::File::FileSystem> filesystem);
}
