#include "Runtime.h"
#include "lua_bind/Guard.h"
#include "core/runtime/Clock/Clock.h"
#include "core/runtime/Lua/Lua.h"

namespace lua_bind::runtime {
namespace {
void Count(lua_State* L,int count) {
    if(lua_gettop(L)!=count) throw std::invalid_argument("wrong Runtime argument count");
}
auto& Clock(lua_State* L) { return *static_cast<core::runtime::Clock*>(lua_touserdata(L,lua_upvalueindex(1))); }
int SetFrameFunction(lua_State* L) {
    Count(L,1);
    if(!core::runtime::Lua::SetFrameFunction(L,1)) throw std::invalid_argument("FrameFunction must be a function from this VM");
    return 0;
}
int SetExitFunction(lua_State* L) {
    Count(L,1);
    if(!core::runtime::Lua::SetExitFunction(L,1)) throw std::invalid_argument("ExitFunction must be a function from this VM");
    return 0;
}
int Quit(lua_State* L) {
    Count(L,0);
    *static_cast<bool*>(lua_touserdata(L,lua_upvalueindex(2)))=true;
    return 0;
}
int SetTargetFPS(lua_State* L) {
    Count(L,1);
    if(lua_type(L,1)!=LUA_TNUMBER) throw std::invalid_argument("FPS must be a number");
    Clock(L).SetTargetFPS(lua_tonumber(L,1)); return 0;
}
int GetTargetFPS(lua_State* L) { Count(L,0); lua_pushnumber(L,Clock(L).TargetFPS()); return 1; }
int GetCurrentFPS(lua_State* L) { Count(L,0); lua_pushnumber(L,Clock(L).CurrentFPS()); return 1; }
int GetArguments(lua_State* L) {
    Count(L,0);
    const auto& arguments=*static_cast<const std::vector<std::string>*>(lua_touserdata(L,lua_upvalueindex(3)));
    lua_createtable(L,static_cast<int>(arguments.size()),0);
    int index=1;
    for(const auto& value:arguments) {
        lua_pushlstring(L,value.data(),value.size());
        lua_rawseti(L,-2,index++);
    }
    return 1;
}
}
void Register(lua_State* L,core::runtime::Clock& clock,bool& quitRequested,const std::vector<std::string>& arguments) {
    const luaL_Reg functions[]={
        {"SetFrameFunction",Guard<SetFrameFunction>},{"SetExitFunction",Guard<SetExitFunction>},
        {"Quit",Guard<Quit>},{"SetTargetFPS",Guard<SetTargetFPS>},
        {"GetTargetFPS",Guard<GetTargetFPS>},{"GetCurrentFPS",Guard<GetCurrentFPS>},
        {"GetArguments",Guard<GetArguments>},{nullptr,nullptr}};
    lua_pushlightuserdata(L,&clock); lua_pushlightuserdata(L,&quitRequested);
    lua_pushlightuserdata(L,const_cast<std::vector<std::string>*>(&arguments));
    luaL_openlib(L,"Runtime",functions,3); lua_pop(L,1);
}
}
