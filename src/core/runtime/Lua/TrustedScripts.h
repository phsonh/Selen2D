#pragma once
#include <lua.hpp>
#include <string_view>
namespace core::runtime::Lua {
// Records only engine-compiled file prototypes, never user-supplied chunk names.
void RecordScriptSource(lua_State*, int functionIndex, std::string_view path);
int PushCallingScriptPath(lua_State*);
}
