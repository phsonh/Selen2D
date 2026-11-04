#include "Debug.h"
#include "lua_bind/Guard.h"
#include "core/modules/Debug/Debug.h"
#include "core/runtime/Log/Log.h"
#include <cmath>
#include <limits>
#include <unordered_set>

namespace lua_bind::modules::Debug {
namespace {
namespace Native = core::modules::Debug;

void Count(lua_State* L, int minimum, int maximum) {
    const int count = lua_gettop(L);
    if (count < minimum || count > maximum)
        throw std::invalid_argument("wrong Debug argument count");
}

std::string DialogText(lua_State* L, int index) {
    const auto text = CheckString(L, index);
    if (text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Debug dialog strings cannot contain embedded NUL bytes");
    return std::string(text);
}

Native::DialogType Type(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return Native::DialogType::Info;
    const auto type = CheckString(L, index);
    if (type == "info") return Native::DialogType::Info;
    if (type == "warning") return Native::DialogType::Warning;
    if (type == "error") return Native::DialogType::Error;
    throw std::invalid_argument("Debug dialog type must be info, warning or error");
}

int Log(lua_State* L) {
    Count(L, 1, 1);
    core::runtime::Log::Info(CheckString(L, 1));
    return 0;
}

int Warn(lua_State* L) {
    Count(L, 1, 1);
    core::runtime::Log::Warn(CheckString(L, 1));
    return 0;
}

int SetConsoleVisible(lua_State* L) {
    Count(L, 1, 1);
    if (lua_type(L, 1) != LUA_TBOOLEAN)
        throw std::invalid_argument("Debug.SetConsoleVisible requires a boolean");
    Native::SetConsoleVisible(lua_toboolean(L, 1) != 0);
    return 0;
}

int IsConsoleVisible(lua_State* L) {
    Count(L, 0, 0);
    lua_pushboolean(L, Native::IsConsoleVisible());
    return 1;
}

int RecordError(lua_State* L) {
    Count(L, 1, 1);
    const auto message = CheckString(L, 1);
    // A logging failure cannot replace or suppress the requested Lua error.
    try { core::runtime::Log::Error(message); } catch (...) {}
    return 0;
}

int Error(lua_State* L) {
    char diagnostic[4096]{};
    if (TryFunction<RecordError>(L, diagnostic, sizeof(diagnostic)) < 0)
        lua_pushstring(L, diagnostic);
    else
        lua_pushvalue(L, 1); // Preserve the original string, including length/NUL.
    // All C++ exception scopes have ended before LuaJIT unwinds to its caller.
    return lua_error(L);
}

int MessageBox(lua_State* L) {
    Count(L, 1, 3);
    Native::Dialog dialog;
    dialog.message = DialogText(L, 1);
    dialog.title = lua_isnoneornil(L, 2) ? "提示" : DialogText(L, 2);
    dialog.type = Type(L, 3);
    dialog.buttons.emplace_back("确定");
    if (Native::ShowDialog(dialog) == -1) lua_pushnil(L);
    else lua_pushboolean(L, true);
    return 1;
}

std::vector<std::string> Buttons(lua_State* L) {
    if (lua_type(L, 3) != LUA_TTABLE)
        throw std::invalid_argument("Debug.ButtonBox buttons must be a sequential string array");
    // Inspect raw keys rather than trusting Lua's # operator: a sparse table
    // has no reliable length, and __index must not manufacture missing labels.
    std::size_t count = 0;
    std::size_t largest = 0;
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
        if (lua_type(L, -2) != LUA_TNUMBER)
            throw std::invalid_argument("Debug.ButtonBox buttons must only have positive integer keys");
        const double key = lua_tonumber(L, -2);
        if (!std::isfinite(key) || key < 1 || key > std::numeric_limits<int>::max() || key != std::floor(key))
            throw std::invalid_argument("Debug.ButtonBox buttons must only have positive integer keys");
        largest = std::max(largest, static_cast<std::size_t>(key));
        ++count;
        lua_pop(L, 1);
    }
    if (count == 0 || count != largest)
        throw std::invalid_argument("Debug.ButtonBox buttons must be a nonempty contiguous array");

    std::vector<std::string> buttons;
    std::unordered_set<std::string> unique;
    buttons.reserve(count);
    for (std::size_t index = 1; index <= count; ++index) {
        lua_rawgeti(L, 3, static_cast<int>(index));
        auto text = DialogText(L, -1);
        lua_pop(L, 1);
        if (text.empty() || !unique.insert(text).second)
            throw std::invalid_argument("Debug.ButtonBox button labels must be nonempty and unique");
        buttons.push_back(std::move(text));
    }
    return buttons;
}

int ButtonBox(lua_State* L) {
    Count(L, 3, 4);
    Native::Dialog dialog;
    dialog.message = DialogText(L, 1);
    dialog.title = DialogText(L, 2);
    dialog.type = Type(L, 4);
    dialog.buttons = Buttons(L);
    const int selected = Native::ShowDialog(dialog);
    if (selected == -1) lua_pushnil(L);
    else {
        const auto& label = dialog.buttons.at(static_cast<std::size_t>(selected));
        lua_pushlstring(L, label.data(), label.size());
    }
    return 1;
}
}

void Register(lua_State* L) {
    const luaL_Reg functions[] = {
        {"Log", Guard<Log>}, {"Warn", Guard<Warn>}, {"Error", Error},
        {"MessageBox", Guard<MessageBox>}, {"ButtonBox", Guard<ButtonBox>},
        {"SetConsoleVisible", Guard<SetConsoleVisible>}, {"IsConsoleVisible", Guard<IsConsoleVisible>},
        {nullptr, nullptr}
    };
    luaL_openlib(L, "Debug", functions, 0);
    lua_pop(L, 1);
}
}
