#include "Text.h"
#include "core/modules/Text/Text.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <limits>
#include <unordered_set>

namespace lua_bind::modules::Text {
namespace {
namespace Native = core::modules::Text;
using Value = Native::JsonValue;

void Count(lua_State* L, int minimum, int maximum) {
    if (lua_gettop(L) < minimum || lua_gettop(L) > maximum) throw std::invalid_argument("wrong Text argument count");
}
int Absolute(lua_State* L, int index) { return index < 0 && index > LUA_REGISTRYINDEX ? lua_gettop(L) + index + 1 : index; }

void Push(lua_State* L, const Value& value, int depth = 0) {
    if (depth > Native::maximum_json_depth || !lua_checkstack(L, 8)) throw std::invalid_argument("JSON nesting exceeds Lua stack limit");
    if (value.is_null()) lua_pushnil(L);
    else if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
    else if (value.is_number()) lua_pushnumber(L, value.get<double>());
    else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(L, text.data(), text.size());
    } else if (value.is_array()) {
        if (value.size() > std::size_t(std::numeric_limits<int>::max())) throw std::invalid_argument("JSON array too large for Lua");
        lua_createtable(L, static_cast<int>(value.size()), 0);
        for (std::size_t i = 0; i < value.size(); ++i) {
            Push(L, value[i], depth + 1);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    } else if (value.is_object()) {
        if (value.size() > std::size_t(std::numeric_limits<int>::max())) throw std::invalid_argument("JSON object too large for Lua");
        lua_createtable(L, 0, static_cast<int>(value.size()));
        for (const auto& child : value.items()) {
            lua_pushlstring(L, child.key().data(), child.key().size());
            Push(L, child.value(), depth + 1);
            lua_rawset(L, -3);
        }
    } else throw std::invalid_argument("unsupported JSON value");
}

struct TableReader {
    std::unordered_set<const void*> active;
    Value Read(lua_State* L, int index, int depth = 0) {
        if (depth > Native::maximum_json_depth || !lua_checkstack(L, 8)) throw std::invalid_argument("JSON nesting exceeds Lua stack limit");
        index = Absolute(L, index);
        switch (lua_type(L, index)) {
        case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
        case LUA_TNUMBER: {
            const auto value = lua_tonumber(L, index);
            if (!std::isfinite(value)) throw std::invalid_argument("JSON number must be finite");
            return value;
        }
        case LUA_TSTRING: return std::string(CheckString(L, index));
        case LUA_TTABLE: {
            const auto identity = lua_topointer(L, index);
            if (!active.insert(identity).second) throw std::invalid_argument("cannot encode cyclic Lua table");
            std::size_t count = 0;
            double largest = 0;
            bool numeric = true, strings = true;
            // Raw traversal deliberately ignores __index/__pairs: metatables
            // cannot inject data, call Lua, or affect the JSON representation.
            lua_pushnil(L);
            while (lua_next(L, index)) {
                ++count;
                if (lua_type(L, -2) == LUA_TNUMBER) {
                    const double key = lua_tonumber(L, -2);
                    if (!std::isfinite(key) || key < 1 || key > std::numeric_limits<int>::max() || key != std::floor(key)) numeric = false;
                    largest = std::max(largest, key);
                    strings = false;
                } else {
                    numeric = false;
                    if (lua_type(L, -2) != LUA_TSTRING) strings = false;
                }
                lua_pop(L, 1);
            }
            const bool array = count > 0 && numeric;
            auto result = array ? Value::array() : Value::object();
            if (array) {
                if (largest != static_cast<double>(count)) throw std::invalid_argument("JSON array must have dense integer keys 1..n");
                for (std::size_t i = 1; i <= count; ++i) {
                    lua_rawgeti(L, index, static_cast<int>(i));
                    result.push_back(Read(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
            } else {
                if (!strings) throw std::invalid_argument("JSON table needs only string keys or dense integer keys 1..n");
                lua_pushnil(L);
                while (lua_next(L, index)) {
                    const std::string key(CheckString(L, -2));
                    result[key] = Read(L, -1, depth + 1);
                    lua_pop(L, 1);
                }
            }
            active.erase(identity);
            return result;
        }
        default: throw std::invalid_argument(std::string("cannot encode Lua ") + lua_typename(L, lua_type(L, index)));
        }
    }
};

int Json2Table(lua_State* L) {
    Count(L, 1, 1);
    const auto value = Native::DecodeJson(CheckString(L, 1));
    if (!value.is_structured()) throw std::invalid_argument("Json2Table root must be a JSON object or array");
    Push(L, value);
    return 1;
}
int Table2Json(lua_State* L) {
    Count(L, 1, 1);
    if (lua_type(L, 1) != LUA_TTABLE) throw std::invalid_argument("Table2Json requires a table");
    TableReader reader;
    const auto text = Native::EncodeJson(reader.Read(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FormatTime(lua_State* L) {
    Count(L, 1, 2);
    if (lua_type(L, 1) != LUA_TNUMBER) throw std::invalid_argument("duration must be a number");
    const auto text = Native::FormatTime(lua_tonumber(L, 1), lua_isnoneornil(L, 2) ? "mm:ss" : CheckString(L, 2));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
}

void Register(lua_State* L) {
    const luaL_Reg functions[] = {
        {"Json2Table", Guard<Json2Table>}, {"Table2Json", Guard<Table2Json>},
        {"FormatTime", Guard<FormatTime>}, {nullptr, nullptr}};
    luaL_register(L, "Text", functions);
    lua_pop(L, 1);
}
}
