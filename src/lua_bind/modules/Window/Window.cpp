#include "Window.h"
#include "lua_bind/Guard.h"
#include "core/modules/Window/Window.h"
#include "core/modules/Graphic/Graphic.h"
#include <cmath>
#include <limits>

namespace lua_bind::modules::Window {
namespace {
namespace Native=core::modules::Window;
void Count(lua_State* L,int count) { if(lua_gettop(L)!=count) throw std::invalid_argument("wrong Window argument count"); }
bool Boolean(lua_State* L) {
    Count(L,1);
    if(lua_type(L,1)!=LUA_TBOOLEAN) throw std::invalid_argument("Window expects a boolean");
    return lua_toboolean(L,1)!=0;
}
int Dimension(lua_State* L,int at) {
    if(lua_type(L,at)!=LUA_TNUMBER) throw std::invalid_argument("Window size must be a positive integer");
    const auto value=lua_tonumber(L,at);
    if(!std::isfinite(value) || value<=0 || value>std::numeric_limits<int>::max() || std::floor(value)!=value)
        throw std::invalid_argument("Window size must be a positive integer within native range");
    return static_cast<int>(value);
}
int SetTitle(lua_State* L) {
    Count(L,1); const auto title=CheckString(L,1);
    if(title.find('\0')!=std::string_view::npos) throw std::invalid_argument("Window title cannot contain NUL");
    Native::SetTitle(lua_tostring(L,1)); return 0;
}
int GetTitle(lua_State* L) { Count(L,0); lua_pushstring(L,Native::GetTitle()); return 1; }
int SetSize(lua_State* L) { Count(L,2); Native::SetSize(Dimension(L,1),Dimension(L,2)); return 0; }
int GetSize(lua_State* L) { Count(L,0); const auto size=Native::GetSize(); lua_pushinteger(L,size.width); lua_pushinteger(L,size.height); return 2; }
int SetFullscreen(lua_State* L) { Native::SetFullscreen(Boolean(L)); return 0; }
int IsFullscreen(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::IsFullscreen()); return 1; }
int SetResizable(lua_State* L) { Native::SetResizable(Boolean(L)); return 0; }
int IsResizable(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::IsResizable()); return 1; }
int SetVSync(lua_State* L) { core::modules::Graphic::SetVSync(Boolean(L)); return 0; }
int IsVSyncEnabled(lua_State* L) { Count(L,0); lua_pushboolean(L,core::modules::Graphic::IsVSyncEnabled()); return 1; }
int IsFocused(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::IsFocused()); return 1; }
int SetCursorVisible(lua_State* L) { Native::SetCursorVisible(Boolean(L)); return 0; }
int IsCursorVisible(lua_State* L) { Count(L,0); lua_pushboolean(L,Native::IsCursorVisible()); return 1; }
}
void Register(lua_State* L) {
    const luaL_Reg functions[]={
        {"SetTitle",Guard<SetTitle>},{"GetTitle",Guard<GetTitle>},{"SetSize",Guard<SetSize>},{"GetSize",Guard<GetSize>},
        {"SetFullscreen",Guard<SetFullscreen>},{"IsFullscreen",Guard<IsFullscreen>},
        {"SetResizable",Guard<SetResizable>},{"IsResizable",Guard<IsResizable>},{"SetVSync",Guard<SetVSync>},
        {"IsVSyncEnabled",Guard<IsVSyncEnabled>},{"IsFocused",Guard<IsFocused>},
        {"SetCursorVisible",Guard<SetCursorVisible>},{"IsCursorVisible",Guard<IsCursorVisible>},{nullptr,nullptr}};
    luaL_register(L,"Window",functions); lua_pop(L,1);
}
}
