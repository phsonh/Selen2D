#pragma once
#include <lua.hpp>
namespace core::runtime { class Clock; }
namespace lua_bind::modules::Time {
void Register(lua_State*,core::runtime::Clock&); // Clock outlives VM.
}
