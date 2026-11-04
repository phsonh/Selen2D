#pragma once
#include <lua.hpp>
namespace lua_bind::modules::Graphic::TextBinding {
// Every function already uses Guard. Register with the same service userdata
// upvalue as Graphic; State and native-service validity come from its context.
const luaL_Reg* Functions() noexcept;
int RenderTextToCanvas(lua_State*);
}
