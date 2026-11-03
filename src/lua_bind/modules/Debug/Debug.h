#pragma once

struct lua_State;

namespace lua_bind::modules::Debug {
// The host owns the flag for at least as long as the Lua state. Error shares
// Runtime.Quit's safe-boundary exit path; it never destroys the VM itself.
void Register(lua_State* L, bool& quitRequested);
}
