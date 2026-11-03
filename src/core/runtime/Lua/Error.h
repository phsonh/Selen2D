#pragma once
#include <lua.hpp>

namespace core::runtime::Lua {
// luaL_traceback's message parameter is NUL terminated, whereas Lua errors
// are byte strings. Append the stack separately so the original bytes survive.
inline int ErrorTraceback(lua_State* L) {
    if (lua_isstring(L, 1)) lua_pushvalue(L, 1);
    else lua_pushliteral(L, "non-string Lua error");
    lua_pushliteral(L, "\n");
    luaL_traceback(L, L, nullptr, 1);
    lua_concat(L, 3);
    return 1;
}
}
