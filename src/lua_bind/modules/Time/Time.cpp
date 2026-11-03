#include "Time.h"
#include "lua_bind/Guard.h"
#include "core/runtime/Clock/Clock.h"
#include "core/modules/Time/Time.h"
#include <SDL3/SDL_timer.h>

namespace lua_bind::modules::Time {
namespace {
namespace Native=core::modules::Time;
auto& Clock(lua_State* L) { return *static_cast<core::runtime::Clock*>(lua_touserdata(L,lua_upvalueindex(1))); }
void Count(lua_State* L,int min,int max) {
    if(lua_gettop(L)<min || lua_gettop(L)>max) throw std::invalid_argument("wrong Time argument count");
}
double Number(lua_State* L,int index) {
    if(lua_type(L,index)!=LUA_TNUMBER) throw std::invalid_argument("Time argument must be a number");
    return lua_tonumber(L,index);
}
int GetDate(lua_State* L) {
    Count(L,0,0); const auto d=Native::GetDate();
    lua_createtable(L,0,6);
    const char* names[]={"year","month","day","hour","minute","second"};
    const int values[]={d.year,d.month,d.day,d.hour,d.minute,d.second};
    for(int i=0;i<6;++i) { lua_pushinteger(L,values[i]); lua_setfield(L,-2,names[i]); }
    return 1;
}
int GetElapsedMilliseconds(lua_State* L) { Count(L,0,0); lua_pushnumber(L,Clock(L).ElapsedMilliseconds(SDL_GetTicksNS())); return 1; }
int GetFrameCount(lua_State* L) { Count(L,0,0); lua_pushnumber(L,static_cast<double>(Clock(L).FrameCount())); return 1; }
int GetDeltaMilliseconds(lua_State* L) { Count(L,0,0); lua_pushnumber(L,Clock(L).DeltaMilliseconds()); return 1; }
int FramesToSeconds(lua_State* L) {
    Count(L,1,2); lua_pushnumber(L,Native::FramesToSeconds(Number(L,1),OptionalNumber(L,2,Clock(L).TargetFPS()))); return 1;
}
int SecondsToFrames(lua_State* L) {
    Count(L,1,2); lua_pushnumber(L,Native::SecondsToFrames(Number(L,1),OptionalNumber(L,2,Clock(L).TargetFPS()))); return 1;
}
}
void Register(lua_State* L,core::runtime::Clock& clock) {
    const luaL_Reg functions[]={
        {"GetDate",Guard<GetDate>},{"GetElapsedMilliseconds",Guard<GetElapsedMilliseconds>},
        {"GetFrameCount",Guard<GetFrameCount>},{"GetDeltaMilliseconds",Guard<GetDeltaMilliseconds>},
        {"FramesToSeconds",Guard<FramesToSeconds>},{"SecondsToFrames",Guard<SecondsToFrames>},
        {nullptr,nullptr}};
    lua_pushlightuserdata(L,&clock); luaL_openlib(L,"Time",functions,1); lua_pop(L,1);
}
}
