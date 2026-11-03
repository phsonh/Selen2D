#pragma once

struct lua_State;

namespace lua_bind::modules::Window {
	void Register(lua_State* L);
}