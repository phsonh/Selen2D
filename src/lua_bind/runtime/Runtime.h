#pragma once
#include <lua.hpp>
#include <string>
#include <vector>
namespace core::runtime { class Clock; }
namespace lua_bind::runtime {
// Borrowed host state must outlive the Lua VM, including finalizers.
void Register(lua_State*, core::runtime::Clock&, bool& quitRequested, const std::vector<std::string>& arguments);
}
