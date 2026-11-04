#include "TrustedScripts.h"
// Pinned LuaJIT implementation boundary. A weak proto key prevents stale pointer
// reuse, and covers closures not instantiated until after the file returns.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201 5054 4127)
#endif
#include <lj_obj.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
namespace core::runtime::Lua {
namespace {
char sourceRegistry;
void Sources(lua_State* L) {
    lua_pushlightuserdata(L, &sourceRegistry); lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1); lua_newtable(L); lua_newtable(L);
    lua_pushliteral(L, "k"); lua_setfield(L, -2, "__mode"); lua_setmetatable(L, -2);
    lua_pushlightuserdata(L, &sourceRegistry); lua_pushvalue(L, -2); lua_rawset(L, LUA_REGISTRYINDEX);
}
void PushProto(lua_State* L, GCproto* proto) {
    lua_pushnil(L); setprotoV(L, L->top - 1, proto);
}
void Record(lua_State* L, GCproto* proto, int table, std::string_view path) {
    PushProto(L, proto); lua_pushlstring(L, path.data(), path.size()); lua_rawset(L, table);
    for (MSize i = 0; i < proto->sizekgc; ++i) {
        auto* object = proto_kgc(proto, -1 - static_cast<int>(i));
        if (object->gch.gct == static_cast<uint8_t>(~LJ_TPROTO)) Record(L, gco2pt(object), table, path);
    }
}
}
void RecordScriptSource(lua_State* L, int index, std::string_view path) {
    if (!lua_isfunction(L, index) || lua_iscfunction(L, index)) return;
    auto* fn = const_cast<GCfunc*>(static_cast<const GCfunc*>(lua_topointer(L, index)));
    Sources(L); Record(L, funcproto(fn), lua_gettop(L), path); lua_pop(L, 1);
}
int PushCallingScriptPath(lua_State* L) {
    // The pinned x64 VM records erased tail-call origins at dispatch time;
    // current mutable global/upvalue values cannot prove historical callers.
    if(L->native_call_tail) {lua_pushnil(L);return 1;}
    lua_checkstack(L,64);
    lua_Debug frame{};
    if (!lua_getstack(L, 1, &frame) || !lua_getinfo(L, "f", &frame)) { lua_pushnil(L); return 1; }
    if (lua_iscfunction(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }
    auto* fn = const_cast<GCfunc*>(static_cast<const GCfunc*>(lua_topointer(L, -1)));
    Sources(L); PushProto(L, funcproto(fn)); lua_rawget(L, -2);
    lua_remove(L, -2); lua_remove(L, -2); return 1;
}
}
