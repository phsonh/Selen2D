#include "Input.h"
#include "lua_bind/Guard.h"
#include "core/modules/Input/Input.h"
#include "core/modules/Input/InputNames.h"

namespace lua_bind::modules::Input {
namespace {
namespace Native=core::modules::Input;
void Count(lua_State* L,int expected) { if(lua_gettop(L)!=expected) throw std::invalid_argument("wrong Input argument count"); }
std::string_view Name(lua_State* L) { Count(L,1); return CheckString(L,1); }
bool Boolean(lua_State* L) {
    Count(L,1);
    if(lua_type(L,1)!=LUA_TBOOLEAN) throw std::invalid_argument("Input expects a boolean");
    return lua_toboolean(L,1)!=0;
}
Native::Pair Numbers(lua_State* L,bool threshold) {
    const int count=lua_gettop(L);
    if(count<1 || count>2) throw std::invalid_argument("Input configuration expects one or two numbers");
    for(int i=1;i<=count;++i)
        if(lua_type(L,i)!=LUA_TNUMBER) throw std::invalid_argument("Input configuration expects numbers");
    const double first=lua_tonumber(L,1);
    return {first,count==2?lua_tonumber(L,2):(threshold?first*.8:first)};
}
int PushPair(lua_State* L,double first,double second) {
    lua_pushnumber(L,first); lua_pushnumber(L,second); return 2;
}
int IsKeyDown(lua_State* L) { const auto code=Native::KeyCode(Name(L)); lua_pushboolean(L,Native::GetState().Down(code)); return 1; }
int SetMouseEnabled(lua_State* L) { Native::SetMouseEnabled(Boolean(L)); return 0; }
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
int SetGamepadEnabled(lua_State* L) { Native::SetGamepadEnabled(Boolean(L)); return 0; }
int IsGamepadEnabled(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::GetState().GamepadEnabled()); return 1; }
int IsGamepadConnected(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::GetState().GamepadConnected()); return 1; }
int SetGamepadDeadzone(lua_State* L) { const auto v=Numbers(L,false); Native::SetGamepadDeadzone(v.left,v.right); return 0; }
int GetGamepadDeadzone(lua_State* L) { Count(L,0); const auto v=Native::GetState().GamepadDeadzone(); return PushPair(L,v.left,v.right); }
int SetGamepadTriggerDeadzone(lua_State* L) { const auto v=Numbers(L,false); Native::SetGamepadTriggerDeadzone(v.left,v.right); return 0; }
int GetGamepadTriggerDeadzone(lua_State* L) { Count(L,0); const auto v=Native::GetState().GamepadTriggerDeadzone(); return PushPair(L,v.left,v.right); }
int SetGamepadStickButtonThreshold(lua_State* L) { const auto v=Numbers(L,true); Native::SetGamepadStickButtonThreshold(v.left,v.right); return 0; }
int GetGamepadStickButtonThreshold(lua_State* L) { Count(L,0); const auto v=Native::GetState().GamepadStickButtonThreshold(); return PushPair(L,v.press,v.release); }
int SetGamepadTriggerThreshold(lua_State* L) { const auto v=Numbers(L,true); Native::SetGamepadTriggerThreshold(v.left,v.right); return 0; }
int GetGamepadTriggerThreshold(lua_State* L) { Count(L,0); const auto v=Native::GetState().GamepadTriggerThreshold(); return PushPair(L,v.press,v.release); }
int IsGamepadButtonDown(lua_State* L) { const auto button=Native::GamepadButton(Name(L)); lua_pushboolean(L,Native::GetState().GamepadDown(button)); return 1; }
int GetGamepadAxis(lua_State* L) { const auto axis=Native::GamepadAxis(Name(L)); lua_pushnumber(L,Native::GetState().GamepadAxis(axis)); return 1; }
}
void Register(lua_State* L) {
    const luaL_Reg functions[]={
        {"IsKeyDown",Guard<IsKeyDown>},{"SetMouseEnabled",Guard<SetMouseEnabled>},{"IsMouseEnabled",Guard<IsMouseEnabled>},
        {"GetMousePosition",Guard<GetMousePosition>},{"GetMouseWheel",Guard<GetMouseWheel>},
        {"IsMouseButtonDown",Guard<IsMouseButtonDown>},
        {"SetGamepadEnabled",Guard<SetGamepadEnabled>},{"IsGamepadEnabled",Guard<IsGamepadEnabled>},
        {"IsGamepadConnected",Guard<IsGamepadConnected>},
        {"SetGamepadDeadzone",Guard<SetGamepadDeadzone>},{"GetGamepadDeadzone",Guard<GetGamepadDeadzone>},
        {"SetGamepadTriggerDeadzone",Guard<SetGamepadTriggerDeadzone>},{"GetGamepadTriggerDeadzone",Guard<GetGamepadTriggerDeadzone>},
        {"SetGamepadStickButtonThreshold",Guard<SetGamepadStickButtonThreshold>},{"GetGamepadStickButtonThreshold",Guard<GetGamepadStickButtonThreshold>},
        {"SetGamepadTriggerThreshold",Guard<SetGamepadTriggerThreshold>},{"GetGamepadTriggerThreshold",Guard<GetGamepadTriggerThreshold>},
        {"IsGamepadButtonDown",Guard<IsGamepadButtonDown>},
        {"GetGamepadAxis",Guard<GetGamepadAxis>},{nullptr,nullptr}};
    luaL_register(L,"Input",functions); lua_pop(L,1);
}
}
