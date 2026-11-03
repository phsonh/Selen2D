#include "Input.h"
#include "lua_bind/Guard.h"
#include "core/modules/Input/Input.h"
#include "core/modules/Input/InputNames.h"

namespace lua_bind::modules::Input {
namespace {
namespace Native=core::modules::Input;
void Count(lua_State* L,int expected) { if(lua_gettop(L)!=expected) throw std::invalid_argument("wrong Input argument count"); }
std::string_view Name(lua_State* L) { Count(L,1); return CheckString(L,1); }
int IsKeyDown(lua_State* L) { const auto code=Native::KeyCode(Name(L)); lua_pushboolean(L,Native::GetState().Down(code)); return 1; }
int SetMouseEnabled(lua_State* L) {
    Count(L,1);
    if(lua_type(L,1)!=LUA_TBOOLEAN) throw std::invalid_argument("Input.SetMouseEnabled expects a boolean");
    Native::SetMouseEnabled(lua_toboolean(L,1)!=0); return 0;
}
int IsMouseEnabled(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::GetState().MouseEnabled()); return 1; }
int GetMousePosition(lua_State* L) {
    Count(L,0); const auto& state=Native::GetState();
    if(!state.MouseValid()) { lua_pushnil(L); return 1; }
    lua_pushnumber(L,state.MouseX()); lua_pushnumber(L,state.MouseY()); return 2;
}
int GetMouseWheel(lua_State* L) {
    Count(L,0); const auto& state=Native::GetState();
    lua_pushnumber(L,state.WheelX()); lua_pushnumber(L,state.WheelY()); return 2;
}
int IsMouseButtonDown(lua_State* L) { const auto button=Native::MouseButton(Name(L)); lua_pushboolean(L,Native::GetState().MouseDown(button)); return 1; }
int IsGamepadButtonDown(lua_State* L) { const auto button=Native::GamepadButton(Name(L)); lua_pushboolean(L,Native::GetState().GamepadDown(button)); return 1; }
int GetGamepadAxis(lua_State* L) { const auto axis=Native::GamepadAxis(Name(L)); lua_pushnumber(L,Native::GetState().GamepadAxis(axis)); return 1; }
}
void Register(lua_State* L) {
    const luaL_Reg functions[]={
        {"IsKeyDown",Guard<IsKeyDown>},{"SetMouseEnabled",Guard<SetMouseEnabled>},{"IsMouseEnabled",Guard<IsMouseEnabled>},
        {"GetMousePosition",Guard<GetMousePosition>},{"GetMouseWheel",Guard<GetMouseWheel>},
        {"IsMouseButtonDown",Guard<IsMouseButtonDown>},{"IsGamepadButtonDown",Guard<IsGamepadButtonDown>},
        {"GetGamepadAxis",Guard<GetGamepadAxis>},{nullptr,nullptr}};
    luaL_register(L,"Input",functions); lua_pop(L,1);
}
}
