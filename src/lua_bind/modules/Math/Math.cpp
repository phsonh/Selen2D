#include "Math.h"
#include "core/modules/Math/Math.h"
#include "lua_bind/Guard.h"

#include <stdexcept>
#include <string>

namespace lua_bind::modules::Math {
    namespace {
        namespace Native = core::modules::Math;

        void Arity(lua_State* L, int count, const char* name) {
            if (lua_gettop(L) != count)
                throw std::invalid_argument(std::string(name) + " expects exactly " + std::to_string(count) + " arguments");
        }

        double Number(lua_State* L, int index) {
            if (lua_type(L, index) != LUA_TNUMBER)
                throw std::invalid_argument("argument #" + std::to_string(index) + " must be a number");
            return lua_tonumber(L, index);
        }

        template<double (*Function)(double)>
        int Unary(lua_State* L) {
            Arity(L, 1, "Math trigonometric function");
            lua_pushnumber(L, Function(Number(L, 1)));
            return 1;
        }

        int Atan2(lua_State* L) {
            Arity(L, 2, "Math.atan2");
            lua_pushnumber(L, Native::atan2(Number(L, 1), Number(L, 2)));
            return 1;
        }

        int Seed(lua_State* L) {
            Arity(L, 1, "Math.Random.Seed");
            Native::Random::Seed(Number(L, 1));
            return 0;
        }

        int Int(lua_State* L) {
            Arity(L, 2, "Math.Random.Int");
            // lua_Integer may be narrower than Lua's exact double-integer range.
            lua_pushnumber(L, static_cast<lua_Number>(Native::Random::Int(Number(L, 1), Number(L, 2))));
            return 1;
        }

        int Float(lua_State* L) {
            Arity(L, 2, "Math.Random.Float");
            lua_pushnumber(L, Native::Random::Float(Number(L, 1), Number(L, 2)));
            return 1;
        }

        int Sign(lua_State* L) {
            Arity(L, 0, "Math.Random.Sign");
            lua_pushinteger(L, Native::Random::Sign());
            return 1;
        }

        const luaL_Reg Functions[] = {
            {"sin", Guard<Unary<Native::sin>>}, {"cos", Guard<Unary<Native::cos>>},
            {"tan", Guard<Unary<Native::tan>>}, {"asin", Guard<Unary<Native::asin>>},
            {"acos", Guard<Unary<Native::acos>>}, {"atan2", Guard<Atan2>},
            {nullptr, nullptr}
        };
        const luaL_Reg RandomFunctions[] = {
            {"Seed", Guard<Seed>}, {"Int", Guard<Int>},
            {"Float", Guard<Float>}, {"Sign", Guard<Sign>},
            {nullptr, nullptr}
        };
    }

    void Register(lua_State* L) {
        luaL_register(L, "Math", Functions);
        luaL_register(L, "Math.Random", RandomFunctions);
        lua_pop(L, 2);
    }
}
