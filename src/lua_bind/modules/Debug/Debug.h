#pragma once

struct lua_State;

namespace lua_bind::modules::Debug {
// Error reports through Lua's protected error path; it never requests Quit.
void Register(lua_State* L);
}
